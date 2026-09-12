#include "gpuscene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>

#include <osg/BufferObject>
#include <osg/Callback>
#include <osg/Drawable>
#include <osg/GLDefines>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/GraphicsThread>
#include <osg/LOD>
#include <osg/PrimitiveSet>
#include <osg/Sequence>
#include <osg/State>
#include <osg/Switch>
#include <osg/Texture>
#include <osg/VertexArrayState>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/nifosg/autotransform.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include "renderbin.hpp"
#include "vismask.hpp"

#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif

namespace MWRender
{
    class GpuCaptureCallback : public osg::DrawableCullCallback
    {
    public:
        GpuCaptureCallback(GpuScene& scene, osg::Callback* previous, bool updateMesh)
            : mScene(&scene)
            , mPrevious(previous)
            , mUpdateMesh(updateMesh)
        {
        }

        bool cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* renderInfo) const override
        {
            if (mPrevious != nullptr)
            {
                const osg::DrawableCullCallback* callback = mPrevious->asDrawableCullCallback();
                if (callback != nullptr && callback->cull(nv, drawable, renderInfo))
                    return true;
            }
            osgUtil::CullVisitor* cv = nv->asCullVisitor();
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene == nullptr || !scene->isEnabled() || cv == nullptr)
                return false;
            if (drawable->isCullingActive() && cv->isCulled(drawable->getBoundingBox()))
                return true;
            if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable))
            {
                if (!scene->recordRig(*cv, *rig, *this))
                    return false;
                if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR
                    && rig->getBoundingBox().valid())
                    cv->updateCalculatedNearFar(*cv->getModelViewMatrix(), *rig);
                return true;
            }
            osg::Geometry* geometry = drawable->asGeometry();
            if (geometry == nullptr || !scene->record(*cv, *geometry, mUpdateMesh, *this))
                return false;
            if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR
                && geometry->getBoundingBox().valid())
                cv->updateCalculatedNearFar(*cv->getModelViewMatrix(), *geometry);
            return true;
        }

        void detach() { mScene.store(nullptr, std::memory_order_release); }
        void resetPath() const { mLiveMaterial.store(-1, std::memory_order_relaxed); }
        const osg::Callback* getWrapped() const { return mPrevious.get(); }

    private:
        friend class GpuScene;

        struct Cache
        {
            const osg::Geometry* mGeometry = nullptr;
            std::vector<osg::ref_ptr<const osg::StateSet>> mPath;
            GpuScene::Id mMesh = GpuScene::sInvalid;
            GpuScene::Surface mSurface;
        };

        std::pair<GpuScene::Id, GpuScene::Surface> resolveLocked(osg::Geometry& geometry,
            const std::vector<const osg::StateSet*>& path, unsigned int frame, bool liveMaterial) const;
        bool hasLiveMaterial(const osgUtil::CullVisitor& cv) const;

        std::atomic<GpuScene*> mScene;
        osg::ref_ptr<osg::Callback> mPrevious;
        bool mUpdateMesh;
        mutable std::array<Cache, 4> mCache;
        mutable unsigned int mNextCache = 0;
        mutable std::atomic<int> mLiveMaterial{ -1 };
    };

    namespace
    {
        struct GpuVertex
        {
            float mPosition[3];
            float mNormal[3];
            unsigned char mColor[4];
            float mUv[7][2];
            float mTexCoord7[4];
            float mPad[3];
        };
        static_assert(sizeof(GpuVertex) == 112);

        constexpr const char* sMaterialUniforms[] = { "material.diffuse", "material.ambient", "material.specular",
            "material.emission", "material.shininess", "material.emissiveMult", "material.specStrength",
            "material.vertexColorMode", "alphaRef", "envMapColor", "envMapLumaBias", "bumpMapMatrix", "texMat0",
            "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6", "texMat7", "texMat8", "texMat9" };
        constexpr const char* sTexMatNames[]
            = { "texMat0", "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6", "texMat7" };
        constexpr unsigned int sLinksPerFrame = 4;
        constexpr std::size_t sUploadsPerFrame = 200;

        struct GpuCommand
        {
            std::uint32_t mCount;
            std::uint32_t mInstanceCount;
            std::uint32_t mFirstIndex;
            std::int32_t mBaseVertex;
            std::uint32_t mBaseInstance;
        };
        // Mirrors GL draw-indirect commands.
        static_assert(sizeof(GpuCommand) == 20);
        static_assert(offsetof(GpuCommand, mBaseInstance) == 16);

        struct GpuSkinTask
        {
            std::uint32_t mVertexOffset;
            std::uint32_t mVertexCount;
            std::uint32_t mMatrixOffset;
            std::uint32_t mFlags;
        };

        constexpr unsigned int sFlagSmallFeature = 1;
        constexpr unsigned int sFlagLod = 2;
        constexpr unsigned int sWorkGroupSize = 256;
        constexpr unsigned int sSkinGroupSize = 64;
        constexpr unsigned int sMaxPlanes = 16;

        struct CullPath
        {
            const GpuScene* mScene = nullptr;
            unsigned int mFrame = 0;
            const osg::Camera* mCamera = nullptr;
            std::vector<const osg::StateSet*> mStates;
        };

        thread_local CullPath sCullPath;

        using ClearBufferDataFn = void(GL_APIENTRY*)(GLenum, GLenum, GLenum, GLenum, const void*);
        using CopyBufferSubDataFn = void(GL_APIENTRY*)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);

        bool appendTriangles(const osg::PrimitiveSet& primitives, std::vector<std::uint32_t>& indices)
        {
            if (dynamic_cast<const osg::DrawArrayLengths*>(&primitives) != nullptr)
                return false;
            const unsigned int count = primitives.getNumIndices();
            switch (primitives.getMode())
            {
                case GL_TRIANGLES:
                    for (unsigned int i = 0; i + 2 < count; i += 3)
                    {
                        indices.push_back(primitives.index(i));
                        indices.push_back(primitives.index(i + 1));
                        indices.push_back(primitives.index(i + 2));
                    }
                    return true;
                case GL_QUADS:
                    for (unsigned int i = 0; i + 3 < count; i += 4)
                    {
                        indices.push_back(primitives.index(i));
                        indices.push_back(primitives.index(i + 1));
                        indices.push_back(primitives.index(i + 2));
                        indices.push_back(primitives.index(i));
                        indices.push_back(primitives.index(i + 2));
                        indices.push_back(primitives.index(i + 3));
                    }
                    return true;
                case GL_TRIANGLE_STRIP:
                    for (unsigned int i = 2; i < count; ++i)
                    {
                        const unsigned int a = (i % 2 == 0) ? i - 2 : i - 1;
                        const unsigned int b = (i % 2 == 0) ? i - 1 : i - 2;
                        indices.push_back(primitives.index(a));
                        indices.push_back(primitives.index(b));
                        indices.push_back(primitives.index(i));
                    }
                    return true;
                case GL_TRIANGLE_FAN:
                    for (unsigned int i = 2; i < count; ++i)
                    {
                        indices.push_back(primitives.index(0));
                        indices.push_back(primitives.index(i - 1));
                        indices.push_back(primitives.index(i));
                    }
                    return true;
                default:
                    return false;
            }
        }

        template <class Array>
        const Array* perVertexArray(const osg::Array* array, std::size_t vertices)
        {
            const Array* typed = dynamic_cast<const Array*>(array);
            if (typed == nullptr || typed->getBinding() != osg::Array::BIND_PER_VERTEX || typed->size() != vertices)
                return nullptr;
            return typed;
        }

        bool packGeometry(
            const osg::Geometry& geometry, std::vector<std::uint8_t>& vertexBytes, std::vector<std::uint32_t>& indices)
        {
            const osg::Vec3Array* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (positions == nullptr || positions->empty())
                return false;
            const std::size_t count = positions->size();
            const osg::Vec3Array* normals = perVertexArray<osg::Vec3Array>(geometry.getNormalArray(), count);
            osg::Vec3f overallNormal(0.f, 0.f, 1.f);
            if (geometry.getNormalArray() != nullptr && normals == nullptr)
            {
                const auto* overall = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
                if (overall == nullptr || overall->getBinding() != osg::Array::BIND_OVERALL || overall->size() != 1)
                    return false;
                overallNormal = (*overall)[0];
            }

            const osg::Array* colorArray = geometry.getColorArray();
            const osg::Vec4Array* colors = nullptr;
            const osg::Vec4ubArray* colorsUb = nullptr;
            osg::Vec4f overallColor(1.f, 1.f, 1.f, 1.f);
            if (colorArray != nullptr)
            {
                if (colorArray->getBinding() == osg::Array::BIND_OVERALL && colorArray->getNumElements() == 1)
                {
                    if (const auto* c = dynamic_cast<const osg::Vec4Array*>(colorArray))
                        overallColor = (*c)[0];
                    else if (const auto* cub = dynamic_cast<const osg::Vec4ubArray*>(colorArray))
                        overallColor = osg::Vec4f(
                            (*cub)[0].r() / 255.f, (*cub)[0].g() / 255.f, (*cub)[0].b() / 255.f, (*cub)[0].a() / 255.f);
                    else
                        return false;
                }
                else
                {
                    colors = perVertexArray<osg::Vec4Array>(colorArray, count);
                    colorsUb = perVertexArray<osg::Vec4ubArray>(colorArray, count);
                    if (colors == nullptr && colorsUb == nullptr)
                        return false;
                }
            }

            const osg::Vec2Array* uv[8] = {};
            const osg::Vec4Array* tangents = nullptr;
            for (unsigned int unit = 0; unit < geometry.getNumTexCoordArrays(); ++unit)
            {
                const osg::Array* array = geometry.getTexCoordArray(unit);
                if (array == nullptr)
                    continue;
                if (unit < 7)
                {
                    uv[unit] = perVertexArray<osg::Vec2Array>(array, count);
                    if (uv[unit] == nullptr)
                        return false;
                }
                else if (unit == 7)
                {
                    tangents = perVertexArray<osg::Vec4Array>(array, count);
                    if (tangents == nullptr)
                    {
                        uv[unit] = perVertexArray<osg::Vec2Array>(array, count);
                        if (uv[unit] == nullptr)
                            return false;
                    }
                }
                else
                    return false;
            }
            for (unsigned int i = 0; i < geometry.getNumVertexAttribArrays(); ++i)
                if (geometry.getVertexAttribArray(i) != nullptr)
                    return false;

            for (unsigned int i = 0; i < geometry.getNumPrimitiveSets(); ++i)
                if (!appendTriangles(*geometry.getPrimitiveSet(i), indices))
                    return false;
            if (indices.empty())
                return false;

            vertexBytes.resize(count * sizeof(GpuVertex));
            GpuVertex* out = reinterpret_cast<GpuVertex*>(vertexBytes.data());
            for (std::size_t i = 0; i < count; ++i)
            {
                GpuVertex& v = out[i];
                std::memset(&v, 0, sizeof(GpuVertex));
                const osg::Vec3f& p = (*positions)[i];
                const osg::Vec3f n = normals != nullptr ? (*normals)[i] : overallNormal;
                v.mPosition[0] = p.x();
                v.mPosition[1] = p.y();
                v.mPosition[2] = p.z();
                v.mNormal[0] = n.x();
                v.mNormal[1] = n.y();
                v.mNormal[2] = n.z();
                osg::Vec4f c = overallColor;
                if (colors != nullptr)
                    c = (*colors)[i];
                else if (colorsUb != nullptr)
                {
                    const osg::Vec4ub& cub = (*colorsUb)[i];
                    c = osg::Vec4f(cub.r() / 255.f, cub.g() / 255.f, cub.b() / 255.f, cub.a() / 255.f);
                }
                for (int k = 0; k < 4; ++k)
                    v.mColor[k] = static_cast<unsigned char>(std::clamp(c[k], 0.f, 1.f) * 255.f + 0.5f);
                for (int unit = 0; unit < 7; ++unit)
                {
                    if (uv[unit] == nullptr)
                        continue;
                    const osg::Vec2f& t = (*uv[unit])[i];
                    v.mUv[unit][0] = t.x();
                    v.mUv[unit][1] = t.y();
                }
                if (tangents != nullptr)
                {
                    const osg::Vec4f& t = (*tangents)[i];
                    for (int k = 0; k < 4; ++k)
                        v.mTexCoord7[k] = t[k];
                }
                else if (uv[7] != nullptr)
                {
                    const osg::Vec2f& t = (*uv[7])[i];
                    v.mTexCoord7[0] = t.x();
                    v.mTexCoord7[1] = t.y();
                }
            }
            return true;
        }

        template <class T>
        T uniformValue(const osg::StateSet& stateSet, const char* name, const T& fallback)
        {
            T value = fallback;
            if (const osg::Uniform* uniform = stateSet.getUniform(name))
                uniform->get(value);
            return value;
        }

        template <class Pair, class Find>
        const Pair* resolvePair(const std::vector<const osg::StateSet*>& path, Find&& find)
        {
            const Pair* result = nullptr;
            for (const osg::StateSet* stateSet : path)
            {
                const Pair* incoming = find(*stateSet);
                if (incoming == nullptr)
                    continue;
                if (result == nullptr || !(result->second & osg::StateAttribute::OVERRIDE)
                    || (incoming->second & osg::StateAttribute::PROTECTED))
                    result = incoming;
            }
            return result;
        }

        template <class T>
        T uniformValue(const osg::StateSet::RefUniformPair* pair, const T& fallback)
        {
            T value = fallback;
            if (pair != nullptr)
                pair->first->get(value);
            return value;
        }

        void put(float* out, const osg::Vec4f& v)
        {
            for (int i = 0; i < 4; ++i)
                out[i] = v[i];
        }

        bool hasUpdater(const osg::Callback* callback)
        {
            for (; callback != nullptr; callback = callback->getNestedCallback())
                if (dynamic_cast<const SceneUtil::StateSetUpdater*>(callback) != nullptr)
                    return true;
            return false;
        }

        using LODRange = osg::LOD::MinMaxPair;

        LODRange intersection(const LODRange& left, const LODRange& right)
        {
            return { std::max(left.first, right.first), std::min(left.second, right.second) };
        }

        bool empty(const LODRange& r)
        {
            return r.first >= r.second;
        }

        float uniformScale(const osg::Matrixf& matrix)
        {
            const float x = osg::Matrixf::transform3x3(osg::Vec3f(1, 0, 0), matrix).length();
            const float y = osg::Matrixf::transform3x3(osg::Vec3f(0, 1, 0), matrix).length();
            const float z = osg::Matrixf::transform3x3(osg::Vec3f(0, 0, 1), matrix).length();
            const float maxScale = std::max({ x, y, z });
            const float minScale = std::min({ x, y, z });
            if (maxScale <= 0.f || (maxScale - minScale) > maxScale * 1e-3f)
                return -1.f;
            return maxScale;
        }

        const osg::Callback* foreignCullCallback(const osg::Drawable& drawable)
        {
            const osg::Callback* callback = drawable.getCullCallback();
            if (const auto* capture = dynamic_cast<const GpuCaptureCallback*>(callback))
                return capture->getWrapped();
            return callback;
        }

        bool orderDependentPath(const std::vector<const osg::StateSet*>& statePath)
        {
            bool ordered = false;
            bool blended = false;
            for (const osg::StateSet* state : statePath)
            {
                if (state->useRenderBinDetails())
                {
                    const std::string& name = state->getBinName();
                    ordered = state->getBinNumber() == RenderBin_DepthSorted || name == "DepthSortedBin"
                        || name == "SORT_BACK_TO_FRONT" || name == "TraversalOrderBin";
                }
                const osg::StateAttribute::GLModeValue blend = state->getMode(GL_BLEND);
                if (blend != osg::StateAttribute::INHERIT)
                    blended = (blend & osg::StateAttribute::ON) != 0;
            }
            return ordered || blended;
        }

        class GpuExtractor
        {
        public:
            GpuExtractor(GpuScene& scene, osg::Node::NodeMask mask, bool bakeBillboards, GpuTemplate& out)
                : mScene(scene)
                , mMask(mask)
                , mBakeBillboards(bakeBillboards)
                , mOut(out)
            {
            }

            void visit(const osg::Node* node, const osg::Matrixf& matrix, const osg::LOD* lod, unsigned int lodChild,
                const osg::Vec3f& lodCenter, float lodScale, bool root = false)
            {
                if (!(node->getNodeMask() & mMask))
                    return;
                if (dynamic_cast<const osgParticle::ParticleProcessor*>(node) != nullptr
                    || dynamic_cast<const osgParticle::ParticleSystemUpdater*>(node) != nullptr)
                    return;
                const bool trackedSwitch = node->asSwitch() != nullptr && node->getName() == Constants::NightDayLabel;
                // Paged copies discard node update callbacks.
                if ((!mBakeBillboards && !root && node->getUpdateCallback() != nullptr && !trackedSwitch)
                    || (node->getStateSet() != nullptr && node->getStateSet()->requiresUpdateTraversal()))
                {
                    mOut.mLegacy = true;
                    return;
                }
                const osg::Callback* cullCallback = node->asDrawable() != nullptr
                    ? foreignCullCallback(*node->asDrawable())
                    : node->getCullCallback();
                if (cullCallback != nullptr && !root)
                {
                    mOut.mLegacy = true;
                    return;
                }

                const std::size_t depth = mStates.size();
                if (node->getStateSet() != nullptr)
                    mStates.push_back(node->getStateSet());

                if (const osg::Drawable* drawable = node->asDrawable())
                    visitDrawable(drawable, matrix, lod, lodChild, lodCenter, lodScale);
                else if (const osg::Switch* sw = node->asSwitch())
                {
                    for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                        if (sw->getValue(i))
                            visit(sw->getChild(i), matrix, lod, lodChild, lodCenter, lodScale);
                }
                else if (const osg::LOD* nested = dynamic_cast<const osg::LOD*>(node))
                {
                    const float scale = uniformScale(matrix);
                    if (lod != nullptr || nested->getRangeMode() != osg::LOD::DISTANCE_FROM_EYE_POINT || scale < 0.f)
                        mOut.mLegacy = true;
                    else
                    {
                        const osg::Vec3f center = nested->getCenter() * matrix;
                        for (unsigned int i = 0; i < nested->getNumChildren(); ++i)
                            visit(nested->getChild(i), matrix, nested, i, center, scale);
                    }
                }
                else if (const osg::Sequence* sq = dynamic_cast<const osg::Sequence*>(node))
                    visit(sq->getChild(sq->getValue() != -1 ? sq->getValue() : 0), matrix, lod, lodChild, lodCenter,
                        lodScale);
                else if (const osg::Transform* transform = node->asTransform())
                {
                    const auto* autoTransform = dynamic_cast<const NifOsg::AutoTransform*>(node);
                    if (autoTransform != nullptr && !mBakeBillboards)
                        mOut.mLegacy = true;
                    else
                    {
                        osg::Matrix local(matrix);
                        if (autoTransform != nullptr)
                            local.preMult(autoTransform->computeMatrix(nullptr));
                        else
                            transform->computeLocalToWorldMatrix(local, nullptr);
                        const osg::Matrixf localf(local);
                        for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                            visit(transform->getChild(i), localf, lod, lodChild, lodCenter, lodScale);
                    }
                }
                else if (const osg::Group* group = node->asGroup())
                {
                    for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                        visit(group->getChild(i), matrix, lod, lodChild, lodCenter, lodScale);
                }
                else
                    mOut.mLegacy = true;

                mStates.resize(depth);
            }

        private:
            void visitDrawable(const osg::Drawable* drawable, const osg::Matrixf& matrix, const osg::LOD* lod,
                unsigned int lodChild, const osg::Vec3f& lodCenter, float lodScale)
            {
                if (dynamic_cast<const osgParticle::ParticleSystem*>(drawable) != nullptr
                    || dynamic_cast<const SceneUtil::OsgaRigGeometry*>(drawable) != nullptr)
                    return;
                const osg::Geometry* geometry = nullptr;
                if (const auto* rig = dynamic_cast<const SceneUtil::RigGeometry*>(drawable))
                    geometry = rig->getSourceGeometry();
                else if (const auto* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(drawable))
                    geometry = morph->getSourceGeometry();
                else
                    geometry = drawable->asGeometry();

                const float scale = geometry != nullptr ? uniformScale(matrix) : -1.f;
                if (geometry == nullptr || scale < 0.f || foreignCullCallback(*geometry) != nullptr)
                {
                    mOut.mLegacy = true;
                    return;
                }

                const std::size_t depth = mStates.size();
                if (geometry != drawable && geometry->getStateSet() != nullptr)
                    mStates.push_back(geometry->getStateSet());
                if (orderDependentPath(mStates))
                {
                    mOut.mLegacy = true;
                    return;
                }
                const GpuScene::Id mesh = mScene.getMesh(*geometry);
                const GpuScene::Surface surface
                    = mesh != GpuScene::sInvalid ? mScene.getSurface(mStates) : GpuScene::Surface();
                mStates.resize(depth);
                if (mesh == GpuScene::sInvalid || surface.mBucket == GpuScene::sInvalid)
                {
                    mOut.mLegacy = true;
                    return;
                }

                mOut.mClaimed.insert(drawable);
                mOut.mGeometries.push_back(GpuGeometry{ geometry, surface, matrix, geometry->getBoundingBox(), lod,
                    lodChild, lodCenter, lodScale, drawable });
            }

            GpuScene& mScene;
            osg::Node::NodeMask mMask;
            bool mBakeBillboards;
            GpuTemplate& mOut;
            std::vector<const osg::StateSet*> mStates;
        };

        class CaptureVisitor : public osg::NodeVisitor
        {
        public:
            explicit CaptureVisitor(osg::Node* skip)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mSkip(skip)
            {
            }

            void apply(osg::Node& node) override
            {
                if (&node != mSkip)
                    traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mRigs.push_back(rig);
                else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    mMorphs.push_back(morph);
                else if (osg::Geometry* geometry = drawable.asGeometry())
                    mGeometries.push_back(geometry);
            }

            std::vector<osg::Geometry*> mGeometries;
            std::vector<SceneUtil::RigGeometry*> mRigs;
            std::vector<SceneUtil::MorphGeometry*> mMorphs;

        private:
            osg::Node* mSkip;
        };
    }

    class GpuSceneNode : public osg::Group
    {
    public:
        GpuSceneNode() = default;
        GpuSceneNode(const GpuSceneNode& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Group(copy, copyop)
            , mScene(copy.mScene.load(std::memory_order_relaxed))
        {
        }
        META_Node(MWRender, GpuSceneNode)

        explicit GpuSceneNode(GpuScene* scene)
            : mScene(scene)
        {
            setUpdateCallback(new UpdateCallback);
        }

        void setScene(GpuScene* scene) { mScene.store(scene, std::memory_order_release); }

        void traverse(osg::NodeVisitor& nv) override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene != nullptr && nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
                scene->recordView(static_cast<osgUtil::CullVisitor&>(nv));
            osg::Group::traverse(nv);
        }

        osg::BoundingSphere computeBound() const override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            return scene != nullptr ? scene->getResidentBound() : osg::BoundingSphere();
        }

    private:
        struct UpdateCallback : osg::NodeCallback
        {
            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                GpuSceneNode& self = static_cast<GpuSceneNode&>(*node);
                if (GpuScene* const scene = self.mScene.load(std::memory_order_acquire))
                    scene->update();
                traverse(node, nv);
            }
        };

        std::atomic<GpuScene*> mScene{ nullptr };
    };

    class GpuChunkNode : public osg::Node
    {
    public:
        GpuChunkNode() = default;
        GpuChunkNode(const GpuChunkNode& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Node(copy, copyop)
            , mScene(nullptr)
            , mBound(copy.mBound)
        {
        }
        META_Node(MWRender, GpuChunkNode)

        GpuChunkNode(GpuScene& scene, GpuScene::Id slot, GpuScene::Id generation, const osg::BoundingSphere& bound)
            : mScene(&scene)
            , mSlot(slot)
            , mGeneration(generation)
            , mBound(bound)
        {
        }

        ~GpuChunkNode() override
        {
            if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                scene->releaseChunk(*this, mSlot);
        }

        void detach() { mScene.store(nullptr, std::memory_order_release); }
        GpuScene::Id getSlot() const { return mSlot; }

        void traverse(osg::NodeVisitor& nv) override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene == nullptr || nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
                return;
            scene->markChunk(static_cast<osgUtil::CullVisitor&>(nv), mSlot, mGeneration);
        }

        osg::BoundingSphere computeBound() const override { return mBound; }

    private:
        std::atomic<GpuScene*> mScene{ nullptr };
        GpuScene::Id mSlot = 0;
        GpuScene::Id mGeneration = 0;
        osg::BoundingSphere mBound;
    };

    namespace
    {
        class DispatchDrawable : public osg::Drawable
        {
        public:
            DispatchDrawable() = default;
            DispatchDrawable(const DispatchDrawable& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
                : osg::Drawable(copy, copyop)
                , mScene(copy.mScene.load(std::memory_order_relaxed))
                , mDispatcher(copy.mDispatcher)
            {
            }
            META_Node(MWRender, DispatchDrawable)

            DispatchDrawable(GpuScene& scene, GpuScene::Id dispatcher)
                : mScene(&scene)
                , mDispatcher(dispatcher)
            {
                setCullingActive(false);
                setSupportsDisplayList(false);
                setUseDisplayList(false);
                setUseVertexBufferObjects(true);
                setUseVertexArrayObject(false);
            }

            osg::BoundingBox computeBoundingBox() const override { return osg::BoundingBox(); }

            void drawImplementation(osg::RenderInfo& renderInfo) const override
            {
                if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                    scene->draw(renderInfo, mDispatcher);
            }

            void detach() { mScene.store(nullptr, std::memory_order_release); }

        private:
            std::atomic<GpuScene*> mScene{ nullptr };
            GpuScene::Id mDispatcher = 0;
        };
    }

    struct GpuScene::Gl
    {
        osg::GLExtensions* mExt = nullptr;
        ClearBufferDataFn mClearBufferData = nullptr;
        CopyBufferSubDataFn mCopyBufferSubData = nullptr;
        GLuint mVao = 0;
        GLuint mDynamicVao = 0;
        GLuint mVertexBuffer = 0;
        GLuint mIndexBuffer = 0;
        GLuint mInstanceBuffer = 0;
        GLuint mBatchBuffer = 0;
        GLuint mChunkBuffer = 0;
        GLuint mCountBuffer = 0;
        GLuint mVisibleBuffer = 0;
        GLuint mCommandBuffer = 0;
        GLuint mMaterialBuffer = 0;
        GLuint mDynamicInstanceBuffer = 0;
        GLuint mDynamicVisibleBuffer = 0;
        GLuint mDynamicCommandBuffer = 0;
        GLuint mSkinSourceBuffer = 0;
        GLuint mSkinGroupBuffer = 0;
        GLuint mSkinRangeBuffer = 0;
        GLuint mSkinInfluenceBuffer = 0;
        GLuint mSkinTaskBuffer = 0;
        GLuint mSkinMatrixBuffer = 0;
        std::size_t mMaterialCapacity = 0;
        std::map<osg::ref_ptr<const osg::Texture>, GLuint64> mHandles;
        std::size_t mVertexCapacity = 0;
        std::size_t mIndexCapacity = 0;
        std::size_t mInstanceCapacity = 0;
        std::size_t mBatchCapacity = 0;
        std::size_t mSkinSourceCapacity = 0;
        std::size_t mSkinGroupCapacity = 0;
        std::size_t mSkinRangeCapacity = 0;
        std::size_t mSkinInfluenceCapacity = 0;
        std::uint32_t mBatchCount = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> mDynamicRanges;
        osg::ref_ptr<osg::Program> mCullProgram;
        osg::ref_ptr<osg::Program> mCommandProgram;
        osg::ref_ptr<osg::Program> mSkinProgram;
        osg::ref_ptr<osg::Uniform> mInstanceCount;
        osg::ref_ptr<osg::Uniform> mChunkCount;
        osg::ref_ptr<osg::Uniform> mPlaneCount;
        osg::ref_ptr<osg::Uniform> mPlanes;
        osg::ref_ptr<osg::Uniform> mPixelSizeVector;
        osg::ref_ptr<osg::Uniform> mSmallFeaturePixelSize;
        osg::ref_ptr<osg::Uniform> mViewPoint;
        osg::ref_ptr<osg::Uniform> mLodScale;
        osg::ref_ptr<osg::Uniform> mBatchCountUniform;
        std::map<const osg::Program*, osg::ref_ptr<osg::StateSet>> mCastingStateSets;

        void grow(GLuint& buffer, std::size_t& capacity, std::size_t required, GLenum usage)
        {
            if (buffer != 0 && required <= capacity)
                return;
            std::size_t newCapacity = std::max<std::size_t>(capacity, 1 << 16);
            while (newCapacity < required)
                newCapacity *= 2;
            GLuint newBuffer = 0;
            mExt->glGenBuffers(1, &newBuffer);
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, newBuffer);
            mExt->glBufferData(GL_COPY_WRITE_BUFFER, newCapacity, nullptr, usage);
            if (buffer != 0)
            {
                mExt->glBindBuffer(GL_COPY_READ_BUFFER, buffer);
                mCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, capacity);
                mExt->glBindBuffer(GL_COPY_READ_BUFFER, 0);
                mExt->glDeleteBuffers(1, &buffer);
            }
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            buffer = newBuffer;
            capacity = newCapacity;
            mVao = 0;
            mDynamicVao = 0;
        }

        void upload(GLuint buffer, std::size_t offset, const void* data, std::size_t size)
        {
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            mExt->glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        void createVao(osg::State& state, bool dynamic = false)
        {
            GLuint& vao = dynamic ? mDynamicVao : mVao;
            if (vao != 0)
                return;
            mExt->glGenVertexArrays(1, &vao);
            const GLuint previous = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            state.bindVertexArrayObject(vao);
            mExt->glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
            const GLsizei stride = sizeof(GpuVertex);
            glVertexPointer(3, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mPosition)));
            glEnableClientState(GL_VERTEX_ARRAY);
            glNormalPointer(GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mNormal)));
            glEnableClientState(GL_NORMAL_ARRAY);
            glColorPointer(4, GL_UNSIGNED_BYTE, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mColor)));
            glEnableClientState(GL_COLOR_ARRAY);
            for (unsigned int unit = 0; unit < 7; ++unit)
            {
                state.setClientActiveTextureUnit(unit);
                glTexCoordPointer(2, GL_FLOAT, stride,
                    reinterpret_cast<const void*>(offsetof(GpuVertex, mUv) + unit * sizeof(float) * 2));
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            }
            state.setClientActiveTextureUnit(7);
            glTexCoordPointer(4, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mTexCoord7)));
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            state.setClientActiveTextureUnit(0);
            mExt->glBindBuffer(GL_ARRAY_BUFFER, dynamic ? mDynamicVisibleBuffer : mVisibleBuffer);
            mExt->glVertexAttribIPointer(
                Shader::ShaderManager::sInstanceSlotAttribute, 1, GL_UNSIGNED_INT, sizeof(std::uint32_t), nullptr);
            mExt->glVertexAttribDivisor(Shader::ShaderManager::sInstanceSlotAttribute, 1);
            mExt->glEnableVertexAttribArray(Shader::ShaderManager::sInstanceSlotAttribute);
            mExt->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer);
            state.bindVertexArrayObject(previous);
            osg::VertexArrayState* vas = state.getCurrentVertexArrayState();
            const osg::GLBufferObject* vbo = vas != nullptr ? vas->getCurrentVertexBufferObject() : nullptr;
            mExt->glBindBuffer(GL_ARRAY_BUFFER, vbo != nullptr ? vbo->getGLObjectID() : 0);
        }

        GLuint64 handle(osg::State& state, osg::Texture& texture)
        {
            const auto found = mHandles.find(&texture);
            if (found != mHandles.end())
                return found->second;
            osg::Texture::TextureObject* object = texture.getTextureObject(state.getContextID());
            if (object == nullptr)
            {
                state.applyTextureAttribute(0, &texture);
                object = texture.getTextureObject(state.getContextID());
            }
            GLuint64 result = 0;
            if (object != nullptr)
            {
                result = mExt->glGetTextureHandle(object->id());
                mExt->glMakeTextureHandleResident(result);
            }
            mHandles.emplace(&texture, result);
            return result;
        }

        void zero(GLuint buffer)
        {
            const std::uint32_t zero = 0;
            mExt->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            mClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            mExt->glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    };

    GpuScene::GpuScene(Shader::ShaderManager& shaderManager)
        : mShaderManager(shaderManager)
    {
        if (!SceneUtil::glExtensionsReady())
            return;
        osg::GLExtensions& ext = SceneUtil::getGLExtensions();
        mSupported = static_cast<int>(ext.glslLanguageVersion * 100) >= 430 && ext.isVAOSupported
            && ext.glDispatchCompute != nullptr && ext.glMultiDrawElementsIndirect != nullptr
            && ext.glBindBufferBase != nullptr && ext.glMemoryBarrier != nullptr && ext.glVertexAttribDivisor != nullptr
            && ext.glVertexAttribIPointer != nullptr;
        if (!mSupported)
        {
            Log(Debug::Warning) << "GPU-driven object paging unavailable: OpenGL 4.3 features missing";
            return;
        }
        mMaterialTable = ext.glGetTextureHandle != nullptr && ext.glMakeTextureHandleResident != nullptr
            && osg::isGLExtensionSupported(ext.contextID, "GL_ARB_bindless_texture")
            && (osg::isGLExtensionSupported(ext.contextID, "GL_NV_gpu_shader5")
                || osg::isGLExtensionSupported(ext.contextID, "GL_EXT_nonuniform_qualifier"));
        if (!mMaterialTable)
            Log(Debug::Warning) << "GPU-driven object paging: no bindless textures, one draw per material";
        mGl = std::make_unique<Gl>();
        mGl->mExt = &ext;
        osg::setGLExtensionFuncPtr(mGl->mClearBufferData, "glClearBufferData");
        osg::setGLExtensionFuncPtr(mGl->mCopyBufferSubData, "glCopyBufferSubData");
        if (mGl->mCopyBufferSubData == nullptr || mGl->mClearBufferData == nullptr)
        {
            Log(Debug::Warning) << "GPU paging unavailable: buffer functions missing";
            mSupported = false;
            mGl.reset();
            return;
        }
        mNode = new GpuSceneNode(this);
        mNode->setName("GpuScene");
        // Nested bins avoid the reserved stage bins.
        mOrderedNode = new osg::Group;
        mOrderedNode->setName("GpuSceneOrdered");
        mOrderedNode->setCullingActive(false);
        mOrderedNode->getOrCreateStateSet()->setRenderBinDetails(RenderBin_Default, "RenderBin");
        mNode->addChild(mOrderedNode);
    }

    GpuScene::~GpuScene()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const osg::observer_ptr<GpuCaptureCallback>& callback : mCaptureCallbacks)
            if (callback.valid())
                callback->detach();
        for (GpuChunkNode* node : mChunkNodes)
            node->detach();
        for (Dispatcher& dispatcher : mDispatchers)
            static_cast<DispatchDrawable*>(dispatcher.mDrawable.get())->detach();
        if (mNode != nullptr)
        {
            mNode->setScene(nullptr);
            while (mNode->getNumParents() > 0)
                mNode->getParent(0)->removeChild(mNode);
        }
    }

    osg::Group* GpuScene::getNode()
    {
        return mNode;
    }

    void GpuScene::setEnabled(bool enabled)
    {
        mEnabled = enabled;
    }

    void GpuScene::setLightManager(SceneUtil::LightManager* lightManager)
    {
        mLightManager = lightManager;
    }

    void GpuScene::setDefaultMaterial(const SceneUtil::Material* material)
    {
        mDefaultMaterial = material;
    }

    void GpuScene::capture(osg::Node& node)
    {
        if (!isEnabled())
            return;
        CaptureVisitor visitor(mNode);
        // The sky keeps per-view transforms and state.
        visitor.setTraversalMask(~Mask_Sky);
        node.accept(visitor);
        std::lock_guard<std::mutex> lock(mMutex);
        for (osg::Geometry* geometry : visitor.mGeometries)
        {
            if (geometry->getDrawCallback() != nullptr)
                continue;
            if (auto* existing = dynamic_cast<GpuCaptureCallback*>(geometry->getCullCallback()))
            {
                existing->resetPath();
                continue;
            }
            osg::ref_ptr<GpuCaptureCallback> callback = new GpuCaptureCallback(
                *this, geometry->getCullCallback(), geometry->getDataVariance() == osg::Object::DYNAMIC);
            geometry->setCullCallback(callback);
            mCaptureCallbacks.push_back(callback);
        }
        for (SceneUtil::RigGeometry* rig : visitor.mRigs)
        {
            osg::Geometry* geometry = rig->getGpuGeometry();
            const osg::ref_ptr<osg::Geometry> source = rig->getSourceGeometry();
            if (geometry == nullptr || source == nullptr)
                continue;
            const Id mesh = getMeshLocked(*geometry, source.get());
            if (mesh == sInvalid || !registerSkinLocked(mesh, *rig))
                continue;
            rig->setGpuSkinning(true);
            if (auto* existing = dynamic_cast<GpuCaptureCallback*>(rig->getOuterCull()))
            {
                existing->resetPath();
                continue;
            }
            osg::ref_ptr<GpuCaptureCallback> callback = new GpuCaptureCallback(*this, rig->getOuterCull(), false);
            rig->setOuterCull(callback);
            mCaptureCallbacks.push_back(callback);
        }
        for (SceneUtil::MorphGeometry* morph : visitor.mMorphs)
        {
            if (auto* existing = dynamic_cast<GpuCaptureCallback*>(morph->getInnerCull()))
            {
                existing->resetPath();
                continue;
            }
            osg::ref_ptr<GpuCaptureCallback> callback = new GpuCaptureCallback(*this, nullptr, true);
            morph->setInnerCull(callback);
            mCaptureCallbacks.push_back(callback);
        }
    }

    GpuScene::Id GpuScene::getMesh(const osg::Geometry& geometry)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return getMeshLocked(geometry);
    }

    GpuScene::Id GpuScene::getMeshLocked(const osg::Geometry& geometry, const osg::Geometry* source)
    {
        const auto found = mMeshIds.find(&geometry);
        if (found != mMeshIds.end())
            return found->second;

        PendingMesh pending;
        Id id = sInvalid;
        const osg::Geometry& data = source != nullptr ? *source : geometry;
        if (packGeometry(data, pending.mVertices, pending.mIndices))
        {
            const std::uint32_t vertexCount = static_cast<std::uint32_t>(pending.mVertices.size() / sizeof(GpuVertex));
            const std::uint32_t indexCount = static_cast<std::uint32_t>(pending.mIndices.size());
            pending.mVertexOffset = mVertices.allocate(vertexCount);
            pending.mIndexOffset = mIndices.allocate(indexCount);
            Mesh mesh{ &geometry, pending.mIndexOffset, indexCount, static_cast<std::int32_t>(pending.mVertexOffset),
                vertexCount };
            if (!mFreeMeshes.empty())
            {
                id = mFreeMeshes.back();
                mFreeMeshes.pop_back();
                mMeshes[id] = std::move(mesh);
            }
            else
            {
                id = static_cast<Id>(mMeshes.size());
                mMeshes.push_back(std::move(mesh));
            }
            mPendingMeshes.push_back(std::move(pending));
            mMeshIds.emplace(&geometry, id);
        }
        return id;
    }

    bool GpuScene::registerSkinLocked(Id mesh, const SceneUtil::RigGeometry& rig)
    {
        const std::vector<std::uint32_t>& groups = rig.getSkinGroups();
        if (mesh >= mMeshes.size() || groups.size() != mMeshes[mesh].mVertexCount)
            return false;
        const auto found = mSkinMeshes.find(mesh);
        if (found != mSkinMeshes.end())
            return true;

        std::vector<SceneUtil::RigGeometry::SkinRange> ranges;
        std::vector<SceneUtil::RigGeometry::SkinWeight> influences;
        const std::size_t boneCount = rig.getSkinData(ranges, influences);
        if (boneCount == 0 || boneCount > std::numeric_limits<std::uint32_t>::max()
            || influences.size() > std::numeric_limits<std::uint32_t>::max())
            return false;
        std::uint32_t count = 0;
        for (const std::uint32_t group : groups)
        {
            if (group == std::numeric_limits<std::uint32_t>::max())
                continue;
            if (group >= groups.size())
                return false;
            count = std::max(count, group + 1);
        }
        if (count == 0 || count != ranges.size())
            return false;
        for (const SceneUtil::RigGeometry::SkinRange& range : ranges)
            if (static_cast<std::uint64_t>(range.mFirst) + range.mCount > influences.size())
                return false;
        for (const SceneUtil::RigGeometry::SkinWeight& influence : influences)
            if (influence.mBone >= boneCount)
                return false;

        PendingSkin pending;
        pending.mFirst = static_cast<std::uint32_t>(mMeshes[mesh].mBaseVertex);
        pending.mGroups = groups;
        pending.mGroupFirst = mSkinGroups.allocate(count);
        pending.mInfluenceFirst = mSkinInfluences.allocate(static_cast<std::uint32_t>(influences.size()));
        for (std::uint32_t& group : pending.mGroups)
            if (group != std::numeric_limits<std::uint32_t>::max())
                group += pending.mGroupFirst;
        pending.mGroupRanges.reserve(ranges.size());
        for (const SceneUtil::RigGeometry::SkinRange& range : ranges)
            pending.mGroupRanges.push_back({ pending.mInfluenceFirst + range.mFirst, range.mCount });
        pending.mInfluences.reserve(influences.size());
        for (const SceneUtil::RigGeometry::SkinWeight& influence : influences)
            pending.mInfluences.push_back({ influence.mBone, influence.mWeight });

        const SkinMesh skin{ pending.mGroupFirst, count, pending.mInfluenceFirst,
            static_cast<std::uint32_t>(pending.mInfluences.size()), static_cast<std::uint32_t>(boneCount) };
        mSkinMeshes.emplace(mesh, skin);
        mPendingSkins.push_back(std::move(pending));
        return true;
    }

    std::size_t GpuScene::PathHash::operator()(const SurfacePath& path) const
    {
        std::size_t value = path.size();
        for (const osg::ref_ptr<const osg::StateSet>& stateSet : path)
            value ^= std::hash<const osg::StateSet*>{}(stateSet.get()) + 0x9e3779b9 + (value << 6) + (value >> 2);
        return value;
    }

    std::size_t GpuScene::PathHash::operator()(const std::vector<const osg::StateSet*>& path) const
    {
        std::size_t value = path.size();
        for (const osg::StateSet* stateSet : path)
            value ^= std::hash<const osg::StateSet*>{}(stateSet) + 0x9e3779b9 + (value << 6) + (value >> 2);
        return value;
    }

    bool GpuScene::PathEqual::operator()(const SurfacePath& left, const SurfacePath& right) const
    {
        return left == right;
    }

    bool GpuScene::PathEqual::operator()(const SurfacePath& left, const std::vector<const osg::StateSet*>& right) const
    {
        return left.size() == right.size()
            && std::equal(left.begin(), left.end(), right.begin(),
                [](const osg::ref_ptr<const osg::StateSet>& l, const osg::StateSet* r) { return l.get() == r; });
    }

    bool GpuScene::PathEqual::operator()(const std::vector<const osg::StateSet*>& left, const SurfacePath& right) const
    {
        return operator()(right, left);
    }

    struct GpuScene::MaterialSource
    {
        std::vector<const osg::StateSet*> mPath;
        const osg::StateSet::RefAttributePair* mMaterial = nullptr;
        std::array<const osg::StateSet::RefUniformPair*, sMapCount> mMaps{};
        std::array<const osg::StateSet::RefAttributePair*, sMapCount> mTextures{};
        std::array<int, sMapCount> mUnits{};
        bool mMapsInitialized = false;
        const osg::StateSet::RefUniformPair* mEnvMapColor = nullptr;
        const osg::StateSet::RefUniformPair* mAlphaRef = nullptr;
        const osg::StateSet::RefUniformPair* mLumaBias = nullptr;
        const osg::StateSet::RefUniformPair* mBumpMatrix = nullptr;
        std::array<const osg::StateSet::RefUniformPair*, 8> mTexMats{};
    };

    std::unique_ptr<GpuScene::MaterialSource> GpuScene::makeSource(
        const std::vector<const osg::StateSet*>& statePath, const osg::Program& program) const
    {
        auto source = std::make_unique<MaterialSource>();
        source->mPath = statePath;
        const auto uniform = [&](const char* name) {
            return resolvePair<osg::StateSet::RefUniformPair>(
                statePath, [&](const osg::StateSet& stateSet) { return stateSet.getUniformPair(name); });
        };
        source->mMaterial = resolvePair<osg::StateSet::RefAttributePair>(statePath,
            [](const osg::StateSet& stateSet) { return stateSet.getAttributePair(osg::StateAttribute::MATERIAL); });
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            source->mMaps[i] = uniform(Shader::sMaterialMapNames[i]);
            source->mUnits[i] = mShaderManager.getSamplerUnit(program, Shader::sMaterialMapNames[i]).value_or(-1);
        }
        source->mEnvMapColor = uniform("envMapColor");
        source->mAlphaRef = uniform("alphaRef");
        source->mLumaBias = uniform("envMapLumaBias");
        source->mBumpMatrix = uniform("bumpMapMatrix");
        for (unsigned int i = 0; i < source->mTexMats.size(); ++i)
            source->mTexMats[i] = uniform(sTexMatNames[i]);
        return source;
    }

    GpuScene::PendingMaterial GpuScene::makeMaterial(MaterialSource& source, Id index) const
    {
        PendingMaterial material;
        material.mIndex = index;
        GpuMaterial& record = material.mRecord;
        std::memset(&record, 0, sizeof(record));
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            const int unit = uniformValue(source.mMaps[i], source.mUnits[i]);
            if (!source.mMapsInitialized || source.mUnits[i] != unit)
            {
                source.mUnits[i] = unit;
                source.mTextures[i] = unit < 0
                    ? nullptr
                    : resolvePair<osg::StateSet::RefAttributePair>(source.mPath, [&](const osg::StateSet& stateSet) {
                          return stateSet.getTextureAttributePair(unit, osg::StateAttribute::TEXTURE);
                      });
            }
            if (source.mTextures[i] != nullptr)
                material.mTextures[i] = dynamic_cast<osg::Texture*>(source.mTextures[i]->first.get());
        }
        source.mMapsInitialized = true;
        const SceneUtil::Material* sceneMaterial = source.mMaterial == nullptr
            ? nullptr
            : dynamic_cast<const SceneUtil::Material*>(source.mMaterial->first.get());
        const SceneUtil::Material& mat = sceneMaterial != nullptr ? *sceneMaterial : *mDefaultMaterial;
        put(record.mDiffuse, mat.getDiffuse());
        put(record.mAmbient, mat.getAmbient());
        put(record.mSpecular, mat.getSpecular());
        put(record.mEmission, mat.getEmission());
        put(record.mEnvMapColor, uniformValue(source.mEnvMapColor, osg::Vec4f(1, 1, 1, 1)));
        record.mParams[0] = mat.getShininess();
        record.mParams[1] = mat.getEmissiveMultiplier();
        record.mParams[2] = mat.getSpecularStrength();
        record.mParams[3] = uniformValue(source.mAlphaRef, 0.f);
        const osg::Vec2f lumaBias = uniformValue(source.mLumaBias, osg::Vec2f(0, 0));
        record.mLumaBias[0] = lumaBias.x();
        record.mLumaBias[1] = lumaBias.y();
        const osg::Matrix2 bumpMatrix = uniformValue(source.mBumpMatrix, osg::Matrix2(1, 0, 0, 1));
        for (int i = 0; i < 4; ++i)
            record.mBumpMatrix[i] = bumpMatrix[i];
        record.mModes[0] = static_cast<std::int32_t>(mat.getVertexColorMode());
        for (unsigned int unit = 0; unit < 8; ++unit)
        {
            const osg::Matrixf texMat = uniformValue(source.mTexMats[unit], osg::Matrixf::identity());
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 4; ++col)
                    record.mTexMatRows[unit * 2 + row][col] = texMat(col, row);
        }
        return material;
    }

    void GpuScene::refreshMaterialLocked(Id material, unsigned int frame)
    {
        if (!mMaterialTable || material >= mMaterials.size() || material >= mMaterialSources.size()
            || mMaterialFrames[material] == frame)
            return;
        mMaterialFrames[material] = frame;
        PendingMaterial pending = makeMaterial(*mMaterialSources[material], material);
        const PendingMaterial& current = mMaterials[material];
        bool changed = std::memcmp(&pending.mRecord, &current.mRecord, sizeof(GpuMaterial)) != 0;
        for (unsigned int i = 0; i < sMapCount && !changed; ++i)
            changed = pending.mTextures[i].get() != current.mTextures[i].get();
        if (!changed)
            return;
        mMaterials[material] = pending;
        mPendingMaterials.push_back(std::move(pending));
    }

    GpuScene::Surface GpuScene::getSurface(const std::vector<const osg::StateSet*>& statePath, int order)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return getSurfaceLocked(statePath, order);
    }

    GpuScene::Surface GpuScene::getSurfaceLocked(const std::vector<const osg::StateSet*>& statePath, int order)
    {
        const auto found = mSurfaces.find(statePath);
        if (found != mSurfaces.end())
            return found->second;
        SurfacePath key(statePath.begin(), statePath.end());

        Surface surface;
        osg::ref_ptr<osg::StateSet> merged = new osg::StateSet;
        for (const osg::StateSet* stateSet : statePath)
            merged->merge(*stateSet);
        const osg::Program* program
            = dynamic_cast<const osg::Program*>(merged->getAttribute(osg::StateAttribute::PROGRAM));
        osg::ref_ptr<osg::Program> instanced;
        if (program != nullptr)
            instanced = mShaderManager.getInstancedProgram(
                *program, mMaterialTable ? Shader::MaterialMode::Bindless : Shader::MaterialMode::Legacy);
        if (instanced == nullptr)
        {
            mSurfaces.emplace(std::move(key), surface);
            return surface;
        }

        osg::ref_ptr<osg::StateSet> pipeline = merged;
        if (mMaterialTable)
        {
            std::unique_ptr<MaterialSource> source = makeSource(statePath, *program);
            PendingMaterial material = makeMaterial(*source, mMaterialCount++);
            pipeline = new osg::StateSet(*merged, osg::CopyOp::SHALLOW_COPY);
            std::vector<unsigned int> materialUnits;
            for (unsigned int i = 0; i < sMapCount; ++i)
            {
                const int unit = source->mUnits[i];
                if (unit < 0)
                    continue;
                materialUnits.push_back(static_cast<unsigned int>(unit));
                pipeline->removeUniform(Shader::sMaterialMapNames[i]);
            }
            std::sort(materialUnits.begin(), materialUnits.end());
            materialUnits.erase(std::unique(materialUnits.begin(), materialUnits.end()), materialUnits.end());
            for (const unsigned int unit : materialUnits)
            {
                pipeline->removeTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                pipeline->removeTextureMode(unit, GL_TEXTURE_2D);
            }
            pipeline->removeAttribute(osg::StateAttribute::MATERIAL);
            for (const char* name : sMaterialUniforms)
                pipeline->removeUniform(name);
            surface.mMaterial = material.mIndex;
            mMaterials.push_back(material);
            mMaterialSources.push_back(std::move(source));
            mMaterialFrames.push_back(std::numeric_limits<unsigned int>::max());
            mPendingMaterials.push_back(std::move(material));
        }
        pipeline->setAttributeAndModes(instanced, osg::StateAttribute::ON);
        if (order != 0)
            pipeline->setRenderBinDetails(order - 1, "RenderBin");

        for (Id id = 0; id < mBuckets.size(); ++id)
        {
            if (mBuckets[id].mStateSet->compare(*pipeline, true) == 0)
            {
                surface.mBucket = id;
                break;
            }
        }
        if (surface.mBucket == sInvalid)
        {
            surface.mBucket = static_cast<Id>(mBuckets.size());
            Bucket bucket;
            bucket.mStateSet = pipeline;
            bucket.mProgram = instanced;
            mProgramsToLink.push_back(instanced);
            mBuckets.push_back(std::move(bucket));

            BinKey binKey;
            binKey.mHint = pipeline->getRenderingHint();
            binKey.mNumber = pipeline->getBinNumber();
            binKey.mMode = pipeline->getRenderBinMode();
            binKey.mName = pipeline->getBinName();
            binKey.mNested = pipeline->getNestRenderBins();
            auto dispatcher = std::find_if(mDispatchers.begin(), mDispatchers.end(),
                [&](const Dispatcher& candidate) { return candidate.mKey == binKey; });
            if (dispatcher == mDispatchers.end())
            {
                const Id id = static_cast<Id>(mDispatchers.size());
                Dispatcher created;
                created.mKey = binKey;
                created.mStateSet = new osg::StateSet;
                if (pipeline->useRenderBinDetails())
                    created.mStateSet->setRenderBinDetails(
                        pipeline->getBinNumber(), pipeline->getBinName(), pipeline->getRenderBinMode());
                else
                    created.mStateSet->setRenderingHint(pipeline->getRenderingHint());
                created.mStateSet->setNestRenderBins(pipeline->getNestRenderBins());
                created.mDrawable = new DispatchDrawable(*this, id);
                created.mDrawable->setStateSet(created.mStateSet);
                created.mOrdered = order != 0;
                mPendingDrawables.emplace_back(created.mDrawable, created.mOrdered);
                mDispatchers.push_back(std::move(created));
                dispatcher = std::prev(mDispatchers.end());
            }
            dispatcher->mBuckets.push_back(surface.mBucket);
        }
        mSurfaces.emplace(std::move(key), surface);
        return surface;
    }

    std::pair<GpuScene::Id, GpuScene::Surface> GpuCaptureCallback::resolveLocked(osg::Geometry& geometry,
        const std::vector<const osg::StateSet*>& path, unsigned int frame, bool liveMaterial) const
    {
        GpuScene& scene = *mScene.load(std::memory_order_relaxed);
        GpuScene::Id mesh = GpuScene::sInvalid;
        Cache* match = nullptr;
        for (Cache& cache : mCache)
        {
            if (cache.mGeometry != &geometry)
                continue;
            if (cache.mMesh < scene.mMeshes.size() && scene.mMeshes[cache.mMesh].mGeometry == &geometry)
                mesh = cache.mMesh;
            if (cache.mPath.size() == path.size()
                && std::equal(cache.mPath.begin(), cache.mPath.end(), path.begin(),
                    [](const osg::ref_ptr<const osg::StateSet>& left, const osg::StateSet* right) {
                        return left.get() == right;
                    }))
                match = &cache;
        }
        if (mesh == GpuScene::sInvalid)
            mesh = scene.getMeshLocked(geometry);
        if (match != nullptr)
        {
            match->mMesh = mesh;
            if (liveMaterial)
                scene.refreshMaterialLocked(match->mSurface.mMaterial, frame);
            return { mesh, match->mSurface };
        }

        const GpuScene::Surface surface = scene.getSurfaceLocked(path, 0);
        if (liveMaterial)
            scene.refreshMaterialLocked(surface.mMaterial, frame);
        Cache& cache = mCache[mNextCache++ % mCache.size()];
        cache.mGeometry = &geometry;
        cache.mPath.assign(path.begin(), path.end());
        cache.mMesh = mesh;
        cache.mSurface = surface;
        return { mesh, surface };
    }

    bool GpuCaptureCallback::hasLiveMaterial(const osgUtil::CullVisitor& cv) const
    {
        int live = mLiveMaterial.load(std::memory_order_relaxed);
        if (live >= 0)
            return live != 0;
        live = 0;
        for (const osg::Node* node : cv.getNodePath())
        {
            if (hasUpdater(node->getUpdateCallback()) || hasUpdater(node->getCullCallback()))
            {
                live = 1;
                break;
            }
        }
        mLiveMaterial.store(live, std::memory_order_relaxed);
        return live != 0;
    }

    osg::ref_ptr<osg::Node> GpuScene::createTerrainChunk(
        Terrain::TerrainDrawable& drawable, const osg::Vec3f& worldOffset, bool)
    {
        if (!isEnabled() || drawable.getPasses().empty())
            return nullptr;
        const Id mesh = getMesh(drawable);
        if (mesh == sInvalid)
            return nullptr;
        std::vector<Instance> instances;
        for (const osg::ref_ptr<osg::StateSet>& pass : drawable.getPasses())
        {
            std::vector<const osg::StateSet*> path;
            if (drawable.getStateSet() != nullptr)
                path.push_back(drawable.getStateSet());
            path.push_back(pass);
            const Surface surface = getSurface(path, static_cast<int>(&pass - drawable.getPasses().data()) + 1);
            if (surface.mBucket == sInvalid)
                return nullptr;
            Instance instance;
            instance.mWorld = osg::Matrixf::translate(worldOffset);
            const osg::BoundingBox& box = drawable.getBoundingBox();
            instance.mBounds = osg::BoundingSphere(box.center() + worldOffset, box.radius());
            instance.mSmallFeatureCulling = true;
            instance.mGeometry = &drawable;
            instance.mBucket = surface.mBucket;
            instance.mMaterial = surface.mMaterial;
            instances.push_back(instance);
        }
        return createChunk(std::move(instances), drawable.getBound());
    }

    void GpuScene::setCastingPrograms(const std::vector<osg::ref_ptr<osg::Program>>& programs)
    {
        for (const osg::ref_ptr<osg::Program>& program : programs)
        {
            if (osg::ref_ptr<osg::Program> instanced = mShaderManager.getInstancedProgram(
                    *program, mMaterialTable ? Shader::MaterialMode::Bindless : Shader::MaterialMode::Legacy))
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mProgramsToLink.push_back(std::move(instanced));
            }
        }
    }

    unsigned int GpuScene::getPendingWork() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return static_cast<unsigned int>(mPendingMeshes.size() + mPendingInstances.size() + mPendingSkins.size()
            + mPendingMaterials.size() + mProgramsToLink.size());
    }

    std::uint32_t GpuScene::RangeAllocator::allocate(std::uint32_t count)
    {
        for (auto it = mFree.begin(); it != mFree.end(); ++it)
        {
            if (it->second < count)
                continue;
            const std::uint32_t first = it->first;
            const std::uint32_t remaining = it->second - count;
            mFree.erase(it);
            if (remaining > 0)
                mFree.emplace(first + count, remaining);
            return first;
        }
        const std::uint32_t first = mExtent;
        mExtent += count;
        return first;
    }

    void GpuScene::RangeAllocator::free(std::uint32_t first, std::uint32_t count)
    {
        auto inserted = mFree.emplace(first, count).first;
        auto next = std::next(inserted);
        if (next != mFree.end() && inserted->first + inserted->second == next->first)
        {
            inserted->second += next->second;
            mFree.erase(next);
        }
        if (inserted != mFree.begin())
        {
            auto prev = std::prev(inserted);
            if (prev->first + prev->second == inserted->first)
            {
                prev->second += inserted->second;
                mFree.erase(inserted);
            }
        }
    }

    std::unique_ptr<const GpuTemplate> GpuScene::extract(const osg::Node& node, unsigned int mask, bool bakeBillboards)
    {
        auto result = std::make_unique<GpuTemplate>();
        GpuExtractor extractor(*this, mask, bakeBillboards, *result);
        extractor.visit(&node, osg::Matrixf::identity(), nullptr, 0, osg::Vec3f(), 1.f, true);
        if (result->mGeometries.empty())
            return nullptr;
        return result;
    }

    void GpuScene::appendInstances(const GpuTemplate& tmpl, const osg::Matrixf& refMatrix, float refScale,
        const osg::LOD::MinMaxPair& refDistances, bool smallFeatureCulling, std::vector<Instance>& out) const
    {
        for (const GpuGeometry& geometry : tmpl.mGeometries)
        {
            Instance instance;
            if (geometry.mLod != nullptr)
            {
                const osg::LOD::RangeList& ranges = geometry.mLod->getRangeList();
                if (empty(intersection(ranges[geometry.mLodChild], refDistances)))
                    continue;
                unsigned int survivors = 0;
                for (const LODRange& range : ranges)
                    if (!empty(intersection(range, refDistances)))
                        ++survivors;
                if (survivors > 1)
                {
                    const float lodScale = geometry.mLodScale * refScale;
                    instance.mLod = true;
                    instance.mLodCenter = geometry.mLodCenter * refMatrix;
                    instance.mLodMin = ranges[geometry.mLodChild].first * lodScale;
                    instance.mLodMax = ranges[geometry.mLodChild].second * lodScale;
                }
            }
            instance.mWorld = geometry.mMatrix * refMatrix;
            const float scale = uniformScale(geometry.mMatrix) * refScale;
            instance.mBounds
                = osg::BoundingSphere(geometry.mBounds.center() * instance.mWorld, geometry.mBounds.radius() * scale);
            instance.mSmallFeatureCulling = smallFeatureCulling;
            instance.mGeometry = geometry.mGeometry;
            instance.mBucket = geometry.mSurface.mBucket;
            instance.mMaterial = geometry.mSurface.mMaterial;
            out.push_back(instance);
        }
    }

    osg::ref_ptr<osg::Node> GpuScene::createChunk(
        std::vector<Instance>&& instances, const osg::BoundingSphere& localBound)
    {
        if (instances.empty())
            return nullptr;

        std::lock_guard<std::mutex> lock(mMutex);
        for (Instance& instance : instances)
            if (getMeshLocked(*instance.mGeometry) == sInvalid)
                return nullptr;
        Chunk chunk;
        chunk.mGeneration = mNextGeneration++;
        if (mNextGeneration == 0)
            mNextGeneration = 1;
        chunk.mInstanceCount = static_cast<std::uint32_t>(instances.size());
        chunk.mFirstInstance = mInstances.allocate(chunk.mInstanceCount);

        Id slot;
        if (!mFreeChunks.empty())
        {
            slot = mFreeChunks.back();
            mFreeChunks.pop_back();
        }
        else
        {
            slot = static_cast<Id>(mChunks.size());
            mChunks.emplace_back();
        }

        std::map<Id, std::uint32_t> batchCounts;
        PendingInstances pending;
        pending.mFirst = chunk.mFirstInstance;
        pending.mRecords.reserve(instances.size());
        for (const Instance& instance : instances)
        {
            const Id meshId = mMeshIds.at(instance.mGeometry.get());
            const auto key = std::make_pair(instance.mBucket, meshId);
            auto batchIt = mBatchIds.find(key);
            if (batchIt == mBatchIds.end())
            {
                const Id batchId = static_cast<Id>(mBatches.size());
                mBatches.push_back(Batch{ instance.mBucket, meshId, 0 });
                mBuckets[instance.mBucket].mBatches.push_back(batchId);
                batchIt = mBatchIds.emplace(key, batchId).first;
            }
            const Id batch = batchIt->second;
            ++mBatches[batch].mResident;
            ++batchCounts[batch];
            chunk.mMeshes.push_back(meshId);
            pending.mRecords.push_back(makeRecord(instance, batch, slot, chunk.mGeneration));
            chunk.mBounds.expandBy(instance.mBounds);
        }
        chunk.mBatchCounts.assign(batchCounts.begin(), batchCounts.end());
        std::sort(chunk.mMeshes.begin(), chunk.mMeshes.end());
        chunk.mMeshes.erase(std::unique(chunk.mMeshes.begin(), chunk.mMeshes.end()), chunk.mMeshes.end());
        for (const Id meshId : chunk.mMeshes)
            ++mMeshes[meshId].mUsers;
        mResidentBound.expandBy(chunk.mBounds);
        mBoundDirty = true;
        mBatchesDirty = true;
        mPendingInstances.push_back(std::move(pending));
        mChunks[slot] = std::move(chunk);
        osg::ref_ptr<GpuChunkNode> node = new GpuChunkNode(*this, slot, mChunks[slot].mGeneration, localBound);
        mChunkNodes.insert(node.get());
        return node;
    }

    GpuScene::GpuInstance GpuScene::makeRecord(const Instance& instance, Id batch, Id slot, Id generation)
    {
        GpuInstance record;
        const osg::Matrixf& m = instance.mWorld;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                record.mRows[row][col] = m(col, row);
        record.mBounds[0] = instance.mBounds.center().x();
        record.mBounds[1] = instance.mBounds.center().y();
        record.mBounds[2] = instance.mBounds.center().z();
        record.mBounds[3] = instance.mBounds.radius();
        record.mLod[0] = instance.mLodCenter.x();
        record.mLod[1] = instance.mLodCenter.y();
        record.mLod[2] = instance.mLodCenter.z();
        record.mLod[3] = instance.mLodMin;
        unsigned int flags = 0;
        if (instance.mSmallFeatureCulling)
            flags |= sFlagSmallFeature;
        if (instance.mLod)
            flags |= sFlagLod;
        record.mMeta[0] = batch | (flags << 24);
        record.mMeta[1] = slot;
        record.mMeta[2] = generation;
        std::memcpy(&record.mMeta[3], &instance.mLodMax, sizeof(float));
        record.mMaterial[0] = instance.mMaterial;
        record.mMaterial[1] = sInvalid;
        record.mMaterial[1] = record.mMaterial[2] = record.mMaterial[3] = 0;
        return record;
    }

    bool GpuScene::updateChunk(osg::Node& node, const std::vector<Instance>& instances)
    {
        auto* chunkNode = dynamic_cast<GpuChunkNode*>(&node);
        if (chunkNode == nullptr)
            return false;
        std::lock_guard<std::mutex> lock(mMutex);
        const Id slot = chunkNode->getSlot();
        Chunk& chunk = mChunks[slot];
        if (chunk.mInstanceCount != instances.size())
            return false;
        PendingInstances pending;
        pending.mFirst = chunk.mFirstInstance;
        pending.mRecords.reserve(instances.size());
        chunk.mBounds = osg::BoundingSphere();
        for (const Instance& instance : instances)
        {
            const Id meshId = mMeshIds.at(instance.mGeometry.get());
            const Id batch = mBatchIds.at(std::make_pair(instance.mBucket, meshId));
            pending.mRecords.push_back(makeRecord(instance, batch, slot, chunk.mGeneration));
            chunk.mBounds.expandBy(instance.mBounds);
        }
        mResidentBound.expandBy(chunk.mBounds);
        mBoundDirty = true;
        mPendingInstances.push_back(std::move(pending));
        return true;
    }

    void GpuScene::releaseChunk(GpuChunkNode& node, Id slot)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mChunkNodes.erase(&node);
        Chunk& chunk = mChunks[slot];
        for (const auto& [batch, count] : chunk.mBatchCounts)
            mBatches[batch].mResident -= count;
        for (const Id meshId : chunk.mMeshes)
        {
            Mesh& mesh = mMeshes[meshId];
            if (--mesh.mUsers > 0)
                continue;
            mVertices.free(static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount);
            mIndices.free(mesh.mFirstIndex, mesh.mIndexCount);
            const auto skin = mSkinMeshes.find(meshId);
            if (skin != mSkinMeshes.end())
            {
                mSkinGroups.free(skin->second.mGroupFirst, skin->second.mGroupCount);
                mSkinInfluences.free(skin->second.mInfluenceFirst, skin->second.mInfluenceCount);
                mSkinMeshes.erase(skin);
            }
            mMeshIds.erase(mesh.mGeometry.get());
            mesh.mGeometry = nullptr;
            mFreeMeshes.push_back(meshId);
        }
        mInstances.free(chunk.mFirstInstance, chunk.mInstanceCount);
        chunk = Chunk();
        mFreeChunks.push_back(slot);
        mResidentBound = osg::BoundingSphere();
        for (const Chunk& other : mChunks)
            if (other.mGeneration != 0)
                mResidentBound.expandBy(other.mBounds);
        mBoundDirty = true;
        mBatchesDirty = true;
    }

    void GpuScene::recordView(osgUtil::CullVisitor& cv)
    {
        if (!isEnabled())
            return;
        const osg::CullingSet& cullingSet = cv.getCurrentCullingSet();
        const osg::Polytope& frustum = cullingSet.getFrustum();
        const osg::Polytope::ClippingMask mask = frustum.getCurrentMask();
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();

        sCullPath.mScene = this;
        sCullPath.mFrame = frame;
        sCullPath.mCamera = cv.getCurrentCamera();
        sCullPath.mStates.clear();
        for (osgUtil::StateGraph* graph = cv.getCurrentStateGraph(); graph != nullptr; graph = graph->_parent)
            if (graph->getStateSet() != nullptr)
                sCullPath.mStates.push_back(graph->getStateSet());

        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = mViews.begin(); it != mViews.end();)
        {
            if (it->first.first + 2 < frame)
            {
                mFreeViews.push_back(std::move(it->second));
                it = mViews.erase(it);
            }
            else
                ++it;
        }
        auto [viewIt, inserted] = mViews.try_emplace(ViewKey(frame, cv.getCurrentCamera()));
        if (inserted && !mFreeViews.empty())
        {
            viewIt->second = std::move(mFreeViews.back());
            mFreeViews.pop_back();
            viewIt->second.mHasParams = false;
            viewIt->second.mDispatched = false;
            viewIt->second.mPlanes.clear();
            viewIt->second.mChunks.clear();
            viewIt->second.mDynamic.clear();
            viewIt->second.mSkins.clear();
        }
        mDispatchingCameras.insert(cv.getCurrentCamera());
        ViewRecord& view = viewIt->second;
        view.mPlanes.clear();
        const osg::Polytope::PlaneList& planes = frustum.getPlaneList();
        for (unsigned int i = 0; i < planes.size() && view.mPlanes.size() < sMaxPlanes; ++i)
            if (mask & (1u << i))
                view.mPlanes.push_back(planes[i].asVec4());
        view.mPixelSizeVector = cullingSet.getPixelSizeVector();
        view.mSmallFeaturePixelSize = (cullingSet.getCullingMask() & osg::CullingSet::SMALL_FEATURE_CULLING)
            ? cullingSet.getSmallFeatureCullingPixelSize()
            : 0.f;
        view.mViewPoint = cv.getViewPointLocal();
        view.mLodScale = cv.getLODScale();
        view.mHasParams = true;
    }

    bool GpuScene::recordRig(osgUtil::CullVisitor& cv, SceneUtil::RigGeometry& rig, const GpuCaptureCallback& callback)
    {
        if (!mSkinSupported.load(std::memory_order_relaxed))
            return false;
        osg::Geometry* geometry = rig.getGpuGeometry();
        if (geometry == nullptr)
            return false;
        SkinUpdate skin;
        if (!getSkinPose(cv.getTraversalNumber(), rig, skin.mPose))
            return false;
        return record(cv, *geometry, false, callback, &skin);
    }

    bool GpuScene::getSkinPose(unsigned int frame, SceneUtil::RigGeometry& rig, SkinPose& out)
    {
        osg::Geometry* geometry = rig.getGpuGeometry();
        if (geometry == nullptr)
            return false;
        {
            SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
            std::lock_guard<std::mutex> lock(slot.mMutex);
            if (slot.mValid && slot.mFrame == frame)
            {
                const auto found = slot.mPoses.find(geometry);
                if (found != slot.mPoses.end())
                {
                    out = found->second;
                    return true;
                }
            }
        }

        thread_local std::vector<osg::Matrixf> matrices;
        matrices.clear();
        if (!rig.getPalette(frame, matrices))
            return false;
        thread_local std::vector<GpuSkinMatrix> pose;
        pose.resize(matrices.size());
        for (std::size_t i = 0; i < matrices.size(); ++i)
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 4; ++row)
                    pose[i].mCols[col][row] = matrices[i](row, col);

        SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
        std::lock_guard<std::mutex> lock(slot.mMutex);
        if (!slot.mValid || slot.mFrame != frame)
        {
            slot.mFrame = frame;
            slot.mValid = true;
            slot.mPoses.clear();
            slot.mMatrices.clear();
        }
        const auto found = slot.mPoses.find(geometry);
        if (found != slot.mPoses.end())
            out = found->second;
        else
        {
            constexpr std::size_t maxMatrices = std::numeric_limits<std::uint32_t>::max();
            if (pose.size() > maxMatrices || slot.mMatrices.size() > maxMatrices - pose.size())
                return false;
            out = { frame, static_cast<std::uint32_t>(slot.mMatrices.size()), static_cast<std::uint32_t>(pose.size()) };
            slot.mMatrices.insert(slot.mMatrices.end(), pose.begin(), pose.end());
            slot.mPoses.emplace(geometry, out);
        }
        return true;
    }

    bool GpuScene::record(osgUtil::CullVisitor& cv, osg::Geometry& geometry, bool updateMesh,
        const GpuCaptureCallback& callback, SkinUpdate* skin)
    {
        static const std::string sLightBuffer = "LightBuffer";
        thread_local std::vector<const osg::StateSet*> statePath;
        statePath.clear();
        const bool hasBase = sCullPath.mScene == this && sCullPath.mFrame == cv.getFrameStamp()->getFrameNumber()
            && sCullPath.mCamera == cv.getCurrentCamera();
        for (osgUtil::StateGraph* graph = cv.getCurrentStateGraph(); graph != nullptr; graph = graph->_parent)
        {
            const osg::StateSet* stateSet = graph->getStateSet();
            if (stateSet == nullptr
                || (hasBase
                    && std::find(sCullPath.mStates.begin(), sCullPath.mStates.end(), stateSet)
                        != sCullPath.mStates.end())
                || stateSet->getUniform(sLightBuffer) != nullptr)
                continue;
            statePath.push_back(stateSet);
        }
        std::reverse(statePath.begin(), statePath.end());
        if (orderDependentPath(statePath))
            return false;
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        const bool liveMaterial = callback.hasLiveMaterial(cv);

        const osg::RefMatrix* viewMatrix = cv.getCurrentRenderStage()->getInitialViewMatrix();
        osg::Matrixf world(*cv.getModelViewMatrix());
        if (viewMatrix != nullptr)
            world *= osg::Matrixf::inverse(osg::Matrixf(*viewMatrix));
        DynamicInstance instance;
        instance.mGeometry = &geometry;
        instance.mWorld = world;
        instance.mUpdateMesh = updateMesh;

        std::lock_guard<std::mutex> lock(mMutex);
        // Only dispatching cameras consume captures.
        if (mDispatchingCameras.find(cv.getCurrentCamera()) == mDispatchingCameras.end())
            return false;
        const auto [mesh, surface] = callback.resolveLocked(geometry, statePath, frame, liveMaterial);
        if (mesh == sInvalid || surface.mBucket == sInvalid)
            return false;
        instance.mMesh = mesh;
        instance.mBucket = surface.mBucket;
        instance.mMaterial = surface.mMaterial;
        ViewRecord& view = mViews[ViewKey(frame, cv.getCurrentCamera())];
        view.mDynamic.push_back(std::move(instance));
        if (skin != nullptr && skin->mPose.mCount != 0)
        {
            skin->mMesh = mesh;
            view.mSkins.push_back(*skin);
        }
        return true;
    }

    void GpuScene::markChunk(osgUtil::CullVisitor& cv, Id slot, Id generation)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ViewRecord& view = mViews[ViewKey(cv.getFrameStamp()->getFrameNumber(), cv.getCurrentCamera())];
        view.mChunks.emplace_back(slot, generation);
    }

    void GpuScene::update()
    {
        // Releases drain on the next enable.
        if (!isEnabled())
            return;
        std::vector<std::pair<osg::ref_ptr<osg::Drawable>, bool>> drawables;
        bool boundDirty = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            drawables.swap(mPendingDrawables);
            boundDirty = mBoundDirty;
            mBoundDirty = false;
        }
        for (const auto& [drawable, ordered] : drawables)
            (ordered ? mOrderedNode.get() : static_cast<osg::Group*>(mNode.get()))->addChild(drawable);
        if (boundDirty)
            mNode->dirtyBound();
    }

    osg::BoundingSphere GpuScene::getResidentBound() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mResidentBound;
    }

    void GpuScene::rebuildBatches(std::vector<GpuBatch>& table)
    {
        table.assign(mBatches.size(), GpuBatch{ 0, 0, 0, 0, sInvalid, { 0, 0, 0 } });
        std::uint32_t slot = 0;
        std::uint32_t command = 0;
        for (Bucket& bucket : mBuckets)
        {
            bucket.mCommandOffset = command;
            for (const Id batchId : bucket.mBatches)
            {
                const Batch& batch = mBatches[batchId];
                if (batch.mResident == 0)
                    continue;
                const Mesh& mesh = mMeshes[batch.mMesh];
                table[batchId] = GpuBatch{ mesh.mFirstIndex, mesh.mIndexCount, mesh.mBaseVertex, slot, command, {} };
                slot += batch.mResident;
                ++command;
            }
            bucket.mCommandCount = command - bucket.mCommandOffset;
        }
        mBatchesDirty = false;
    }

    void GpuScene::takePendingWork(PendingWork& work, std::size_t limit)
    {
        auto take = [limit](auto& from, auto& to) {
            const std::size_t count = std::min(limit, from.size());
            to.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                to.push_back(std::move(from.front()));
                from.pop_front();
            }
        };
        take(mPendingMeshes, work.mMeshes);
        take(mPendingInstances, work.mInstances);
        take(mPendingSkins, work.mSkins);
        take(mPendingMaterials, work.mMaterials);
        if (mBatchesDirty && mPendingMeshes.empty() && mPendingInstances.empty() && mPendingSkins.empty()
            && mPendingMaterials.empty())
        {
            rebuildBatches(work.mBatches);
            work.mBatchesRebuilt = true;
        }
    }

    void GpuScene::upload(osg::State& state, PendingWork& work)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;

        std::size_t vertexBytes = 0;
        std::size_t indexBytes = 0;
        for (const PendingMesh& mesh : work.mMeshes)
        {
            vertexBytes = std::max(vertexBytes, mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size());
            indexBytes = std::max(indexBytes, (mesh.mIndexOffset + mesh.mIndices.size()) * sizeof(std::uint32_t));
        }
        std::size_t instanceBytes = 0;
        for (const PendingInstances& pending : work.mInstances)
            instanceBytes = std::max(instanceBytes, (pending.mFirst + pending.mRecords.size()) * sizeof(GpuInstance));
        std::size_t materialBytes = 0;
        for (const PendingMaterial& material : work.mMaterials)
            materialBytes = std::max(materialBytes, (material.mIndex + 1) * sizeof(GpuMaterial));
        std::size_t skinGroupBytes = 0;
        std::size_t skinRangeBytes = 0;
        std::size_t skinInfluenceBytes = 0;
        for (const PendingSkin& skin : work.mSkins)
        {
            skinGroupBytes = std::max(skinGroupBytes, (skin.mFirst + skin.mGroups.size()) * sizeof(std::uint32_t));
            skinRangeBytes
                = std::max(skinRangeBytes, (skin.mGroupFirst + skin.mGroupRanges.size()) * sizeof(GpuSkinGroup));
            skinInfluenceBytes = std::max(
                skinInfluenceBytes, (skin.mInfluenceFirst + skin.mInfluences.size()) * sizeof(GpuSkinInfluence));
        }
        gl.grow(gl.mVertexBuffer, gl.mVertexCapacity, vertexBytes, GL_STATIC_DRAW);
        gl.grow(gl.mSkinSourceBuffer, gl.mSkinSourceCapacity, vertexBytes, GL_STATIC_DRAW);
        gl.grow(gl.mSkinGroupBuffer, gl.mSkinGroupCapacity, skinGroupBytes, GL_STATIC_DRAW);
        gl.grow(gl.mSkinRangeBuffer, gl.mSkinRangeCapacity, skinRangeBytes, GL_STATIC_DRAW);
        gl.grow(gl.mSkinInfluenceBuffer, gl.mSkinInfluenceCapacity, skinInfluenceBytes, GL_STATIC_DRAW);
        gl.grow(gl.mIndexBuffer, gl.mIndexCapacity, indexBytes, GL_STATIC_DRAW);
        gl.grow(gl.mMaterialBuffer, gl.mMaterialCapacity, materialBytes, GL_DYNAMIC_COPY);
        const std::size_t oldInstanceCapacity = gl.mInstanceCapacity;
        gl.grow(gl.mInstanceBuffer, gl.mInstanceCapacity, instanceBytes, GL_DYNAMIC_COPY);
        if (gl.mInstanceCapacity != oldInstanceCapacity)
        {
            const std::size_t visibleBytes = gl.mInstanceCapacity / sizeof(GpuInstance) * sizeof(std::uint32_t);
            if (gl.mVisibleBuffer != 0)
                ext.glDeleteBuffers(1, &gl.mVisibleBuffer);
            ext.glGenBuffers(1, &gl.mVisibleBuffer);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, gl.mVisibleBuffer);
            ext.glBufferData(GL_COPY_WRITE_BUFFER, visibleBytes, nullptr, GL_DYNAMIC_COPY);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            gl.mVao = 0;
        }
        for (const PendingMesh& mesh : work.mMeshes)
        {
            gl.upload(
                gl.mVertexBuffer, mesh.mVertexOffset * sizeof(GpuVertex), mesh.mVertices.data(), mesh.mVertices.size());
            gl.upload(gl.mSkinSourceBuffer, mesh.mVertexOffset * sizeof(GpuVertex), mesh.mVertices.data(),
                mesh.mVertices.size());
            gl.upload(gl.mIndexBuffer, mesh.mIndexOffset * sizeof(std::uint32_t), mesh.mIndices.data(),
                mesh.mIndices.size() * sizeof(std::uint32_t));
        }
        for (const PendingInstances& pending : work.mInstances)
            gl.upload(gl.mInstanceBuffer, pending.mFirst * sizeof(GpuInstance), pending.mRecords.data(),
                pending.mRecords.size() * sizeof(GpuInstance));
        for (const PendingSkin& skin : work.mSkins)
        {
            gl.upload(gl.mSkinGroupBuffer, skin.mFirst * sizeof(std::uint32_t), skin.mGroups.data(),
                skin.mGroups.size() * sizeof(std::uint32_t));
            gl.upload(gl.mSkinRangeBuffer, skin.mGroupFirst * sizeof(GpuSkinGroup), skin.mGroupRanges.data(),
                skin.mGroupRanges.size() * sizeof(GpuSkinGroup));
            gl.upload(gl.mSkinInfluenceBuffer, skin.mInfluenceFirst * sizeof(GpuSkinInfluence), skin.mInfluences.data(),
                skin.mInfluences.size() * sizeof(GpuSkinInfluence));
        }
        for (PendingMaterial& material : work.mMaterials)
        {
            for (unsigned int i = 0; i < sMapCount; ++i)
                material.mRecord.mMaps[i]
                    = material.mTextures[i] != nullptr ? gl.handle(state, *material.mTextures[i]) : 0;
            gl.upload(
                gl.mMaterialBuffer, material.mIndex * sizeof(GpuMaterial), &material.mRecord, sizeof(GpuMaterial));
        }
        mCompletedWork += static_cast<unsigned int>(
            work.mMeshes.size() + work.mInstances.size() + work.mSkins.size() + work.mMaterials.size());
        if (work.mBatchesRebuilt)
        {
            const std::vector<GpuBatch>& batches = work.mBatches;
            const std::size_t batchBytes = std::max<std::size_t>(batches.size(), 1) * sizeof(GpuBatch);
            gl.grow(gl.mBatchBuffer, gl.mBatchCapacity, batchBytes, GL_DYNAMIC_COPY);
            if (!batches.empty())
                gl.upload(gl.mBatchBuffer, 0, batches.data(), batches.size() * sizeof(GpuBatch));
            gl.mBatchCount = static_cast<std::uint32_t>(batches.size());
            if (gl.mCommandBuffer != 0)
                ext.glDeleteBuffers(1, &gl.mCommandBuffer);
            ext.glGenBuffers(1, &gl.mCommandBuffer);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, gl.mCommandBuffer);
            ext.glBufferData(GL_COPY_WRITE_BUFFER, std::max<std::size_t>(batches.size(), 1) * sizeof(GpuCommand),
                nullptr, GL_DYNAMIC_COPY);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            if (gl.mCountBuffer == 0)
                ext.glGenBuffers(1, &gl.mCountBuffer);
            ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl.mCountBuffer);
            ext.glBufferData(GL_SHADER_STORAGE_BUFFER, std::max<std::size_t>(batches.size(), 1) * sizeof(std::uint32_t),
                nullptr, GL_DYNAMIC_COPY);
            ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }

    void GpuScene::compile(osg::State& state)
    {
        PendingWork work;
        std::vector<osg::ref_ptr<osg::Program>> programs;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            takePendingWork(work, sUploadsPerFrame);
            const std::size_t count = std::min<std::size_t>(mProgramsToLink.size(), sLinksPerFrame);
            programs.assign(mProgramsToLink.end() - count, mProgramsToLink.end());
            mProgramsToLink.resize(mProgramsToLink.size() - count);
        }
        upload(state, work);
        for (const osg::ref_ptr<osg::Program>& program : programs)
        {
            program->compileGLObjects(state);
            ++mCompletedWork;
        }
    }

    namespace
    {
        class CompileOperation : public osg::GraphicsOperation
        {
        public:
            CompileOperation(GpuScene& scene, unsigned int target)
                : osg::GraphicsOperation("GpuSceneCompile", true)
                , mScene(scene)
                , mTarget(target)
            {
            }

            void operator()(osg::GraphicsContext* context) override
            {
                mScene.compile(*context->getState());
                if (mScene.getCompletedWork() >= mTarget || mScene.getPendingWork() == 0)
                    setKeep(false);
            }

        private:
            GpuScene& mScene;
            unsigned int mTarget;
        };
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createCompileOperation(unsigned int target)
    {
        mCompletedWork = 0;
        return new CompileOperation(*this, target);
    }

    bool GpuScene::applySkin(osg::State& state, const std::vector<SkinUpdate>& skins)
    {
        if (skins.empty())
            return true;

        thread_local std::vector<GpuSkinTask> tasks;
        thread_local std::vector<GpuSkinMatrix> matrices;
        thread_local std::vector<SkinPose> poses;
        tasks.clear();
        matrices.clear();
        poses.clear();
        std::uint32_t maxVertices = 0;
        std::size_t matrixCount = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const SkinUpdate& skin : skins)
            {
                if (skin.mMesh >= mMeshes.size() || skin.mPose.mCount == 0)
                    continue;
                const Mesh& mesh = mMeshes[skin.mMesh];
                const auto registered = mSkinMeshes.find(skin.mMesh);
                const std::uint64_t last = static_cast<std::uint64_t>(mesh.mBaseVertex) + mesh.mVertexCount;
                if (mesh.mBaseVertex < 0 || last > mVertices.mExtent || registered == mSkinMeshes.end()
                    || registered->second.mBoneCount + 1 != skin.mPose.mCount)
                    return false;
                if (mesh.mGeometry == nullptr || !mUpdatedMeshes.insert(mesh.mGeometry.get()).second)
                    continue;
                constexpr std::size_t maxMatrices = std::numeric_limits<std::uint32_t>::max();
                if (matrixCount > maxMatrices - skin.mPose.mCount)
                    return false;
                unsigned int flags = 0;
                if (mesh.mGeometry->getNormalArray() != nullptr)
                    flags |= 1;
                if (dynamic_cast<const osg::Vec4Array*>(mesh.mGeometry->getTexCoordArray(7)) != nullptr)
                    flags |= 2;
                const GpuSkinTask task{ static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount,
                    static_cast<std::uint32_t>(matrixCount), flags };
                tasks.push_back(task);
                poses.push_back(skin.mPose);
                matrixCount += skin.mPose.mCount;
                maxVertices = std::max(maxVertices, mesh.mVertexCount);
            }
        }
        if (tasks.empty())
            return true;
        matrices.reserve(matrixCount);
        const unsigned int frame = poses.front().mFrame;
        SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
        {
            std::lock_guard<std::mutex> lock(slot.mMutex);
            if (!slot.mValid || slot.mFrame != frame)
                return false;
            for (const SkinPose& pose : poses)
            {
                const std::uint64_t last = static_cast<std::uint64_t>(pose.mFirst) + pose.mCount;
                if (pose.mFrame != frame || last > slot.mMatrices.size())
                    return false;
                matrices.insert(matrices.end(), slot.mMatrices.begin() + pose.mFirst, slot.mMatrices.begin() + last);
            }
        }

        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;
        if (gl.mSkinProgram == nullptr)
        {
            gl.mSkinProgram = new osg::Program;
            gl.mSkinProgram->addShader(mShaderManager.getShader("gpuscene_skin.comp"));
        }
        state.applyAttribute(gl.mSkinProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mSkinProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;

        auto stream = [&](GLuint& buffer, const void* data, std::size_t size) {
            if (buffer == 0)
                ext.glGenBuffers(1, &buffer);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            ext.glBufferData(GL_COPY_WRITE_BUFFER, size, data, GL_STREAM_DRAW);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        };
        stream(gl.mSkinMatrixBuffer, matrices.data(), matrices.size() * sizeof(GpuSkinMatrix));
        stream(gl.mSkinTaskBuffer, tasks.data(), tasks.size() * sizeof(GpuSkinTask));
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, gl.mSkinSourceBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, gl.mVertexBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, gl.mSkinGroupBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, gl.mSkinTaskBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, gl.mSkinMatrixBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, gl.mSkinRangeBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, gl.mSkinInfluenceBuffer);
        ext.glDispatchCompute(
            (maxVertices + sSkinGroupSize - 1) / sSkinGroupSize, static_cast<GLuint>(tasks.size()), 1);
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        return true;
    }

    bool GpuScene::prepareDynamic(osg::State& state, ViewRecord& view, std::vector<std::uint32_t>& chunkGenerations)
    {
        Gl& gl = *mGl;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        if (mUpdateFrame != frame)
        {
            mUpdateFrame = frame;
            mUpdatedMeshes.clear();
        }
        if (mSkinSupported.load(std::memory_order_relaxed) && !applySkin(state, view.mSkins))
        {
            Log(Debug::Error) << "GPU skin shader failed";
            mSkinSupported.store(false, std::memory_order_relaxed);
        }
        gl.mDynamicRanges.assign(mBuckets.size(), { 0, 0 });
        if (view.mDynamic.empty())
            return true;

        std::vector<std::array<Id, 3>> order;
        order.reserve(view.mDynamic.size());
        for (Id i = 0; i < view.mDynamic.size(); ++i)
            order.push_back({ view.mDynamic[i].mBucket, view.mDynamic[i].mMesh, i });
        std::sort(order.begin(), order.end());

        struct MeshUpload
        {
            osg::ref_ptr<const osg::Geometry> mGeometry;
            std::uint32_t mVertexOffset;
            std::uint32_t mVertexCount;
            std::uint32_t mIndexCount;
        };
        std::vector<MeshUpload> meshUploads;
        std::vector<GpuInstance> instances;
        std::vector<GpuCommand> commands;
        instances.reserve(view.mDynamic.size());

        const std::uint32_t chunkBase = static_cast<std::uint32_t>(chunkGenerations.size());
        std::size_t first = 0;
        while (first < order.size())
        {
            const DynamicInstance& head = view.mDynamic[order[first][2]];
            std::size_t last = first + 1;
            while (last < order.size() && order[last][0] == head.mBucket && order[last][1] == head.mMesh)
                ++last;

            Mesh mesh;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (head.mMesh >= mMeshes.size() || mMeshes[head.mMesh].mGeometry == nullptr)
                {
                    first = last;
                    continue;
                }
                mesh = mMeshes[head.mMesh];
            }

            const std::uint32_t baseInstance = static_cast<std::uint32_t>(instances.size());
            for (std::size_t i = first; i < last; ++i)
            {
                const DynamicInstance& instance = view.mDynamic[order[i][2]];
                GpuInstance record{};
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col)
                        record.mRows[row][col] = instance.mWorld(col, row);
                record.mMeta[1] = chunkBase + static_cast<std::uint32_t>(instances.size());
                record.mMaterial[0] = instance.mMaterial;
                record.mMaterial[1] = sInvalid;
                instances.push_back(record);
                if (instance.mUpdateMesh && mUpdatedMeshes.insert(instance.mGeometry.get()).second)
                    meshUploads.push_back(MeshUpload{ instance.mGeometry, static_cast<std::uint32_t>(mesh.mBaseVertex),
                        mesh.mVertexCount, mesh.mIndexCount });
            }

            const std::uint32_t bucket = head.mBucket;
            if (bucket < gl.mDynamicRanges.size())
            {
                if (gl.mDynamicRanges[bucket].second == 0)
                    gl.mDynamicRanges[bucket].first = static_cast<std::uint32_t>(commands.size());
                ++gl.mDynamicRanges[bucket].second;
            }
            const GpuCommand command{ mesh.mIndexCount, static_cast<std::uint32_t>(instances.size()) - baseInstance,
                mesh.mFirstIndex, mesh.mBaseVertex, baseInstance };
            commands.push_back(command);
            first = last;
        }

        if (instances.empty())
            return true;
        chunkGenerations.resize(chunkBase + instances.size(), 0);
        std::vector<std::uint32_t> visible(instances.size());
        std::iota(visible.begin(), visible.end(), 0);

        osg::GLExtensions& ext = *gl.mExt;
        auto stream = [&](GLuint& buffer, const void* data, std::size_t size) {
            if (buffer == 0)
                ext.glGenBuffers(1, &buffer);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            ext.glBufferData(GL_COPY_WRITE_BUFFER, std::max<std::size_t>(size, 4), data, GL_STREAM_DRAW);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        };
        stream(gl.mDynamicInstanceBuffer, instances.data(), instances.size() * sizeof(GpuInstance));
        stream(gl.mDynamicVisibleBuffer, visible.data(), visible.size() * sizeof(std::uint32_t));
        stream(gl.mDynamicCommandBuffer, commands.data(), commands.size() * sizeof(GpuCommand));

        for (const MeshUpload& pending : meshUploads)
        {
            std::vector<std::uint8_t> vertices;
            std::vector<std::uint32_t> indices;
            if (!packGeometry(*pending.mGeometry, vertices, indices)
                || vertices.size() != pending.mVertexCount * sizeof(GpuVertex) || indices.size() != pending.mIndexCount)
                continue;
            gl.upload(gl.mVertexBuffer, pending.mVertexOffset * sizeof(GpuVertex), vertices.data(), vertices.size());
        }
        gl.createVao(state, true);
        return true;
    }

    bool GpuScene::dispatch(osg::State& state, const ViewRecord& view, std::uint32_t instanceExtent,
        const std::vector<std::uint32_t>& chunkGenerations)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;

        if (gl.mCullProgram == nullptr)
        {
            gl.mCullProgram = new osg::Program;
            gl.mCullProgram->addShader(mShaderManager.getShader("gpuscene_cull.comp"));
            gl.mCommandProgram = new osg::Program;
            gl.mCommandProgram->addShader(mShaderManager.getShader("gpuscene_commands.comp"));
            gl.mInstanceCount = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "instanceCount");
            gl.mChunkCount = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "chunkCount");
            gl.mPlaneCount = new osg::Uniform(osg::Uniform::INT, "planeCount");
            gl.mPlanes = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "planes", sMaxPlanes);
            gl.mPixelSizeVector = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "pixelSizeVector");
            gl.mSmallFeaturePixelSize = new osg::Uniform(osg::Uniform::FLOAT, "smallFeaturePixelSize");
            gl.mViewPoint = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "viewPoint");
            gl.mLodScale = new osg::Uniform(osg::Uniform::FLOAT, "lodScale");
            gl.mBatchCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "batchCount");
        }

        if (gl.mChunkBuffer == 0)
            ext.glGenBuffers(1, &gl.mChunkBuffer);
        if (gl.mVertexBuffer == 0 || gl.mCommandBuffer == 0)
            return true;
        gl.createVao(state);

        ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl.mChunkBuffer);
        ext.glBufferData(GL_SHADER_STORAGE_BUFFER,
            std::max<std::size_t>(chunkGenerations.size(), 1) * sizeof(std::uint32_t),
            chunkGenerations.empty() ? nullptr : chunkGenerations.data(), GL_STREAM_DRAW);
        ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        gl.zero(gl.mCountBuffer);

        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, gl.mBatchBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, gl.mChunkBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, gl.mCountBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, gl.mVisibleBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, gl.mCommandBuffer);
        if (gl.mMaterialBuffer != 0)
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, gl.mMaterialBuffer);

        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        state.applyAttribute(gl.mCullProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mCullProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        gl.mInstanceCount->set(instanceExtent);
        gl.mChunkCount->set(static_cast<unsigned int>(chunkGenerations.size()));
        gl.mPlaneCount->set(static_cast<int>(view.mPlanes.size()));
        for (unsigned int i = 0; i < view.mPlanes.size(); ++i)
            gl.mPlanes->setElement(i, view.mPlanes[i]);
        gl.mPixelSizeVector->set(view.mPixelSizeVector);
        gl.mSmallFeaturePixelSize->set(view.mSmallFeaturePixelSize);
        gl.mViewPoint->set(view.mViewPoint);
        gl.mLodScale->set(view.mLodScale);
        for (const osg::ref_ptr<osg::Uniform>& uniform : { gl.mInstanceCount, gl.mChunkCount, gl.mPlaneCount,
                 gl.mPlanes, gl.mPixelSizeVector, gl.mSmallFeaturePixelSize, gl.mViewPoint, gl.mLodScale })
            pcp->apply(*uniform);
        ext.glDispatchCompute((instanceExtent + sWorkGroupSize - 1) / sWorkGroupSize, 1, 1);
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        state.applyAttribute(gl.mCommandProgram.get());
        pcp = gl.mCommandProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        gl.mBatchCountUniform->set(gl.mBatchCount);
        pcp->apply(*gl.mBatchCountUniform);
        ext.glDispatchCompute((gl.mBatchCount + sWorkGroupSize - 1) / sWorkGroupSize, 1, 1);
        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        return true;
    }

    void GpuScene::draw(osg::RenderInfo& renderInfo, Id dispatcherId)
    {
        osg::State& state = *renderInfo.getState();
        if (state.getFrameStamp() == nullptr || !isEnabled())
            return;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        const osg::Camera* camera = renderInfo.getCurrentCamera();

        bool needDispatch = false;
        ViewRecord view;
        std::vector<std::uint32_t> chunkGenerations;
        PendingWork work;
        std::uint32_t instanceExtent = 0;
        struct DrawRange
        {
            Id mBucket;
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<const osg::Program> mProgram;
            std::uint32_t mOffset;
            std::uint32_t mCount;
        };
        std::vector<DrawRange> ranges;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto found = mViews.find(ViewKey(frame, camera));
            if (found == mViews.end() || !found->second.mHasParams || dispatcherId >= mDispatchers.size())
                return;
            ViewRecord& record = found->second;
            if (!record.mDispatched)
            {
                record.mDispatched = true;
                needDispatch = true;
                view = record;
                chunkGenerations.assign(mChunks.size(), 0);
                for (const auto& [slot, generation] : record.mChunks)
                    if (slot < chunkGenerations.size() && mChunks[slot].mGeneration == generation)
                        chunkGenerations[slot] = generation;
                takePendingWork(work, std::numeric_limits<std::size_t>::max());
                instanceExtent = mInstances.mExtent;
            }
            for (const Id bucketId : mDispatchers[dispatcherId].mBuckets)
            {
                const Bucket& bucket = mBuckets[bucketId];
                ranges.push_back(DrawRange{
                    bucketId, bucket.mStateSet, bucket.mProgram, bucket.mCommandOffset, bucket.mCommandCount });
            }
        }

        if (needDispatch)
        {
            upload(state, work);
            prepareDynamic(state, view, chunkGenerations);
            const bool dispatched = dispatch(state, view, instanceExtent, chunkGenerations);
            if (!dispatched)
            {
                Log(Debug::Error) << "GPU scene compute shaders failed to link";
                mSupported = false;
            }
        }

        Gl& gl = *mGl;
        if (!mSupported)
            return;

        osg::GLExtensions& ext = *gl.mExt;
        for (const DrawRange& range : ranges)
        {
            const std::pair<std::uint32_t, std::uint32_t> dynamicRange = range.mBucket < gl.mDynamicRanges.size()
                ? gl.mDynamicRanges[range.mBucket]
                : std::pair<std::uint32_t, std::uint32_t>();
            if (range.mCount == 0 && dynamicRange.second == 0)
                continue;

            state.pushStateSet(range.mStateSet);
            state.apply();
            const osg::Program::PerContextProgram* appliedPcp = state.getLastAppliedProgramObject();
            const osg::Program* applied = appliedPcp != nullptr ? appliedPcp->getProgram() : nullptr;
            bool canDraw = applied != nullptr;
            osg::ref_ptr<osg::StateSet> casting;
            if (applied != nullptr && applied != range.mProgram)
            {
                osg::ref_ptr<osg::Program> instanced = mShaderManager.getInstancedProgram(
                    *applied, mMaterialTable ? Shader::MaterialMode::Bindless : Shader::MaterialMode::Legacy);
                if (instanced == nullptr)
                    canDraw = false;
                else if (instanced != applied)
                {
                    casting = gl.mCastingStateSets[applied];
                    if (casting == nullptr)
                    {
                        casting = new osg::StateSet;
                        casting->setAttributeAndModes(instanced,
                            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                        gl.mCastingStateSets[applied] = casting;
                    }
                    state.pushStateSet(casting);
                    state.apply();
                }
            }

            const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            if (canDraw && range.mCount != 0 && gl.mVao != 0)
            {
                ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
                state.bindVertexArrayObject(gl.mVao);
                ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gl.mCommandBuffer);
                ext.glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<std::size_t>(range.mOffset) * sizeof(GpuCommand)),
                    range.mCount, 0);
            }
            if (canDraw && dynamicRange.second != 0 && gl.mDynamicVao != 0)
            {
                ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mDynamicInstanceBuffer);
                state.bindVertexArrayObject(gl.mDynamicVao);
                ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gl.mDynamicCommandBuffer);
                ext.glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<std::size_t>(dynamicRange.first) * sizeof(GpuCommand)),
                    dynamicRange.second, 0);
            }
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
            state.bindVertexArrayObject(previousVao);

            if (casting != nullptr)
                state.popStateSet();
            state.popStateSet();
        }
        state.apply();
    }
}
