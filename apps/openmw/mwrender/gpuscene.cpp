#include "gpuscene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <tuple>

#include <osg/AlphaFunc>
#include <osg/Billboard>
#include <osg/BufferObject>
#include <osg/Callback>
#include <osg/ColorMask>
#include <osg/Drawable>
#include <osg/FrameBufferObject>
#include <osg/FrontFace>
#include <osg/GLDefines>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/LOD>
#include <osg/Multisample>
#include <osg/PolygonMode>
#include <osg/PrimitiveSet>
#include <osg/Sampler>
#include <osg/Sequence>
#include <osg/State>
#include <osg/Stats>
#include <osg/Switch>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
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
#include <components/nifosg/controller.hpp>
#include <components/sceneutil/geometrybatch.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/shadowsbin.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include "gpuscenekeys.hpp"
#include "postprocessor.hpp"
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
#ifndef GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#endif
#ifndef GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS
#define GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS 0x90D6
#endif
#ifndef GL_TEXTURE_SAMPLES
#define GL_TEXTURE_SAMPLES 0x9106
#endif
#ifndef GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS
#define GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS 0x90DA
#endif
#ifndef GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS
#define GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS 0x90DC
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
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

    struct GpuScene::RayRequest
    {
        RayId mId = sInvalidRay;
        osg::Vec3f mFrom;
        osg::Vec3f mTo;
        bool mIgnorePlayer = false;
        std::vector<osg::observer_ptr<SceneUtil::PositionAttitudeTransform>> mIgnore;
    };

    struct GpuScene::RaySkin
    {
        Id mMesh = sInvalid;
        osg::ref_ptr<const osg::Geometry> mGeometry;
        std::uint32_t mFirst = 0;
        std::uint32_t mCount = 0;
    };

    struct GpuScene::RayBatch
    {
        struct GpuRay
        {
            float mOrigin[4];
            float mDelta[4];
        };

        struct GpuTask
        {
            std::uint32_t mMeta[4];
            float mRows[3][4];
        };

        struct GpuHit
        {
            std::uint32_t mRatio;
            std::uint32_t mTask;
            std::uint32_t mTriangle;
            std::uint32_t mBaryX;
            std::uint32_t mBaryY;
            std::uint32_t mNormalX;
            std::uint32_t mNormalY;
            std::uint32_t mNormalZ;
        };

        static_assert(sizeof(GpuRay) == 32);
        static_assert(sizeof(GpuTask) == 64);
        static_assert(sizeof(GpuHit) == 32);

        std::vector<RayRequest> mRequests;
        std::vector<GpuRay> mRays;
        std::vector<GpuTask> mTasks;
        std::vector<osg::ref_ptr<SceneUtil::PositionAttitudeTransform>> mActors;
        std::vector<RaySkin> mSkins;
        std::vector<GpuSkinMatrix> mSkinMatrices;
        std::uint32_t mMaxTriangles = 0;
        bool mValid = true;
    };

    struct GpuScene::RayState
    {
        std::mutex mMutex;
        std::vector<RayHit> mCompleted;
        bool mAlive = true;
    };

    class GpuScene::RayReadOperation : public osg::GraphicsOperation
    {
    public:
        RayReadOperation(std::shared_ptr<RayState> state, std::unique_ptr<RayBatch> batch, osg::GLExtensions& ext,
            GLuint buffer, GLsync fence)
            : osg::GraphicsOperation("GpuRayRead", true)
            , mState(std::move(state))
            , mBatch(std::move(batch))
            , mExt(ext)
            , mBuffer(buffer)
            , mFence(fence)
        {
        }

        void operator()(osg::GraphicsContext*) override
        {
            if (mBatch == nullptr)
                return;
            const GLenum wait = mExt.glClientWaitSync(mFence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (wait == GL_TIMEOUT_EXPIRED)
                return;
            bool valid = wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED;
            std::vector<RayBatch::GpuHit> hits(mBatch->mRequests.size());
            mExt.glBindBuffer(GL_SHADER_STORAGE_BUFFER, mBuffer);
            const std::size_t bytes = hits.size() * sizeof(RayBatch::GpuHit);
            const void* mapped
                = valid ? mExt.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytes, GL_MAP_READ_BIT) : nullptr;
            if (mapped != nullptr)
                std::memcpy(hits.data(), mapped, bytes);
            else
                valid = false;
            if (mapped != nullptr && !mExt.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER))
                valid = false;
            mExt.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            mExt.glDeleteSync(mFence);
            mExt.glDeleteBuffers(1, &mBuffer);
            finishRay(mState, *mBatch, hits.data(), valid);
            mBatch.reset();
            setKeep(false);
        }

    private:
        std::shared_ptr<RayState> mState;
        std::unique_ptr<RayBatch> mBatch;
        osg::GLExtensions& mExt;
        GLuint mBuffer;
        GLsync mFence;
    };

    /// Runs after draws on the context thread.
    class GpuScene::RetireOperation : public osg::GraphicsOperation
    {
    public:
        explicit RetireOperation(GpuScene& scene)
            : osg::GraphicsOperation("GpuSceneRetire", true)
            , mScene(&scene)
        {
        }

        void operator()(osg::GraphicsContext* context) override
        {
            if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                scene->finishFrame(*context->getState());
        }

        void detach()
        {
            mScene.store(nullptr, std::memory_order_release);
            setKeep(false);
        }

    private:
        std::atomic<GpuScene*> mScene;
    };

    namespace
    {
        using GpuVertex = SceneUtil::PackedVertex;
        using SceneUtil::packGeometry;

        constexpr const char* sMaterialUniforms[] = { "material.diffuse", "material.ambient", "material.specular",
            "material.emission", "material.shininess", "material.emissiveMult", "material.specStrength",
            "material.vertexColorMode", "alphaRef", "envMapColor", "envMapLumaBias", "bumpMapMatrix", "texMat0",
            "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6", "texMat7", "texMat8", "texMat9" };
        constexpr const char* sTexMatNames[]
            = { "texMat0", "texMat1", "texMat2", "texMat3", "texMat4", "texMat5", "texMat6", "texMat7" };
        constexpr GLint sMaxStorageBinding = 23;
        constexpr GLint sVertexStorageBlocks = 6;
        constexpr GLint sFragmentStorageBlocks = 4;

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

        constexpr unsigned int sFlagSmallFeature = 1;
        constexpr unsigned int sFlagLod = 2;
        constexpr unsigned int sWorkGroupSize = 256;
        /// Projected size that seeds the depth prepass.
        constexpr float sOccluderPixelSize = 256.f;
        constexpr unsigned int sSkinGroupSize = 64;
        constexpr unsigned int sMaxPlanes = 16;
        /// One chunk mask bit per view.
        constexpr std::size_t sMaxGroupViews = 32;

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

        bool prepassBucket(const osg::StateSet& state)
        {
            return state.getAttribute(osg::StateAttribute::ALPHAFUNC) == nullptr
                && (state.getMode(GL_BLEND) & osg::StateAttribute::ON) == 0 && state.getBinNumber() <= 0
                && state.getRenderingHint() < osg::StateSet::TRANSPARENT_BIN;
        }

        osg::ref_ptr<osg::StateSet> makePrepassState(const osg::StateSet& source)
        {
            osg::ref_ptr<osg::StateSet> result = new osg::StateSet;
            for (const auto& [mode, value] : source.getModeList())
                result->setMode(mode, value);
            for (const auto& [typeMember, attribute] : source.getAttributeList())
            {
                const osg::StateAttribute::Type type = typeMember.first;
                if (type == osg::StateAttribute::PROGRAM || type == osg::StateAttribute::MATERIAL
                    || type == osg::StateAttribute::COLORMASK || type == osg::StateAttribute::POLYGONMODE)
                    continue;
                result->setAttribute(const_cast<osg::StateAttribute*>(attribute.first.get()), attribute.second);
            }
            return result;
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
                if (dynamic_cast<const SceneUtil::StateSetUpdater*>(callback) != nullptr
                    || dynamic_cast<const NifOsg::VisController*>(callback) != nullptr)
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

        osg::BoundingBox worldBound(const osg::BoundingBox& local, const osg::Matrixf& world)
        {
            osg::BoundingBox result;
            for (unsigned int corner = 0; corner < 8; ++corner)
                result.expandBy(local.corner(corner) * world);
            return result;
        }

        bool intersects(const osg::BoundingBox& box, const osg::Vec3f& from, const osg::Vec3f& to)
        {
            if (!box.valid())
                return false;
            const osg::Vec3f delta = to - from;
            float near = 0.f;
            float far = 1.f;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(delta[axis]) < 1e-7f)
                {
                    if (from[axis] < box._min[axis] || from[axis] > box._max[axis])
                        return false;
                    continue;
                }
                const float inverse = 1.f / delta[axis];
                float first = (box._min[axis] - from[axis]) * inverse;
                float last = (box._max[axis] - from[axis]) * inverse;
                if (first > last)
                    std::swap(first, last);
                near = std::max(near, first);
                far = std::min(far, last);
                if (near > far)
                    return false;
            }
            return true;
        }

        void restoreProxy(osg::Node& node)
        {
            if (node.getNodeMask() == MWRender::Mask_GpuProxy)
                node.setNodeMask(~0u);
            osg::Group* group = node.asGroup();
            if (group != nullptr)
                for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                    restoreProxy(*group->getChild(i));
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
                osg::ref_ptr<const osg::StateSet> live;
                if (cullCallback != nullptr && !root)
                {
                    if (node->asDrawable() == nullptr)
                        live = mScene.liveStateSet(*cullCallback);
                    if (live == nullptr)
                    {
                        mOut.mLegacy = true;
                        return;
                    }
                }

                const std::size_t depth = mStates.size();
                if (node->getStateSet() != nullptr)
                    mStates.push_back(node->getStateSet());
                if (live != nullptr)
                {
                    mStates.push_back(live.get());
                    mLive.push_back(live.get());
                }

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
                if (live != nullptr)
                    mLive.pop_back();
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
                const GpuScene::MaterialUpdate updates
                    = mLive.empty() ? GpuScene::MaterialUpdate::None : GpuScene::MaterialUpdate::Record;
                const GpuScene::Surface surface
                    = mesh != GpuScene::sInvalid ? mScene.getSurface(mStates, 0, updates) : GpuScene::Surface();
                mStates.resize(depth);
                if (mesh == GpuScene::sInvalid || surface.mBucket == GpuScene::sInvalid)
                {
                    mOut.mLegacy = true;
                    return;
                }

                mOut.mClaimed.insert(drawable);
                for (const osg::StateSet* live : mLive)
                    mScene.bindLiveMaterial(*live, surface.mMaterial);
                mOut.mGeometries.push_back(GpuGeometry{ geometry, surface, matrix, geometry->getBoundingBox(), lod,
                    lodChild, lodCenter, lodScale, drawable });
            }

            GpuScene& mScene;
            osg::Node::NodeMask mMask;
            bool mBakeBillboards;
            GpuTemplate& mOut;
            std::vector<const osg::StateSet*> mStates;
            std::vector<const osg::StateSet*> mLive;
        };

        class CaptureVisitor : public osg::NodeVisitor
        {
        public:
            struct CapturedRig
            {
                SceneUtil::RigGeometry* mRig;
                std::vector<const osg::StateSet*> mStates;
                bool mUpdater;
            };

            struct Part
            {
                osg::Drawable* mDrawable;
                osg::Group* mParent;
                std::vector<const osg::StateSet*> mStates;
                std::vector<osg::ref_ptr<osg::Transform>> mTransforms;
            };

            explicit CaptureVisitor(osg::Node* skip, bool actor = false)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mSkip(skip)
                , mActor(actor)
            {
            }

            void apply(osg::Node& node) override
            {
                if (&node == mSkip)
                    return;
                const std::size_t depth = mStates.size();
                if (node.getStateSet() != nullptr)
                    mStates.push_back(node.getStateSet());
                const bool updater = hasUpdater(node.getUpdateCallback()) || hasUpdater(node.getCullCallback());
                bool conditional = dynamic_cast<osg::Billboard*>(&node) != nullptr
                    || dynamic_cast<NifOsg::AutoTransform*>(&node) != nullptr
                    || dynamic_cast<osg::Switch*>(&node) != nullptr || dynamic_cast<osg::LOD*>(&node) != nullptr
                    || dynamic_cast<osg::Sequence*>(&node) != nullptr
                    || (node.getNodeMask() != ~0u && node.getNodeMask() != Mask_GpuProxy);
                for (const osg::Callback* callback = node.getCullCallback(); callback != nullptr;
                    callback = callback->getNestedCallback())
                    conditional = conditional || dynamic_cast<const SceneUtil::LightListCallback*>(callback) == nullptr;
                mUpdaterDepth += updater;
                mConditionalDepth += conditional;
                traverse(node);
                mConditionalDepth -= conditional;
                mUpdaterDepth -= updater;
                mStates.resize(depth);
            }

            void apply(osg::Drawable& drawable) override
            {
                const std::size_t depth = mStates.size();
                if (drawable.getStateSet() != nullptr)
                    mStates.push_back(drawable.getStateSet());
                const bool updater = hasUpdater(drawable.getUpdateCallback()) || hasUpdater(drawable.getCullCallback());
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    mRigs.push_back(CapturedRig{ rig, mStates, mUpdaterDepth != 0 || updater });
                else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    mMorphs.push_back(morph);
                else if (osg::Geometry* geometry = drawable.asGeometry())
                    mGeometries.push_back(geometry);
                if (mActor && !mUpdaterDepth && !updater && !mConditionalDepth
                    && (drawable.getNodeMask() == ~0u || drawable.getNodeMask() == Mask_GpuProxy)
                    && drawable.getDrawCallback() == nullptr && foreignCullCallback(drawable) == nullptr)
                {
                    Part part{ &drawable, nullptr, mStates, {} };
                    for (osg::Node* node : getNodePath())
                    {
                        if (auto* group = node->asGroup())
                            part.mParent = group;
                        if (auto* transform = node->asTransform())
                            part.mTransforms.emplace_back(transform);
                    }
                    mParts.push_back(std::move(part));
                }
                mStates.resize(depth);
            }

            std::vector<Part> mParts;
            std::vector<osg::Geometry*> mGeometries;
            std::vector<CapturedRig> mRigs;
            std::vector<SceneUtil::MorphGeometry*> mMorphs;

        private:
            osg::Node* mSkip;
            bool mActor;
            std::vector<const osg::StateSet*> mStates;
            unsigned int mUpdaterDepth = 0;
            unsigned int mConditionalDepth = 0;
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
                    scene->update(nv->getFrameStamp()->getFrameNumber());
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
            , mLit(copy.mLit)
            , mBound(copy.mBound)
        {
        }
        META_Node(MWRender, GpuChunkNode)

        GpuChunkNode(
            GpuScene& scene, GpuScene::Id slot, GpuScene::Id generation, bool lit, const osg::BoundingSphere& bound)
            : mScene(&scene)
            , mSlot(slot)
            , mGeneration(generation)
            , mLit(lit)
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
        GpuScene::Id getGeneration() const { return mGeneration; }
        bool isLit() const { return mLit; }
        void setBound(const osg::BoundingSphere& bound)
        {
            mBound = bound;
            dirtyBound();
        }
        static constexpr std::size_t sNone = std::numeric_limits<std::size_t>::max();
        std::size_t mResident = sNone;

        void traverse(osg::NodeVisitor& nv) override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene == nullptr || nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
                return;
            osgUtil::CullVisitor& cv = static_cast<osgUtil::CullVisitor&>(nv);
            scene->markChunk(cv, mSlot, mGeneration, mLit);
            if (cv.getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR && mBound.valid())
            {
                osg::BoundingBox box;
                box.expandBy(mBound);
                cv.updateCalculatedNearFar(*cv.getModelViewMatrix(), box);
            }
        }

        osg::BoundingSphere computeBound() const override { return mBound; }

    private:
        std::atomic<GpuScene*> mScene{ nullptr };
        GpuScene::Id mSlot = 0;
        GpuScene::Id mGeneration = 0;
        bool mLit = false;
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
                setUseVertexArrayObject(false);
            }

            osg::BoundingBox computeBoundingBox() const override { return osg::BoundingBox(); }

            void drawImplementation(osg::RenderInfo& renderInfo) const override
            {
                if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                    scene->draw(renderInfo, mDispatcher);
            }

            void releaseGLObjects(osg::State* state) const override
            {
                if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                    scene->releaseGLObjects(state);
                osg::Drawable::releaseGLObjects(state);
            }

            void detach() { mScene.store(nullptr, std::memory_order_release); }

        private:
            std::atomic<GpuScene*> mScene{ nullptr };
            GpuScene::Id mDispatcher = 0;
        };
    }

    namespace
    {
        /// One object per sampler key.
        class GpuSampler : public osg::Sampler
        {
        public:
            GLuint object(unsigned int context) const { return _PCsampler[context]; }

            // osg::Sampler compares every instance equal.
            int compare(const osg::StateAttribute& sa) const override
            {
                COMPARE_StateAttribute_Types(GpuSampler, sa);
                if (this < &rhs)
                    return -1;
                if (this > &rhs)
                    return 1;
                return 0;
            }

        protected:
            ~GpuSampler() override = default;
        };

        /// Unbinds the unit when array states pop.
        class SamplerReset final : public osg::Sampler
        {
        public:
            void apply(osg::State& state) const override
            {
                osg::GLExtensions* ext = state.get<osg::GLExtensions>();
                if (ext != nullptr && ext->glBindSampler != nullptr)
                    ext->glBindSampler(state.getActiveTextureUnit(), 0);
            }

        protected:
            ~SamplerReset() override = default;
        };

        /// Raw array texture unit.
        constexpr unsigned int sArrayUnit = 31;

        /// Units the reset sampler covers.
        constexpr unsigned int sSamplerUnits = 16;

        void clearErrors()
        {
            for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
            {
            }
        }

        void resetUnpack()
        {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#if !defined(OSG_GLES1_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE)
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
            glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
#endif
        }
    }

    struct GpuScene::Textures
    {
        using StorageKey = MWRender::StorageKey;
        using SamplerKey = MWRender::SamplerKey;

        /// A fence guards pending layers.
        enum class LayerState : std::uint8_t
        {
            Used,
            Pending,
            Staged,
            Free,
        };

        struct Layer
        {
            osg::ref_ptr<osg::Image> mImage;
            std::uint32_t mUsers = 0;
            std::uint64_t mRevision = 0;
            LayerState mState = LayerState::Used;
        };

        struct Pool;

        /// Binds the pool name the renderer owns.
        class ArrayTexture : public osg::Texture2DArray
        {
        public:
            explicit ArrayTexture(Pool& pool)
                : mPool(pool)
            {
            }

            void apply(osg::State& state) const override;
            int compare(const osg::StateAttribute& sa) const override;

        protected:
            ~ArrayTexture() override = default;

        private:
            Pool& mPool;
        };

        struct PoolGl
        {
            GLuint mName = 0;
            std::uint32_t mCapacity = 0;
            std::vector<std::uint64_t> mRevisions;
            std::vector<unsigned int> mModified;
        };

        struct Pool
        {
            StorageKey mKey;
            osg::ref_ptr<ArrayTexture> mTexture;
            std::vector<Layer> mLayers;
            std::map<const osg::Image*, std::uint32_t> mImages;
            std::map<unsigned int, PoolGl> mContexts;
            Id mId = sInvalid;
            std::uint32_t mCapacity = 0;
            std::uint32_t mUsers = 0;
            /// Bumped when the pool loses every user.
            std::uint64_t mLife = 0;
            bool mFailed = false;
            /// One upload warning per pool.
            bool mWarned = false;
            mutable std::mutex mMutex;
        };

        struct NameTicket
        {
            GLuint mName = 0;
            std::size_t mBytes = 0;
        };

        struct LayerTicket
        {
            Id mPool = sInvalid;
            std::uint32_t mLayer = 0;
            std::uint64_t mLife = 0;
        };

        using CopyImageFn = void(GL_APIENTRY*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint,
            GLint, GLint, GLsizei, GLsizei, GLsizei);

        /// Budget record; tiers appear only under pressure.
        struct Proxy
        {
            struct Tier
            {
                osg::ref_ptr<osg::Image> mImage;
                osg::ref_ptr<osg::Texture2D> mTexture;
                std::size_t mBytes = 0;
            };

            osg::ref_ptr<osg::Image> mImage;
            /// Tier zero; the scene owns it.
            osg::ref_ptr<osg::Texture2D> mSource;
            osg::ref_ptr<osg::Texture2D> mTemplate;
            osg::ref_ptr<osg::Texture2D> mTexture;
            StorageKey mKey;
            std::vector<Tier> mTiers;
            std::size_t mBytes = 0;
            std::uint32_t mUsers = 0;
            unsigned int mLastVisible = 0;
            unsigned int mCurrent = 0;
            unsigned int mDesired = 0;
            unsigned int mFloor = 0;
            bool mPrepared = false;
            bool mRetiring = false;
            std::vector<Id> mMaterials;
        };

        struct Reservation
        {
            Id mPool = sInvalid;
            std::uint32_t mLayer = 0;
        };

        /// Builds proxy tiers on a worker thread.
        class TierWork : public SceneUtil::WorkItem
        {
        public:
            void doWork() override { buildTiers(*this); }

            Id mProxy = sInvalid;
            osg::ref_ptr<osg::Image> mImage;
            osg::ref_ptr<osg::Texture2D> mTemplate;
            StorageKey mKey;
            unsigned int mFloor = 0;
            std::vector<Proxy::Tier> mTiers;

        protected:
            ~TierWork() override = default;
        };

        Textures(int budget, int maxLayers)
            : mBudgetSetting(budget)
            , mMaxLayers(std::max(maxLayers, 0))
        {
            constexpr std::size_t maxArrays = 1024ull << 20;
            if (budget > 0)
                mBudgetBytes = static_cast<std::size_t>(budget) << 20;
            mArrayLimit = budget > 0 ? mBudgetBytes : maxArrays;
        }

        void resolve(osg::State& state)
        {
            if (mResolved)
                return;
            mResolved = true;
            std::string_view source;
            std::size_t mib = 0;
            if (mBudgetSetting > 0)
            {
                mib = static_cast<std::size_t>(mBudgetSetting);
                source = "setting";
            }
            else if (mBudgetSetting < 0)
            {
                GLint memory[4]{};
                if (osg::isGLExtensionSupported(state.getContextID(), "GL_NVX_gpu_memory_info"))
                {
                    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, memory);
                    source = "NVX";
                }
                else if (osg::isGLExtensionSupported(state.getContextID(), "GL_ATI_meminfo"))
                {
                    glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, memory);
                    source = "ATI";
                }
                if (memory[0] > 0)
                    mib = static_cast<std::size_t>(memory[0]) * 3 / 4 / 1024;
            }
            mBudgetBytes = mib << 20;
            if (mBudgetBytes != 0)
                mArrayLimit = mBudgetBytes;
            if (mBudgetBytes == 0)
                Log(Debug::Info) << "GPU texture budget is unlimited.";
            else
                Log(Debug::Info) << "GPU texture budget: " << source << ", " << mib << " MiB.";
        }

        osg::ref_ptr<osg::Sampler> sampler(const osg::Texture& texture)
        {
            const SamplerKey key = samplerKey(texture);
            const auto found = mSamplers.find(key);
            if (found != mSamplers.end())
                return found->second;
            osg::ref_ptr<GpuSampler> result = new GpuSampler;
            result->setWrap(osg::Texture::WRAP_S, texture.getWrap(osg::Texture::WRAP_S));
            result->setWrap(osg::Texture::WRAP_T, texture.getWrap(osg::Texture::WRAP_T));
            result->setWrap(osg::Texture::WRAP_R, texture.getWrap(osg::Texture::WRAP_R));
            result->setFilter(osg::Texture::MIN_FILTER, texture.getFilter(osg::Texture::MIN_FILTER));
            result->setFilter(osg::Texture::MAG_FILTER, texture.getFilter(osg::Texture::MAG_FILTER));
            result->setMaxAnisotropy(texture.getMaxAnisotropy());
            result->setMinLOD(texture.getMinLOD());
            result->setMaxLOD(texture.getMaxLOD());
            result->setLODBias(texture.getLODBias());
            result->setBorderColor(texture.getBorderColor());
            result->setShadowCompareFunc(texture.getShadowCompareFunc());
            result->setShadowTextureMode(
                texture.getShadowComparison() ? texture.getShadowTextureMode() : osg::Texture::NONE);
            mSamplers.emplace(key, result);
            return result;
        }

        /// A demoted map holds a tier texture.
        bool demoted(const PendingMaterial& material, unsigned int map) const
        {
            const Id proxy = material.mProxies[map];
            return proxy < mProxies.size() && mProxies[proxy].mCurrent != 0;
        }

        bool demoted(const PendingMaterial& material) const
        {
            for (unsigned int map = 0; map < sMapCount; ++map)
                if (demoted(material, map))
                    return true;
            return false;
        }

        bool makeArray(PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline);
        void makeBindless(PendingMaterial& material);
        void makeLegacy(PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline);
        void useProxies(PendingMaterial& material);
        void prepareTiers();
        osg::ref_ptr<TierWork> pickTiers();
        void backProxy(Proxy& proxy);
        static void buildTiers(TierWork& work);
        void attachTiers(TierWork& work);
        void manage(GpuScene& scene, unsigned int frame);
        void switchTier(GpuScene& scene, Id proxy, unsigned int tier);
        /// Records budget bytes without building a proxy.
        Id track(osg::Texture& texture, osg::ref_ptr<osg::Texture>& out);
        Reservation reserve(osg::Texture2D& texture);
        void rollback(const std::vector<Reservation>& reservations);
        void release(const PendingMaterial& material, osg::State* state = nullptr);
        bool syncPool(GpuScene& scene, osg::State& state, Pool& pool);
        bool allocate(GpuScene& scene, osg::State& state, Pool& pool, PoolGl& gl);
        bool failPool(
            GpuScene& scene, osg::State& state, Pool& pool, GLuint name, std::uint32_t kept, const char* message);
        void uploadLayer(osg::State& state, const Pool& pool, const Layer& layer, std::uint32_t index) const;
        void uploadPool(osg::State& state, const Pool& pool, PoolGl& gl, std::vector<std::uint32_t>& uploaded) const;
        void dropMaterials(GpuScene& scene, Id pool, std::uint32_t kept);
        void release(const PendingRetire& retired, osg::State* state = nullptr)
        {
            if (retired.mBytes != 0)
            {
                std::set<osg::Texture*> textures;
                for (const osg::ref_ptr<osg::Texture>& texture : retired.mMaterial.mTextures)
                    if (texture != nullptr && textures.insert(texture.get()).second)
                        releaseTexture(*texture, state);
                mPendingBytes -= std::min(mPendingBytes, retired.mBytes);
            }
            if (retired.mProxy < mProxies.size())
                mProxies[retired.mProxy].mRetiring = false;
            release(retired.mMaterial, state);
        }

        static void releaseTexture(osg::Texture& texture, osg::State* state)
        {
            osg::Texture::TextureObject* object
                = state != nullptr ? texture.getTextureObject(state->getContextID()) : nullptr;
            const osg::ref_ptr<osg::TextureObjectSet> set = object != nullptr ? object->_set : nullptr;
            texture.releaseGLObjects(state);
            // Bindless texture objects cannot be reused.
            if (set != nullptr)
                set->flushAllDeletedTextureObjects();
        }
        void releaseGLObjects(osg::State* state)
        {
            for (const auto& [key, sampler] : mSamplers)
                sampler->releaseGLObjects(state);
            for (Proxy& proxy : mProxies)
                for (std::size_t tier = 1; tier < proxy.mTiers.size(); ++tier)
                    proxy.mTiers[tier].mTexture->releaseGLObjects(state);
            std::vector<NameTicket> names;
            {
                std::lock_guard<std::mutex> lock(mStageMutex);
                if (state == nullptr)
                {
                    mStagedNames.clear();
                    mStagedLayers.clear();
                    mNameBytes.store(0, std::memory_order_relaxed);
                }
                else
                {
                    const auto found = mStagedNames.find(state->getContextID());
                    if (found != mStagedNames.end())
                    {
                        names.swap(found->second);
                        mStagedNames.erase(found);
                    }
                }
            }
            for (const std::unique_ptr<Pool>& pool : mPools)
            {
                if (pool == nullptr)
                    continue;
                std::lock_guard<std::mutex> lock(pool->mMutex);
                if (state == nullptr)
                {
                    pool->mContexts.clear();
                    continue;
                }
                const auto found = pool->mContexts.find(state->getContextID());
                if (found == pool->mContexts.end())
                    continue;
                names.push_back({ found->second.mName, 0 });
                pool->mContexts.erase(found);
            }
            if (state == nullptr)
                mSyncedArrays.clear();
            else
            {
                deleteNames(names);
                mSyncedArrays.erase(state->getContextID());
            }
        }
        void sync(GpuScene& scene, osg::State& state)
        {
            std::uint64_t& synced = mSyncedArrays[state.getContextID()];
            if (synced == mArrayGeneration)
                return;
            bool bound = false;
            for (const std::unique_ptr<Pool>& pool : mPools)
                if (pool != nullptr)
                    bound = syncPool(scene, state, *pool) || bound;
            synced = mArrayGeneration;
            if (!bound)
                return;
            // Raw binds invalidate OSG texture tracking.
            state.haveAppliedTextureAttribute(sArrayUnit, osg::StateAttribute::TEXTURE);
            const unsigned int units = static_cast<unsigned int>(state.getTextureAttributeMapList().size());
            for (unsigned int unit = 0; unit < units; ++unit)
                state.haveAppliedTextureAttribute(unit, osg::StateAttribute::TEXTURE);
        }
        void stageName(unsigned int context, GLuint name, std::size_t bytes)
        {
            if (name == 0)
                return;
            std::lock_guard<std::mutex> lock(mStageMutex);
            mStagedNames[context].push_back({ name, bytes });
            mNameBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        void stageLayer(const Pool& pool, std::uint32_t layer)
        {
            std::lock_guard<std::mutex> lock(mStageMutex);
            mStagedLayers.push_back({ pool.mId, layer, pool.mLife });
        }
        void takeStaged(unsigned int context, std::vector<NameTicket>& names, std::vector<LayerTicket>& layers)
        {
            std::lock_guard<std::mutex> lock(mStageMutex);
            const auto found = mStagedNames.find(context);
            if (found != mStagedNames.end())
            {
                names.insert(names.end(), found->second.begin(), found->second.end());
                mStagedNames.erase(found);
            }
            layers.insert(layers.end(), mStagedLayers.begin(), mStagedLayers.end());
            mStagedLayers.clear();
        }
        /// Deletes names with the context current.
        void deleteNames(std::vector<NameTicket>& names)
        {
            std::size_t bytes = 0;
            std::vector<GLuint> ids;
            ids.reserve(names.size());
            for (const NameTicket& ticket : names)
            {
                bytes += ticket.mBytes;
                if (ticket.mName != 0)
                    ids.push_back(ticket.mName);
            }
            if (!ids.empty())
                glDeleteTextures(static_cast<GLsizei>(ids.size()), ids.data());
            mNameBytes.fetch_sub(
                std::min(bytes, mNameBytes.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            names.clear();
        }
        void freeLayers(std::vector<LayerTicket>& layers)
        {
            for (const LayerTicket& ticket : layers)
            {
                if (ticket.mPool >= mPools.size() || mPools[ticket.mPool] == nullptr)
                    continue;
                Pool& pool = *mPools[ticket.mPool];
                std::lock_guard<std::mutex> lock(pool.mMutex);
                if (pool.mLife != ticket.mLife || ticket.mLayer >= pool.mLayers.size())
                    continue;
                Layer& layer = pool.mLayers[ticket.mLayer];
                if (layer.mState == LayerState::Staged)
                    layer.mState = LayerState::Free;
            }
            layers.clear();
        }
        std::size_t pendingBytes() const { return mPendingBytes + mNameBytes.load(std::memory_order_relaxed); }
        void warnTexture(const osg::Texture* texture, std::string_view reason, const char* message)
        {
            if (!mWarnings.emplace(texture, reason).second)
                return;
            std::size_t& count = mWarningCounts[std::string(reason)];
            ++count;
            if (count <= 8)
                Log(Debug::Warning) << message;
            else if (count == 9)
                Log(Debug::Warning) << "More GPU textures use material draws.";
        }

        int mBudgetSetting = -1;
        int mMaxLayers = 0;
        std::size_t mArrayLimit = 0;
        std::size_t mArrayBytes = 0;
        std::size_t mProxyBytes = 0;
        std::size_t mPendingBytes = 0;
        std::atomic<std::size_t> mNameBytes{ 0 };
        std::size_t mBudgetBytes = 0;
        std::uint64_t mMipChanges = 0;
        bool mResolved = false;
        std::map<SamplerKey, osg::ref_ptr<osg::Sampler>> mSamplers;
        std::map<StorageKey, Id> mPoolIds;
        std::vector<std::unique_ptr<Pool>> mPools;
        std::uint64_t mArrayGeneration = 1;
        std::map<unsigned int, std::uint64_t> mSyncedArrays;
        std::map<std::pair<const osg::Image*, StorageKey>, Id> mProxyIds;
        std::vector<Proxy> mProxies;
        SceneUtil::WorkQueue* mWorkQueue = nullptr;
        osg::ref_ptr<TierWork> mTierWork;
        std::mutex mStageMutex;
        std::map<unsigned int, std::vector<NameTicket>> mStagedNames;
        std::vector<LayerTicket> mStagedLayers;
        CopyImageFn mCopyImage = nullptr;
        std::set<std::pair<const osg::Texture*, std::string>> mWarnings;
        std::map<std::string, std::size_t> mWarningCounts;
    };

    struct GpuScene::Gl
    {
        using HandleKey = std::pair<osg::ref_ptr<const osg::Texture>, osg::ref_ptr<const osg::Sampler>>;

        struct HandleEntry
        {
            GLuint64 mHandle = 0;
            std::uint32_t mUsers = 0;
            std::uint64_t mGeneration = 0;
        };

        struct HandleTicket
        {
            HandleKey mKey;
            std::uint64_t mGeneration = 0;
            unsigned int mFrame = 0;
        };

        struct FenceBatch
        {
            GLsync mFence = nullptr;
            std::vector<HandleTicket> mHandles;
            std::vector<PendingRetire> mMaterials;
            std::vector<Textures::NameTicket> mNames;
            std::vector<Textures::LayerTicket> mLayers;
        };

        osg::GLExtensions* mExt = nullptr;
        unsigned int mDrawLogs = 32;
        bool mTraceDraws = false;
        GLenum(GL_APIENTRY* mGetResetStatus)() = nullptr;
        void draw(osg::State& state, const char* pass, Shader::MaterialMode mode, GLuint buffer, std::size_t first,
            GLsizei count);
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
        std::size_t mCountBytes = 0;
        std::size_t mVisibleBytes = 0;
        std::size_t mCommandBytes = 0;
        GLuint mViewBuffer = 0;
        GLuint mOcclusionBuffer = 0;
        std::vector<GLuint> mReceiverBuffers;
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
        GLuint mRayBuffer = 0;
        GLuint mRayTaskBuffer = 0;
        std::size_t mMaterialCapacity = 0;
        std::map<HandleKey, HandleEntry> mHandles;
        std::vector<std::array<std::optional<HandleKey>, sMapCount>> mMaterialHandles;
        std::vector<HandleTicket> mPendingHandles;
        unsigned int mTicketFrame = 0;
        std::vector<PendingRetire> mPendingRetired;
        std::vector<Textures::NameTicket> mPendingNames;
        std::vector<Textures::LayerTicket> mPendingLayers;
        std::vector<FenceBatch> mRetired;
        std::size_t mVertexCapacity = 0;
        std::size_t mIndexCapacity = 0;
        std::size_t mInstanceCapacity = 0;
        std::size_t mOcclusionCapacity = 0;
        std::size_t mBatchCapacity = 0;
        std::size_t mSkinSourceCapacity = 0;
        std::size_t mSkinGroupCapacity = 0;
        std::size_t mSkinRangeCapacity = 0;
        std::size_t mSkinInfluenceCapacity = 0;
        std::uint32_t mBatchCount = 0;
        std::uint32_t mSlotCount = 0;
        std::uint32_t mCommandCount = 0;
        /// View regions the dispatch buffers hold.
        std::uint32_t mViewCapacity = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> mDynamicRanges;
        osg::ref_ptr<osg::Program> mCullProgram;
        osg::ref_ptr<osg::Program> mCommandProgram;
        osg::ref_ptr<osg::Program> mSkinProgram;
        osg::ref_ptr<osg::Program> mRayProgram;
        osg::ref_ptr<osg::Uniform> mInstanceCount;
        osg::ref_ptr<osg::Uniform> mChunkCount;
        osg::ref_ptr<osg::Uniform> mViewBase;
        osg::ref_ptr<osg::Uniform> mViewCount;
        osg::ref_ptr<osg::Uniform> mSlotCountUniform;
        osg::ref_ptr<osg::Uniform> mBatchCountUniform;
        osg::ref_ptr<osg::Uniform> mCommandCountUniform;
        osg::ref_ptr<osg::Uniform> mOccluderPixelUniform;
        osg::ref_ptr<osg::Uniform> mCullPassUniform;
        osg::ref_ptr<osg::Uniform> mOcclusionReadyUniform;
        osg::ref_ptr<osg::Uniform> mRayPassUniform;
        osg::ref_ptr<osg::Uniform> mRayTaskBaseUniform;
        osg::ref_ptr<osg::Uniform> mPyramidLevelsUniform;
        bool mLimitsChecked = false;
        std::size_t mStorageLimit = 0;
        osg::ref_ptr<osg::Sampler> mSamplerReset;
        bool mSamplerResetBound = false;
        std::map<std::tuple<const osg::Program*, bool, Shader::MaterialMode>, osg::ref_ptr<osg::StateSet>>
            mCastingStateSets;
        std::map<Shader::MaterialMode, osg::ref_ptr<osg::StateSet>> mPrepassStateSets;
        std::map<const osg::StateSet*, osg::ref_ptr<osg::StateSet>> mPrepassStates;
        std::map<Shader::MaterialMode, osg::ref_ptr<osg::Program>> mPrepassPrograms;
        osg::ref_ptr<osg::Program> mDownsampleProgram;
        osg::ref_ptr<osg::Uniform> mFirstPass;
        osg::ref_ptr<osg::Uniform> mSourceDepthUniform;
        osg::ref_ptr<osg::Uniform> mSourceSamplesUniform;
        GLuint mPyramid = 0;
        int mPyramidWidth = 0;
        int mPyramidHeight = 0;
        int mPyramidLevels = 0;
        std::map<std::pair<const osg::Program*, Shader::MaterialMode>, osg::ref_ptr<osg::Program>> mCastingPrograms;

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

        void stream(GLuint& buffer, const void* data, std::size_t size, GLenum usage = GL_STREAM_DRAW)
        {
            if (buffer == 0)
                mExt->glGenBuffers(1, &buffer);
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            mExt->glBufferData(GL_COPY_WRITE_BUFFER, std::max<std::size_t>(size, 4), data, usage);
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
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
            mExt->glEnableVertexAttribArray(Shader::ShaderManager::sVertexMaterialAttribute);
            mExt->glVertexAttribIPointer(Shader::ShaderManager::sVertexMaterialAttribute, 1, GL_UNSIGNED_INT, stride,
                reinterpret_cast<const void*>(offsetof(GpuVertex, mMaterial)));
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

        HandleEntry& acquire(osg::State& state, const HandleKey& key)
        {
            auto [found, inserted] = mHandles.try_emplace(key);
            HandleEntry& entry = found->second;
            if (!inserted)
                return entry;
            osg::Texture& texture = *const_cast<osg::Texture*>(key.first.get());
            osg::Sampler& sampler = *const_cast<osg::Sampler*>(key.second.get());
            osg::Texture::TextureObject* object = texture.getTextureObject(state.getContextID());
            if (object == nullptr)
            {
                state.applyTextureAttribute(0, &texture);
                object = texture.getTextureObject(state.getContextID());
            }
            if (object != nullptr)
            {
                state.setActiveTextureUnit(0);
                glBindTexture(texture.getTextureTarget(), object->id());
                state.haveAppliedTextureAttribute(0, osg::StateAttribute::TEXTURE);
                GLint width = 0;
                glGetTexLevelParameteriv(texture.getTextureTarget(), 0, GL_TEXTURE_WIDTH, &width);
                if (width != 0)
                {
                    sampler.compileGLObjects(state);
                    const auto* managed = dynamic_cast<const GpuSampler*>(&sampler);
                    const GLuint samplerObject = managed != nullptr ? managed->object(state.getContextID()) : 0;
                    if (samplerObject != 0)
                    {
                        entry.mHandle = mExt->glGetTextureSamplerHandle(object->id(), samplerObject);
                        if (entry.mHandle != 0)
                            mExt->glMakeTextureHandleResident(entry.mHandle);
                    }
                }
            }
            if (entry.mHandle == 0)
            {
                const osg::Image* image = texture.getImage(0);
                Log(Debug::Warning) << "GPU bindless texture unavailable: "
                                    << (image != nullptr ? image->getFileName() : texture.getName())
                                    << " texture=" << (object != nullptr ? object->id() : 0);
            }
            return entry;
        }

        static bool contains(const std::array<std::optional<HandleKey>, sMapCount>& keys, const HandleKey& key)
        {
            return std::any_of(keys.begin(), keys.end(),
                [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == key; });
        }

        void dropHandle(const HandleKey& key)
        {
            const auto found = mHandles.find(key);
            if (found == mHandles.end() || found->second.mUsers == 0)
                return;
            // Unused handles remain resident until textures leave.
            --found->second.mUsers;
        }

        void retireTextureHandles(const osg::Texture* texture)
        {
            for (auto& [key, entry] : mHandles)
            {
                if (key.first.get() != texture || entry.mUsers != 0)
                    continue;
                ++entry.mGeneration;
                mPendingHandles.push_back({ key, entry.mGeneration, mTicketFrame });
            }
        }

        void setMaterial(osg::State& state, PendingMaterial& material)
        {
            if (material.mIndex >= mMaterialHandles.size())
                mMaterialHandles.resize(material.mIndex + 1);
            auto& previous = mMaterialHandles[material.mIndex];
            std::array<std::optional<HandleKey>, sMapCount> next;
            for (unsigned int i = 0; i < sMapCount; ++i)
            {
                if (material.mTextures[i] == nullptr || material.mSamplers[i] == nullptr)
                {
                    material.mRecord.mMaps[i] = 0;
                    continue;
                }
                HandleKey key(material.mTextures[i], material.mSamplers[i]);
                HandleEntry& entry = acquire(state, key);
                material.mRecord.mMaps[i] = entry.mHandle;
                next[i] = key;
                const bool duplicate = std::any_of(next.begin(), next.begin() + i,
                    [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == key; });
                if (!duplicate && !contains(previous, key))
                    ++entry.mUsers;
            }
            for (unsigned int i = 0; i < sMapCount; ++i)
            {
                if (!previous[i].has_value() || contains(next, *previous[i]))
                    continue;
                const bool duplicate = std::any_of(previous.begin(), previous.begin() + i,
                    [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == *previous[i]; });
                if (!duplicate)
                    dropHandle(*previous[i]);
            }
            previous = std::move(next);
        }

        void retireMaterial(PendingRetire&& retired)
        {
            const Id material = retired.mMaterial.mIndex;
            if (retired.mFinal && material < mMaterialHandles.size())
            {
                auto& keys = mMaterialHandles[material];
                for (unsigned int i = 0; i < sMapCount; ++i)
                {
                    if (!keys[i].has_value())
                        continue;
                    const bool duplicate = std::any_of(keys.begin(), keys.begin() + i,
                        [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == *keys[i]; });
                    if (!duplicate)
                        dropHandle(*keys[i]);
                }
                keys = {};
            }
            // Handles retire with storage, not materials.
            if (retired.mBytes != 0)
                for (const osg::ref_ptr<osg::Texture>& texture : retired.mMaterial.mTextures)
                    if (texture != nullptr)
                        retireTextureHandles(texture.get());
            mPendingRetired.push_back(std::move(retired));
        }

        void pollRetired(GpuScene& scene, osg::State& state)
        {
            std::vector<PendingRetire> released;
            std::vector<Textures::NameTicket> names;
            std::vector<Textures::LayerTicket> layers;
            for (auto batch = mRetired.begin(); batch != mRetired.end();)
            {
                const unsigned int frame
                    = state.getFrameStamp() != nullptr ? state.getFrameStamp()->getFrameNumber() : 0;
                // Queued frames may still sample these textures.
                if (std::any_of(batch->mHandles.begin(), batch->mHandles.end(),
                        [frame](const HandleTicket& ticket) { return frame - ticket.mFrame < 2; }))
                {
                    ++batch;
                    continue;
                }
                const GLenum wait = mExt->glClientWaitSync(batch->mFence, 0, 0);
                if (wait == GL_TIMEOUT_EXPIRED || wait == GL_WAIT_FAILED)
                {
                    ++batch;
                    continue;
                }
                mExt->glDeleteSync(batch->mFence);
                for (const HandleTicket& ticket : batch->mHandles)
                {
                    const auto found = mHandles.find(ticket.mKey);
                    if (found == mHandles.end() || found->second.mUsers != 0
                        || found->second.mGeneration != ticket.mGeneration)
                        continue;
                    if (found->second.mHandle != 0)
                        mExt->glMakeTextureHandleNonResident(found->second.mHandle);
                    mHandles.erase(found);
                }
                released.insert(released.end(), std::make_move_iterator(batch->mMaterials.begin()),
                    std::make_move_iterator(batch->mMaterials.end()));
                names.insert(names.end(), batch->mNames.begin(), batch->mNames.end());
                layers.insert(layers.end(), batch->mLayers.begin(), batch->mLayers.end());
                batch = mRetired.erase(batch);
            }
            if (!names.empty())
                scene.mTextures->deleteNames(names);
            if (!released.empty() || !layers.empty())
            {
                std::lock_guard<std::mutex> lock(scene.mMutex);
                for (const PendingRetire& retired : released)
                    scene.mTextures->release(retired, &state);
                scene.mTextures->freeLayers(layers);
            }
        }

        void advance(GpuScene& scene, osg::State& state)
        {
            if (state.getFrameStamp() != nullptr)
                mTicketFrame = state.getFrameStamp()->getFrameNumber();
            scene.mTextures->takeStaged(state.getContextID(), mPendingNames, mPendingLayers);
            pollRetired(scene, state);
        }

        /// One fence covers everything staged this iteration.
        void fenceRetired()
        {
            if (mPendingHandles.empty() && mPendingRetired.empty() && mPendingNames.empty() && mPendingLayers.empty())
                return;
            GLsync fence = mExt->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (fence == nullptr)
                return;
            FenceBatch batch;
            batch.mFence = fence;
            batch.mHandles.swap(mPendingHandles);
            batch.mMaterials.swap(mPendingRetired);
            batch.mNames.swap(mPendingNames);
            batch.mLayers.swap(mPendingLayers);
            mRetired.push_back(std::move(batch));
        }

        void cleanup(GpuScene& scene, osg::State& state)
        {
            for (const auto& [key, entry] : mHandles)
                if (entry.mHandle != 0)
                    mExt->glMakeTextureHandleNonResident(entry.mHandle);
            for (const auto& [key, entry] : mHandles)
                Textures::releaseTexture(*const_cast<osg::Texture*>(key.first.get()), &state);
            mHandles.clear();
            for (FenceBatch& batch : mRetired)
                if (batch.mFence != nullptr)
                    mExt->glDeleteSync(batch.mFence);
            std::lock_guard<std::mutex> lock(scene.mMutex);
            for (const PendingRetire& retired : mPendingRetired)
                scene.mTextures->release(retired, &state);
            for (const FenceBatch& batch : mRetired)
                for (const PendingRetire& retired : batch.mMaterials)
                    scene.mTextures->release(retired, &state);
            scene.mTextures->takeStaged(state.getContextID(), mPendingNames, mPendingLayers);
            for (const FenceBatch& batch : mRetired)
            {
                mPendingNames.insert(mPendingNames.end(), batch.mNames.begin(), batch.mNames.end());
                mPendingLayers.insert(mPendingLayers.end(), batch.mLayers.begin(), batch.mLayers.end());
            }
            scene.mTextures->deleteNames(mPendingNames);
            scene.mTextures->freeLayers(mPendingLayers);
            mPendingRetired.clear();
            mPendingHandles.clear();
            mRetired.clear();
            mMaterialHandles.clear();
        }

        bool resizeViews(std::uint32_t views)
        {
            if (views <= mViewCapacity)
                return true;
            const std::size_t capacity = std::max(views, 8u);
            if (capacity * mCommandCount > mStorageLimit / sizeof(GpuCommand)
                || capacity * mSlotCount > mStorageLimit / sizeof(std::uint32_t)
                || capacity * mBatchCount > mStorageLimit / sizeof(std::uint32_t))
            {
                Log(Debug::Error) << "GPU view buffers exceed storage limit";
                return false;
            }
            mViewCapacity = static_cast<std::uint32_t>(capacity);
            const auto reallocate = [&](GLuint& buffer, std::size_t& allocated, std::size_t bytes) {
                bytes = std::max<std::size_t>(bytes, 4);
                GLuint fresh = 0;
                mExt->glGenBuffers(1, &fresh);
                mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, fresh);
                mExt->glBufferData(GL_COPY_WRITE_BUFFER, bytes, nullptr, GL_DYNAMIC_COPY);
                if (buffer != 0)
                {
                    // Earlier view groups keep their regions.
                    mExt->glBindBuffer(GL_COPY_READ_BUFFER, buffer);
                    mCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                        static_cast<GLsizeiptr>(std::min(allocated, bytes)));
                    mExt->glBindBuffer(GL_COPY_READ_BUFFER, 0);
                    mExt->glDeleteBuffers(1, &buffer);
                }
                buffer = fresh;
                allocated = bytes;
            };
            reallocate(mVisibleBuffer, mVisibleBytes,
                static_cast<std::size_t>(mViewCapacity) * mSlotCount * sizeof(std::uint32_t));
            reallocate(mCountBuffer, mCountBytes,
                static_cast<std::size_t>(mViewCapacity) * mBatchCount * sizeof(std::uint32_t));
            reallocate(mCommandBuffer, mCommandBytes,
                static_cast<std::size_t>(mViewCapacity) * mCommandCount * sizeof(GpuCommand));
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            // Fresh buffer names invalidate recorded arrays.
            mVao = 0;
            return true;
        }

        void zero(GLuint buffer)
        {
            const std::uint32_t zero = 0;
            mExt->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            mClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            mExt->glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    };

    GpuScene::GpuScene(Shader::ShaderManager& shaderManager, bool gpuSkinning, bool packedTextures)
        : mShaderManager(shaderManager)
        , mGpuSkinningEnabled(gpuSkinning)
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
        // Realize-time limits refuse before nodes commit.
        const GLint storageBindings = SceneUtil::getMaxShaderStorageBindings();
        if (storageBindings != 0
            && (storageBindings <= sMaxStorageBinding || SceneUtil::getMaxVertexStorageBlocks() < sVertexStorageBlocks
                || SceneUtil::getFragmentStorageBlocks() < sFragmentStorageBlocks))
        {
            Log(Debug::Warning) << "GPU-driven object paging unavailable: too few storage buffer bindings";
            mSupported = false;
            return;
        }
        const bool bindless = ext.glGetTextureHandle != nullptr && ext.glGetTextureSamplerHandle != nullptr
            && ext.glMakeTextureHandleResident != nullptr && ext.glMakeTextureHandleNonResident != nullptr
            && osg::isGLExtensionSupported(ext.contextID, "GL_ARB_bindless_texture");
        // Instance handles need nonuniform sampler access.
        const bool shader5 = osg::isGLExtensionSupported(ext.contextID, "GL_NV_gpu_shader5");
        // AMD exposes nonuniformEXT for OpenGL bindless samplers.
        const bool qualifier = osg::isGLExtensionSupported(ext.contextID, "GL_EXT_nonuniform_qualifier");
        const char* forcedMode = std::getenv("OPENMW_GPU_MATERIAL_MODE");
        const bool forceArray = forcedMode != nullptr && std::string_view(forcedMode) == "array";
        const bool forceLegacy = forcedMode != nullptr && std::string_view(forcedMode) == "legacy";
        const GLint maxLayers = ext.isTexture2DArraySupported ? ext.maxLayerCount : 0;
        if (packedTextures && bindless && (shader5 || qualifier) && !forceArray && !forceLegacy)
            mMaterialMode = Shader::MaterialMode::Bindless;
        else if (packedTextures && !forceLegacy && maxLayers >= 2)
            mMaterialMode = Shader::MaterialMode::Array;
        else
        {
            mMaterialMode = Shader::MaterialMode::Legacy;
            Log(Debug::Warning) << "GPU texture arrays unavailable; using material draws.";
        }
        if (bindless && mMaterialMode != Shader::MaterialMode::Bindless && !forceArray && !forceLegacy)
            Log(Debug::Warning) << "GPU bindless requirements unavailable; using fallback.";
        const char* mode = "legacy";
        if (mMaterialMode == Shader::MaterialMode::Bindless)
            mode = shader5 ? "bindless-nv" : "bindless-ext";
        else if (mMaterialMode == Shader::MaterialMode::Array)
            mode = "array";
        Log(Debug::Info) << "GPU materials: mode=" << mode << " bindless=" << bindless << " shader5=" << shader5
                         << " nonuniformEXT=" << qualifier << " vertexBlocks=" << SceneUtil::getMaxVertexStorageBlocks()
                         << " arrayLayers=" << maxLayers;
        mTextures = std::make_unique<Textures>(Settings::terrain().mObjectPagingGpuTextureBudget, maxLayers);
        mGl = std::make_unique<Gl>();
        mGl->mExt = &ext;
        const char* trace = std::getenv("OPENMW_GPU_DRAW_TRACE");
        mGl->mTraceDraws = trace != nullptr && std::string_view(trace) == "1";
        if (mGl->mTraceDraws)
            Log(Debug::Warning) << "GPU draw tracing enabled; rendering will slow down.";
        mRaySupported = ext.glFenceSync != nullptr && ext.glClientWaitSync != nullptr && ext.glDeleteSync != nullptr
            && ext.glMapBufferRange != nullptr && ext.glUnmapBuffer != nullptr;
        osg::setGLExtensionFuncPtr(mGl->mClearBufferData, "glClearBufferData");
        osg::setGLExtensionFuncPtr(mGl->mCopyBufferSubData, "glCopyBufferSubData");
        if (mGl->mCopyBufferSubData == nullptr || mGl->mClearBufferData == nullptr)
        {
            Log(Debug::Warning) << "GPU paging unavailable: buffer functions missing";
            mSupported = false;
            mGl.reset();
            return;
        }
        mRayState = std::make_shared<RayState>();
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
        if (mRayState != nullptr)
        {
            std::lock_guard<std::mutex> stateLock(mRayState->mMutex);
            mRayState->mAlive = false;
        }
        if (mRetireOperation != nullptr)
            mRetireOperation->detach();
        std::lock_guard<std::mutex> lock(mMutex);
        for (const osg::observer_ptr<GpuCaptureCallback>& callback : mCaptureCallbacks)
        {
            // The unref thread may destroy callbacks concurrently.
            osg::ref_ptr<GpuCaptureCallback> held;
            if (callback.lock(held))
                held->detach();
        }
        restoreRigsLocked();
        for (GpuChunkNode* node : mChunkNodes)
            node->detach();
        for (Dispatcher& dispatcher : mDispatchers)
            static_cast<DispatchDrawable*>(dispatcher.mDrawable.get())->detach();
        if (mPrepassDrawable != nullptr)
            static_cast<DispatchDrawable*>(mPrepassDrawable.get())->detach();
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
        std::vector<RayRequest> failed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!enabled)
            {
                restoreRigsLocked();
                failed.swap(mPendingRays);
                mRayInvalidate = false;
            }
            mEnabled = enabled;
            if (enabled)
            {
                for (const auto& [actor, batches] : mActorBatches)
                    for (const auto& batch : batches)
                        for (const ActorPart& part : batch->mParts)
                            part.mDrawable->setNodeMask(Mask_GpuProxy);
                for (const RigEntry& entry : mRigRegistry)
                {
                    if (entry.mRig->getNodeMask() == ~0u)
                        entry.mRig->setNodeMask(Mask_GpuProxy);
                    if (entry.mRig->getNodeMask() == Mask_GpuProxy)
                        entry.mRig->setCpuSkinning(false);
                }
            }
            updateRigCullLocked();
        }
        if (enabled && mPrepassDrawable == nullptr && mNode != nullptr)
        {
            // The prepass exists only for enabled scenes.
            osg::ref_ptr<DispatchDrawable> prepass = new DispatchDrawable(*this, sPrepass);
            osg::ref_ptr<osg::StateSet> prepassState = new osg::StateSet;
            prepassState->setRenderBinDetails(-1000, "RenderBin");
            prepassState->setAttribute(new osg::ColorMask(false, false, false, false),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            prepassState->setAttribute(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            prepass->setStateSet(prepassState);
            mPrepassDrawable = prepass;
            mNode->addChild(prepass);
        }
        if (!failed.empty())
        {
            RayBatch batch;
            batch.mRequests = std::move(failed);
            finishRay(mRayState, batch, nullptr, false);
        }
    }

    void GpuScene::setLightManager(SceneUtil::LightManager* lightManager)
    {
        mLightManager = lightManager;
    }

    void GpuScene::setDefaultMaterial(const SceneUtil::Material* material)
    {
        mDefaultMaterial = material;
    }

    void GpuScene::setWorkQueue(SceneUtil::WorkQueue* queue)
    {
        if (mTextures == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        mTextures->mWorkQueue = queue;
    }

    void GpuScene::capture(osg::Node& node)
    {
        capture(node, nullptr);
    }

    void GpuScene::captureActor(osg::Node& node, SceneUtil::PositionAttitudeTransform& actor)
    {
        capture(node, &actor);
    }

    void GpuScene::capture(osg::Node& node, SceneUtil::PositionAttitudeTransform* actor)
    {
        if (!isEnabled())
            return;
        SceneUtil::LightListCallback* lightList = nullptr;
        // The actor's light list carries torch ignores.
        for (osg::Callback* callback = node.getCullCallback(); callback != nullptr;
            callback = callback->getNestedCallback())
            if ((lightList = dynamic_cast<SceneUtil::LightListCallback*>(callback)) != nullptr)
                break;
        CaptureVisitor visitor(mNode, actor != nullptr);
        // The sky keeps per-view transforms and state.
        visitor.setTraversalMask(~(Mask_Sky | (actor != nullptr ? Mask_UpdateVisitor : 0u)));
        node.accept(visitor);
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<Id> retired;
        if (actor != nullptr)
            removeActorLocked(*actor, &retired);
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
        for (const CaptureVisitor::CapturedRig& captured : visitor.mRigs)
        {
            if (!mGpuSkinningEnabled)
                continue;
            SceneUtil::RigGeometry* rig = captured.mRig;
            if (orderDependentPath(captured.mStates))
                continue;
            osg::Geometry* geometry = rig->getGpuGeometry();
            const osg::ref_ptr<osg::Geometry> source = rig->getSourceGeometry();
            if (geometry == nullptr || source == nullptr)
                continue;
            const Id mesh = getMeshLocked(*geometry, source.get());
            if (mesh == sInvalid || !registerSkinLocked(mesh, *rig))
                continue;
            rig->setGpuSkinning(true);
            if (actor != nullptr && mSkinSupported.load(std::memory_order_relaxed) && !captured.mUpdater
                && rig->getNodeMask() == ~0u)
            {
                const Surface surface = getSurfaceLocked(captured.mStates, 0, MaterialUpdate::None);
                if (surface.mBucket != sInvalid)
                {
                    ++mMeshes[mesh].mUsers;
                    addMaterialLocked(surface.mMaterial);
                    mRigRegistry.push_back(RigEntry{ rig, actor, mesh, surface, lightList, &node });
                    if (isEnabled())
                    {
                        rig->setNodeMask(Mask_GpuProxy);
                        rig->setCpuSkinning(false);
                    }
                    continue;
                }
            }
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
        if (actor != nullptr && mSkinSupported.load(std::memory_order_relaxed)
            && std::any_of(
                mRigRegistry.begin(), mRigRegistry.end(), [&](const RigEntry& entry) { return entry.mActor == actor; }))
        {
            std::vector<Surface> surfaces;
            bool full = false;
            for (const CaptureVisitor::Part& captured : visitor.mParts)
            {
                const Surface surface = orderDependentPath(captured.mStates)
                    ? Surface{}
                    : getSurfaceLocked(captured.mStates, 0, MaterialUpdate::None);
                surfaces.push_back(surface);
                if (mMaterialMode == Shader::MaterialMode::Bindless && surface.mBucket != sInvalid)
                    for (std::size_t i = 1; i < Shader::sObjectMapCount; ++i)
                        full = full || mMaterials[surface.mMaterial].mTextures[i] != nullptr;
            }
            std::map<Id, std::pair<SceneUtil::GeometryBatch, std::shared_ptr<ActorBatch>>> batches;
            for (std::size_t i = 0; i < visitor.mParts.size(); ++i)
            {
                const CaptureVisitor::Part& captured = visitor.mParts[i];
                const Surface surface = surfaces[i];
                if (surface.mBucket == sInvalid)
                    continue;
                ActorPart part;
                part.mDrawable = captured.mDrawable;
                part.mRig = dynamic_cast<SceneUtil::RigGeometry*>(captured.mDrawable);
                if (mMaterialMode == Shader::MaterialMode::Array && part.mRig == nullptr)
                    continue;
                part.mMorph = dynamic_cast<SceneUtil::MorphGeometry*>(captured.mDrawable);
                if (part.mRig != nullptr)
                {
                    const auto found = std::find_if(mRigRegistry.begin(), mRigRegistry.end(),
                        [&](const RigEntry& entry) { return entry.mActor == actor && entry.mRig == part.mRig; });
                    if (found == mRigRegistry.end())
                        continue;
                    part.mGeometry = part.mRig->getSourceGeometry();
                    part.mMesh = found->mMesh;
                }
                else
                {
                    part.mGeometry = part.mMorph != nullptr ? part.mMorph->getSourceGeometry().get()
                                                            : captured.mDrawable->asGeometry();
                    if (part.mGeometry == nullptr
                        || (part.mMorph == nullptr && part.mGeometry->getDataVariance() == osg::Object::DYNAMIC))
                        continue;
                }
                const Id bucket = getActorBucket(surface.mBucket, full);
                if (bucket == sInvalid)
                    continue;
                const auto* winding = dynamic_cast<const osg::FrontFace*>(
                    mBuckets[surface.mBucket].mStateSet->getAttribute(osg::StateAttribute::FRONTFACE));
                auto& [geometry, batch] = batches[bucket];
                if (batch == nullptr)
                {
                    batch = std::make_shared<ActorBatch>();
                    batch->mBucket = bucket;
                }
                part.mVertex = geometry.mGeometry->getVertexArray()->getNumElements();
                if (!geometry.add(*part.mGeometry, surface.mMaterial,
                        winding != nullptr && winding->getMode() == osg::FrontFace::CLOCKWISE))
                    continue;
                part.mMaterial = surface.mMaterial;
                if (part.mRig != nullptr)
                {
                    part.mPalette = batch->mMatrixCount;
                    part.mMatrixCount = mSkinMeshes.at(part.mMesh).mBoneCount + 1;
                    batch->mMatrixCount += part.mMatrixCount;
                }
                if (part.mRig == nullptr)
                    for (const auto& attachment : captured.mTransforms)
                    {
                        const auto found = std::find_if(batch->mTransforms.begin(), batch->mTransforms.end(),
                            [&](const ActorBatch::Transform& transform) {
                                return transform.mNode == attachment && transform.mParent == part.mTransform;
                            });
                        if (found == batch->mTransforms.end())
                        {
                            batch->mTransforms.push_back({ attachment, part.mTransform });
                            part.mTransform = static_cast<Id>(batch->mTransforms.size() - 1);
                        }
                        else
                            part.mTransform = static_cast<Id>(found - batch->mTransforms.begin());
                    }
                if (part.mRig == nullptr && part.mMorph == nullptr && part.mDrawable->getNumParents() > 1)
                {
                    osg::ref_ptr<osg::Geometry> local = new osg::Geometry(*part.mGeometry, osg::CopyOp::SHALLOW_COPY);
                    captured.mParent->replaceChild(part.mDrawable, local);
                    part.mDrawable = local;
                }
                batch->mParts.push_back(std::move(part));
            }
            for (auto& [bucket, assembled] : batches)
            {
                auto& [geometry, batch] = assembled;
                if (batch->mParts.size() < 2)
                    continue;
                const std::uint32_t rigid = batch->mMatrixCount;
                batch->mMatrixCount += static_cast<std::uint32_t>(batch->mTransforms.size() + 1);
                if (batch->mMatrixCount > 0x1000000u)
                    continue;
                auto& skin = *static_cast<osg::Vec2uiArray*>(geometry.mGeometry->getVertexAttribArray(8));
                for (ActorPart& part : batch->mParts)
                {
                    if (part.mRig == nullptr)
                        part.mPalette = rigid
                            + (part.mTransform != sInvalid ? part.mTransform
                                                           : static_cast<Id>(batch->mTransforms.size()));
                    unsigned int flags = part.mGeometry->getNormalArray() != nullptr ? 1u : 0u;
                    if (dynamic_cast<const osg::Vec4Array*>(part.mGeometry->getTexCoordArray(7)) != nullptr)
                        flags |= 2u;
                    part.mPalette |= flags << 24;
                    const auto count = part.mGeometry->getVertexArray()->getNumElements();
                    for (unsigned int vertex = 0; vertex < count; ++vertex)
                    {
                        Id group = sInvalid;
                        if (part.mRig != nullptr)
                        {
                            group = part.mRig->getSkinGroups()[vertex];
                            group = group != sInvalid ? group + mSkinMeshes.at(part.mMesh).mGroupFirst : sInvalid - 1;
                        }
                        skin[part.mVertex + vertex] = { part.mPalette, group };
                    }
                }
                batch->mMesh = getMeshLocked(*geometry.mGeometry, geometry.mGeometry);
                if (batch->mMesh == sInvalid)
                    continue;
                batch->mGeometry = geometry.mGeometry;
                ++mMeshes[batch->mMesh].mUsers;
                for (const ActorPart& part : batch->mParts)
                {
                    if (part.mMesh != sInvalid)
                        ++mMeshes[part.mMesh].mUsers;
                    addMaterialLocked(part.mMaterial);
                    part.mDrawable->setNodeMask(Mask_GpuProxy);
                    if (part.mRig != nullptr)
                        for (RigEntry& entry : mRigRegistry)
                            if (entry.mRig == part.mRig && entry.mActor == actor)
                                entry.mBatched = true;
                }
                mActorBatches[actor].push_back(std::move(batch));
            }
        }
        for (const Id mesh : retired)
            releaseMeshLocked(mesh);
        updateRigCullLocked();
    }

    void GpuScene::removeActor(SceneUtil::PositionAttitudeTransform& actor)
    {
        std::vector<RayRequest> failed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            removeActorLocked(actor);
            if (mRigRegistry.empty())
            {
                failed.swap(mPendingRays);
                mRayInvalidate = false;
            }
        }
        if (!failed.empty())
        {
            RayBatch batch;
            batch.mRequests = std::move(failed);
            finishRay(mRayState, batch, nullptr, false);
        }
    }

    void GpuScene::removeActorLocked(SceneUtil::PositionAttitudeTransform& actor, std::vector<Id>* retired)
    {
        const auto batches = mActorBatches.find(&actor);
        if (batches != mActorBatches.end())
        {
            for (const auto& batch : batches->second)
            {
                releaseMeshLocked(batch->mMesh);
                for (const ActorPart& part : batch->mParts)
                {
                    part.mDrawable->setNodeMask(~0u);
                    dropMaterialLocked(part.mMaterial);
                    if (part.mMesh != sInvalid)
                    {
                        if (retired != nullptr)
                            retired->push_back(part.mMesh);
                        else
                            releaseMeshLocked(part.mMesh);
                    }
                }
            }
            mActorBatches.erase(batches);
        }
        const auto found = std::find_if(
            mRigRegistry.begin(), mRigRegistry.end(), [&](const RigEntry& entry) { return entry.mActor == &actor; });
        if (found == mRigRegistry.end())
            return;
        for (const RigEntry& entry : mRigRegistry)
            if (entry.mActor == &actor)
            {
                entry.mRig->setCpuSkinning(true);
                if (retired != nullptr)
                    retired->push_back(entry.mMesh);
                else
                    releaseMeshLocked(entry.mMesh);
                dropMaterialLocked(entry.mSurface.mMaterial);
            }
        restoreProxy(actor);
        mRigRegistry.erase(std::remove_if(mRigRegistry.begin(), mRigRegistry.end(),
                               [&](const RigEntry& entry) { return entry.mActor == &actor; }),
            mRigRegistry.end());
        if (!mPendingRays.empty())
            mRayInvalidate = true;
        updateRigCullLocked();
    }

    void GpuScene::restoreRigsLocked()
    {
        SceneUtil::PositionAttitudeTransform* actor = nullptr;
        for (const RigEntry& entry : mRigRegistry)
        {
            entry.mRig->setCpuSkinning(true);
            if (entry.mActor == actor)
                continue;
            actor = entry.mActor;
            restoreProxy(*actor);
        }
    }

    void GpuScene::updateRigCullLocked()
    {
        if (mNode != nullptr)
            mNode->setCullingActive(!isEnabled() || mRigRegistry.empty());
    }

    GpuScene::RayId GpuScene::queueRay(const osg::Vec3f& from, const osg::Vec3f& to, bool ignorePlayer,
        std::span<SceneUtil::PositionAttitudeTransform* const> ignore)
    {
        if (!from.valid() || !to.valid())
            return sInvalidRay;
        RayRequest request;
        RayId id = sInvalidRay;
        bool complete = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!isEnabled() || !mSkinSupported.load(std::memory_order_relaxed)
                || !mRaySupported.load(std::memory_order_relaxed))
                return sInvalidRay;
            if (mNextRay == sInvalidRay)
                ++mNextRay;
            id = mNextRay++;
            request.mId = id;
            request.mFrom = from;
            request.mTo = to;
            request.mIgnorePlayer = ignorePlayer;
            request.mIgnore.reserve(ignore.size());
            for (SceneUtil::PositionAttitudeTransform* actor : ignore)
                request.mIgnore.emplace_back(actor);
            complete = mRigRegistry.empty();
            if (!complete)
                mPendingRays.push_back(std::move(request));
        }
        if (complete)
        {
            RayHit hit;
            hit.mId = id;
            hit.mValid = true;
            std::lock_guard<std::mutex> stateLock(mRayState->mMutex);
            if (mRayState->mAlive)
                mRayState->mCompleted.push_back(std::move(hit));
        }
        return id;
    }

    std::vector<GpuScene::RayHit> GpuScene::takeRayHits()
    {
        std::lock_guard<std::mutex> lock(mRayState->mMutex);
        std::vector<RayHit> result;
        result.swap(mRayState->mCompleted);
        return result;
    }

    std::unique_ptr<GpuScene::RayBatch> GpuScene::makeRayBatchLocked(std::vector<RayRequest>&& requests)
    {
        std::unique_ptr<RayBatch> batch = std::make_unique<RayBatch>();
        batch->mRequests = std::move(requests);
        batch->mRays.reserve(batch->mRequests.size());
        for (const RayRequest& request : batch->mRequests)
        {
            const osg::Vec3f delta = request.mTo - request.mFrom;
            RayBatch::GpuRay ray{};
            for (int i = 0; i < 3; ++i)
            {
                ray.mOrigin[i] = request.mFrom[i];
                ray.mDelta[i] = delta[i];
            }
            batch->mRays.push_back(ray);
        }

        std::vector<bool> eligible(batch->mRequests.size());
        std::vector<osg::Matrixf> palette;
        std::size_t first = 0;
        while (first < mRigRegistry.size())
        {
            SceneUtil::PositionAttitudeTransform& actor = *mRigRegistry[first].mActor;
            std::size_t last = first + 1;
            while (last < mRigRegistry.size() && mRigRegistry[last].mActor == &actor)
                ++last;
            const auto found = mActorSnapshots.find(&actor);
            if (found == mActorSnapshots.end() || (found->second.mMask & (Mask_Actor | Mask_Player)) == 0)
            {
                first = last;
                continue;
            }
            const ActorSnapshot& actorState = found->second;

            bool anyEligible = false;
            for (std::size_t ray = 0; ray < batch->mRequests.size(); ++ray)
            {
                const RayRequest& request = batch->mRequests[ray];
                const bool ignored = std::any_of(request.mIgnore.begin(), request.mIgnore.end(),
                    [&](const auto& item) { return item.get() == &actor; });
                eligible[ray] = !ignored && !(request.mIgnorePlayer && (actorState.mMask & Mask_Player));
                anyEligible = anyEligible || eligible[ray];
            }
            if (!anyEligible)
            {
                first = last;
                continue;
            }

            const osg::Matrixf& world = actorState.mWorld;
            for (std::size_t i = first; i < last; ++i)
            {
                const RigEntry& entry = mRigRegistry[i];
                if (entry.mRig->getNodeMask() != Mask_GpuProxy || entry.mMesh >= mMeshes.size())
                    continue;
                const Mesh& mesh = mMeshes[entry.mMesh];
                if (mesh.mGeometry == nullptr || mesh.mBaseVertex < 0 || mesh.mIndexCount < 3)
                {
                    batch->mValid = false;
                    return batch;
                }

                unsigned int snapshot = 0;
                osg::BoundingBox local;
                if (!entry.mRig->getLatestBound(snapshot, local))
                {
                    batch->mValid = false;
                    return batch;
                }
                const osg::BoundingBox bounds = worldBound(local, world);
                bool anyHit = false;
                for (std::size_t ray = 0; ray < batch->mRequests.size(); ++ray)
                {
                    const RayRequest& request = batch->mRequests[ray];
                    if (eligible[ray] && intersects(bounds, request.mFrom, request.mTo))
                    {
                        anyHit = true;
                        break;
                    }
                }
                if (!anyHit)
                    continue;

                if (!entry.mRig->getPalette(snapshot, palette))
                {
                    batch->mValid = false;
                    return batch;
                }
                RaySkin skin;
                skin.mMesh = entry.mMesh;
                skin.mGeometry = mesh.mGeometry;
                skin.mFirst = static_cast<std::uint32_t>(batch->mSkinMatrices.size());
                skin.mCount = static_cast<std::uint32_t>(palette.size());
                batch->mSkinMatrices.resize(batch->mSkinMatrices.size() + palette.size());
                for (std::size_t pose = 0; pose < palette.size(); ++pose)
                    for (int col = 0; col < 3; ++col)
                        for (int row = 0; row < 4; ++row)
                            batch->mSkinMatrices[skin.mFirst + pose].mCols[col][row] = palette[pose](row, col);
                batch->mSkins.push_back(std::move(skin));

                for (std::size_t ray = 0; ray < batch->mRequests.size(); ++ray)
                {
                    const RayRequest& request = batch->mRequests[ray];
                    if (!eligible[ray] || !intersects(bounds, request.mFrom, request.mTo))
                        continue;
                    RayBatch::GpuTask task{};
                    task.mMeta[0] = static_cast<std::uint32_t>(ray);
                    task.mMeta[1] = mesh.mFirstIndex;
                    task.mMeta[2] = static_cast<std::uint32_t>(mesh.mBaseVertex);
                    task.mMeta[3] = mesh.mIndexCount / 3;
                    for (int row = 0; row < 3; ++row)
                        for (int col = 0; col < 4; ++col)
                            task.mRows[row][col] = world(col, row);
                    batch->mMaxTriangles = std::max(batch->mMaxTriangles, task.mMeta[3]);
                    batch->mTasks.push_back(task);
                    batch->mActors.push_back(&actor);
                }
            }
            first = last;
        }
        return batch;
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

    void GpuScene::releaseMeshLocked(Id meshId)
    {
        Mesh& mesh = mMeshes[meshId];
        if (--mesh.mUsers > 0)
            return;
        const std::uint32_t vertex = static_cast<std::uint32_t>(mesh.mBaseVertex);
        std::erase_if(mPendingMeshes, [&](const PendingMesh& pending) {
            return pending.mVertexOffset == vertex && pending.mIndexOffset == mesh.mFirstIndex;
        });
        std::erase_if(mPendingSkins, [&](const PendingSkin& pending) { return pending.mFirst == vertex; });
        mVertices.free(vertex, mesh.mVertexCount);
        mIndices.free(mesh.mFirstIndex, mesh.mIndexCount);
        const auto skin = mSkinMeshes.find(meshId);
        if (skin != mSkinMeshes.end())
        {
            mSkinGroups.free(skin->second.mGroupFirst, skin->second.mGroupCount);
            mSkinInfluences.free(skin->second.mInfluenceFirst, skin->second.mInfluenceCount);
            mSkinMeshes.erase(skin);
        }
        mMorphVersions.erase(
            mMorphVersions.lower_bound({ meshId, 0 }), mMorphVersions.upper_bound({ meshId, sInvalid }));
        mUpdatedMeshes.erase(mesh.mGeometry.get());
        mMeshIds.erase(mesh.mGeometry.get());
        mMeshVersions.erase(mesh.mGeometry.get());
        mesh.mGeometry = nullptr;
        mFreeMeshes.push_back(meshId);
    }

    bool GpuScene::registerSkinLocked(Id mesh, const SceneUtil::RigGeometry& rig)
    {
        const std::vector<std::uint32_t>& groups = rig.getSkinGroups();
        if (groups.size() != mMeshes[mesh].mVertexCount)
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

    void GpuScene::Textures::ArrayTexture::apply(osg::State& state) const
    {
        GLuint name = 0;
        {
            std::lock_guard<std::mutex> lock(mPool.mMutex);
            const auto found = mPool.mContexts.find(state.getContextID());
            if (found != mPool.mContexts.end())
                name = found->second.mName;
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    }

    int GpuScene::Textures::ArrayTexture::compare(const osg::StateAttribute& sa) const
    {
        COMPARE_StateAttribute_Types(ArrayTexture, sa);
        if (&mPool < &rhs.mPool)
            return -1;
        if (&mPool > &rhs.mPool)
            return 1;
        return 0;
    }

    void GpuScene::Textures::uploadLayer(
        osg::State& state, const Pool& pool, const Layer& layer, std::uint32_t index) const
    {
        const osg::Image* image = layer.mImage.get();
        if (image == nullptr || !image->valid())
            return;
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        const StorageKey& key = pool.mKey;
        state.unbindPixelBufferObject();
        glPixelStorei(GL_UNPACK_ALIGNMENT, image->getPacking());
#if !defined(OSG_GLES1_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, image->getRowLength());
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
#endif
        const bool compressed = osg::Texture::isCompressedInternalFormat(key.mInternal);
        const unsigned int levels = key.mGenerated ? 1 : key.mLevels;
        for (unsigned int level = 0; level < levels; ++level)
        {
            const GLsizei width = std::max(key.mWidth >> level, 1);
            const GLsizei height = std::max(key.mHeight >> level, 1);
            const std::size_t offset = image->isMipmap() ? image->getMipmapOffset(level) : 0;
            const unsigned char* data = image->data() + offset;
            if (compressed)
            {
                const std::size_t size = osg::Image::computeImageSizeInBytes(width, height, 1, key.mFormat, key.mType);
                ext->glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0,
                    static_cast<GLint>(index), width, height, 1, static_cast<GLenum>(key.mInternal),
                    static_cast<GLsizei>(size), data);
            }
            else
                ext->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0, static_cast<GLint>(index),
                    width, height, 1, key.mFormat, key.mType, data);
        }
    }

    void GpuScene::Textures::uploadPool(
        osg::State& state, const Pool& pool, PoolGl& gl, std::vector<std::uint32_t>& uploaded) const
    {
        const std::uint32_t count = static_cast<std::uint32_t>(std::min(pool.mLayers.size(), gl.mRevisions.size()));
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const Layer& layer = pool.mLayers[index];
            if (layer.mImage == nullptr)
                continue;
            const unsigned int modified = layer.mImage->getModifiedCount();
            if (gl.mRevisions[index] == layer.mRevision && gl.mModified[index] == modified)
                continue;
            uploadLayer(state, pool, layer, index);
            gl.mRevisions[index] = layer.mRevision;
            gl.mModified[index] = modified;
            uploaded.push_back(index);
        }
    }

    bool GpuScene::Textures::failPool(
        GpuScene& scene, osg::State& state, Pool& pool, GLuint name, std::uint32_t kept, const char* message)
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        glDeleteTextures(1, &name);
        pool.mFailed = true;
        Log(Debug::Warning) << message;
        dropMaterials(scene, pool.mId, kept);
        return false;
    }

    /// Publishes the replacement only when complete.
    bool GpuScene::Textures::allocate(GpuScene& scene, osg::State& state, Pool& pool, PoolGl& gl)
    {
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        const StorageKey& key = pool.mKey;
        PoolGl next;
        next.mCapacity = pool.mCapacity;
        glGenTextures(1, &next.mName);
        state.setActiveTextureUnit(sArrayUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, next.mName);
        clearErrors();
        ext->glTexStorage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLsizei>(key.mLevels), key.mInternal, key.mWidth,
            key.mHeight, static_cast<GLsizei>(pool.mCapacity));
        if (glGetError() != GL_NO_ERROR)
            return failPool(scene, state, pool, next.mName, gl.mCapacity, "GPU texture array storage failed.");
        constexpr std::array<int, 4> identity{ GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
        if (key.mSwizzle != identity)
            glTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_RGBA, key.mSwizzle.data());
        next.mRevisions.assign(pool.mCapacity, 0);
        next.mModified.assign(pool.mCapacity, 0);
        if (gl.mName != 0 && gl.mCapacity != 0)
        {
            if (mCopyImage == nullptr)
                osg::setGLExtensionFuncPtr(mCopyImage, "glCopyImageSubData");
            if (mCopyImage != nullptr)
            {
                const GLsizei depth = static_cast<GLsizei>(std::min(gl.mCapacity, pool.mCapacity));
                for (unsigned int level = 0; level < key.mLevels; ++level)
                    mCopyImage(gl.mName, GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0, 0, next.mName,
                        GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0, 0, std::max(key.mWidth >> level, 1),
                        std::max(key.mHeight >> level, 1), depth);
                // Only copied layers keep their revision.
                if (glGetError() == GL_NO_ERROR)
                {
                    const std::size_t count = std::min<std::size_t>(gl.mRevisions.size(), pool.mCapacity);
                    std::copy_n(gl.mRevisions.begin(), count, next.mRevisions.begin());
                    std::copy_n(gl.mModified.begin(), std::min<std::size_t>(gl.mModified.size(), count),
                        next.mModified.begin());
                }
                else
                    clearErrors();
            }
        }
        std::vector<std::uint32_t> uploaded;
        uploadPool(state, pool, next, uploaded);
        if (!uploaded.empty())
        {
            resetUnpack();
            if (key.mGenerated && ext->glGenerateMipmap != nullptr)
                ext->glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        }
        if (glGetError() != GL_NO_ERROR)
            return failPool(scene, state, pool, next.mName, gl.mCapacity, "GPU texture array upload failed.");
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        stageName(state.getContextID(), gl.mName, static_cast<std::size_t>(gl.mCapacity) * key.mBytes);
        gl = std::move(next);
        return true;
    }

    bool GpuScene::Textures::syncPool(GpuScene& scene, osg::State& state, Pool& pool)
    {
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        if (ext->glTexStorage3D == nullptr)
            return false;
        std::lock_guard<std::mutex> lock(pool.mMutex);
        for (std::uint32_t index = 0; index < pool.mLayers.size(); ++index)
        {
            Layer& layer = pool.mLayers[index];
            if (layer.mState != LayerState::Pending)
                continue;
            layer.mState = LayerState::Staged;
            stageLayer(pool, index);
        }
        if (pool.mCapacity == 0 || pool.mLayers.empty())
            return false;
        PoolGl& gl = pool.mContexts[state.getContextID()];
        const bool attempted = !pool.mFailed && (gl.mName == 0 || gl.mCapacity != pool.mCapacity);
        // A grown pool uploads before it publishes.
        if (attempted && allocate(scene, state, pool, gl))
            return true;
        if (gl.mName == 0)
            return attempted;
        state.setActiveTextureUnit(sArrayUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, gl.mName);
        clearErrors();
        std::vector<std::uint32_t> uploaded;
        uploadPool(state, pool, gl, uploaded);
        if (!uploaded.empty())
        {
            resetUnpack();
            if (pool.mKey.mGenerated && ext->glGenerateMipmap != nullptr)
                ext->glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
            if (glGetError() != GL_NO_ERROR)
            {
                // A cleared revision uploads again later.
                for (const std::uint32_t index : uploaded)
                    gl.mRevisions[index] = 0;
                if (!pool.mWarned)
                {
                    pool.mWarned = true;
                    Log(Debug::Warning) << "GPU texture array upload failed.";
                }
            }
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        return true;
    }

    void GpuScene::Textures::dropMaterials(GpuScene& scene, Id pool, std::uint32_t kept)
    {
        for (Id material = 0; material < scene.mMaterials.size(); ++material)
        {
            if (scene.mMaterialStates[material] == MaterialState::Published)
                continue;
            const PendingMaterial& pending = scene.mMaterials[material];
            bool affected = false;
            for (unsigned int map = 0; map < sMapCount; ++map)
                affected = affected || (pending.mArrays[map] == pool && pending.mLayers[map] >= kept);
            if (!affected)
                continue;
            // Resident chunks keep drawing a stale record.
            scene.publishMaterialLocked(material);
        }
    }

    GpuScene::Textures::Reservation GpuScene::Textures::reserve(osg::Texture2D& texture)
    {
        StorageKey key;
        if (!storage(texture, key))
            return {};
        const osg::Image* image = texture.getImage();
        Id poolId = sInvalid;
        const auto poolFound = mPoolIds.find(key);
        if (poolFound != mPoolIds.end())
            poolId = poolFound->second;
        else
        {
            poolId = static_cast<Id>(mPools.size());
            auto pool = std::make_unique<Pool>();
            pool->mKey = key;
            pool->mId = poolId;
            pool->mTexture = new ArrayTexture(*pool);
            mPoolIds.emplace(key, poolId);
            mPools.push_back(std::move(pool));
        }
        Pool& pool = *mPools[poolId];
        std::lock_guard<std::mutex> lock(pool.mMutex);
        if (pool.mFailed)
            return {};
        const auto imageFound = pool.mImages.find(image);
        if (imageFound != pool.mImages.end())
        {
            Layer& layer = pool.mLayers[imageFound->second];
            ++layer.mUsers;
            ++pool.mUsers;
            return { poolId, imageFound->second };
        }

        auto layerIt = std::find_if(pool.mLayers.begin(), pool.mLayers.end(),
            [](const Layer& layer) { return layer.mState == LayerState::Free; });
        if (layerIt == pool.mLayers.end())
        {
            const std::size_t oldBytes = static_cast<std::size_t>(pool.mCapacity) * key.mBytes;
            const std::size_t otherBytes = mArrayBytes - oldBytes;
            std::size_t headroom = mArrayLimit - std::min(otherBytes, mArrayLimit);
            if (mBudgetBytes != 0)
            {
                const std::size_t used = otherBytes + mProxyBytes + pendingBytes();
                headroom = std::min(headroom, mBudgetBytes - std::min(used, mBudgetBytes));
            }
            const std::uint32_t affordable = static_cast<std::uint32_t>(
                std::min<std::size_t>(headroom / key.mBytes, std::numeric_limits<std::uint32_t>::max()));
            const std::uint32_t maxCapacity = std::min(static_cast<std::uint32_t>(mMaxLayers), affordable);
            std::uint32_t capacity
                = pool.mCapacity == 0 ? std::min(2u, maxCapacity) : std::min(pool.mCapacity * 2, maxCapacity);
            if (capacity < 2 || capacity <= pool.mCapacity)
                return {};
            const std::size_t bytes = static_cast<std::size_t>(capacity) * key.mBytes;
            const std::uint32_t oldCapacity = pool.mCapacity;
            pool.mCapacity = capacity;
            pool.mLayers.resize(capacity);
            for (std::uint32_t layer = oldCapacity; layer < capacity; ++layer)
                pool.mLayers[layer].mState = LayerState::Free;
            mArrayBytes = mArrayBytes - oldBytes + bytes;
            layerIt = pool.mLayers.begin() + oldCapacity;
        }
        const std::uint32_t layer = static_cast<std::uint32_t>(std::distance(pool.mLayers.begin(), layerIt));
        layerIt->mImage = texture.getImage();
        layerIt->mUsers = 1;
        ++layerIt->mRevision;
        layerIt->mState = LayerState::Used;
        pool.mImages.emplace(image, layer);
        ++pool.mUsers;
        ++mArrayGeneration;
        return { poolId, layer };
    }

    void GpuScene::Textures::rollback(const std::vector<Reservation>& reservations)
    {
        for (const Reservation& reservation : reservations)
        {
            if (reservation.mPool == sInvalid || reservation.mPool >= mPools.size()
                || mPools[reservation.mPool] == nullptr)
                continue;
            Pool& pool = *mPools[reservation.mPool];
            std::lock_guard<std::mutex> lock(pool.mMutex);
            Layer& layer = pool.mLayers[reservation.mLayer];
            --pool.mUsers;
            if (--layer.mUsers != 0)
                continue;
            pool.mImages.erase(layer.mImage.get());
            layer.mImage = nullptr;
            // The fence frees the layer for reuse.
            layer.mState = LayerState::Pending;
            ++mArrayGeneration;
            if (pool.mUsers != 0)
                continue;
            mArrayBytes -= static_cast<std::size_t>(pool.mCapacity) * pool.mKey.mBytes;
            for (const auto& [context, gl] : pool.mContexts)
                stageName(context, gl.mName, static_cast<std::size_t>(gl.mCapacity) * pool.mKey.mBytes);
            pool.mContexts.clear();
            pool.mCapacity = 0;
            pool.mLayers.clear();
            pool.mImages.clear();
            pool.mFailed = false;
            pool.mWarned = false;
            ++pool.mLife;
        }
    }

    GpuScene::Id GpuScene::Textures::track(osg::Texture& texture, osg::ref_ptr<osg::Texture>& out)
    {
        out = &texture;
        auto* source = dynamic_cast<osg::Texture2D*>(&texture);
        StorageKey key;
        // Textures without storage stay outside the budget.
        if (source == nullptr || !storage(*source, key))
            return sInvalid;
        const auto entryKey = std::make_pair(source->getImage(), key);
        const auto found = mProxyIds.find(entryKey);
        if (found != mProxyIds.end())
        {
            const Proxy& entry = mProxies[found->second];
            // Only a demoted entry replaces the source.
            if (entry.mCurrent != 0)
                out = entry.mTexture;
            return found->second;
        }
        Proxy proxy;
        proxy.mImage = source->getImage();
        proxy.mSource = source;
        proxy.mKey = key;
        proxy.mBytes = key.mBytes;
        while (proxy.mFloor + 1 < key.mLevels
            && std::max(key.mWidth >> (proxy.mFloor + 1), key.mHeight >> (proxy.mFloor + 1)) >= 64)
            ++proxy.mFloor;
        const Id id = static_cast<Id>(mProxies.size());
        mProxyIds.emplace(entryKey, id);
        mProxies.push_back(std::move(proxy));
        mProxyBytes += key.mBytes;
        return id;
    }

    void GpuScene::Textures::makeBindless(PendingMaterial& material)
    {
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            material.mProxies[i] = sInvalid;
            material.mArrays[i] = sInvalid;
            if (material.mTextures[i] == nullptr)
                continue;
            material.mSamplers[i] = sampler(*material.mTextures[i]);
            osg::ref_ptr<osg::Texture> texture;
            material.mProxies[i] = track(*material.mTextures[i], texture);
            material.mTextures[i] = texture;
        }
        useProxies(material);
    }

    void GpuScene::Textures::useProxies(PendingMaterial& material)
    {
        for (const Id proxy : material.mProxies)
        {
            if (proxy == sInvalid)
                continue;
            Proxy& entry = mProxies[proxy];
            // Maps sharing an entry count one user.
            if (std::find(entry.mMaterials.begin(), entry.mMaterials.end(), material.mIndex) != entry.mMaterials.end())
                continue;
            ++entry.mUsers;
            entry.mMaterials.push_back(material.mIndex);
        }
    }

    void GpuScene::Textures::makeLegacy(
        PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline)
    {
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            material.mProxies[i] = sInvalid;
            material.mArrays[i] = sInvalid;
            if (material.mTextures[i] == nullptr)
                continue;
            material.mSamplers[i] = sampler(*material.mTextures[i]);
            osg::ref_ptr<osg::Texture> texture;
            material.mProxies[i] = track(*material.mTextures[i], texture);
            const int unit = source.mUnits[i];
            // The merged state already binds the source.
            if (texture != material.mTextures[i] && unit >= 0)
            {
                pipeline.setTextureAttribute(static_cast<unsigned int>(unit), texture);
                // The clone carries other sampler state.
                pipeline.setTextureAttribute(static_cast<unsigned int>(unit), material.mSamplers[i]);
            }
            material.mTextures[i] = texture;
        }
        useProxies(material);
    }

    bool GpuScene::Textures::makeArray(PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline)
    {
        std::vector<Reservation> reservations;
        std::map<int, std::tuple<Id, std::uint32_t, osg::Sampler*>> units;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            material.mProxies[i] = sInvalid;
            material.mArrays[i] = sInvalid;
            if (material.mTextures[i] == nullptr)
                continue;
            auto* texture = dynamic_cast<osg::Texture2D*>(material.mTextures[i].get());
            const Reservation reservation = texture == nullptr ? Reservation() : reserve(*texture);
            if (reservation.mPool == sInvalid)
            {
                rollback(reservations);
                warnTexture(material.mTextures[i].get(), "array", "GPU texture uses material draws.");
                return false;
            }
            reservations.push_back(reservation);
            material.mArrays[i] = reservation.mPool;
            material.mLayers[i] = reservation.mLayer;
            material.mRecord.mMaps[i] = static_cast<std::uint64_t>(reservation.mLayer) + 1;
            material.mSamplers[i] = sampler(*texture);
            // Shared units resolve to one texture.
            units.emplace(
                source.mUnits[i], std::make_tuple(reservation.mPool, reservation.mLayer, material.mSamplers[i].get()));
        }
        for (const auto& [unit, binding] : units)
        {
            if (unit < 0)
                continue;
            const Id pool = std::get<0>(binding);
            pipeline.setTextureAttribute(static_cast<unsigned int>(unit), mPools[pool]->mTexture);
            pipeline.setTextureAttribute(static_cast<unsigned int>(unit), std::get<2>(binding));
            pipeline.removeTextureMode(static_cast<unsigned int>(unit), GL_TEXTURE_2D);
        }
        return true;
    }

    void GpuScene::Textures::release(const PendingMaterial& material, osg::State* state)
    {
        std::vector<Reservation> reservations;
        for (unsigned int i = 0; i < sMapCount; ++i)
            if (material.mArrays[i] != sInvalid)
                reservations.push_back({ material.mArrays[i], material.mLayers[i] });
        rollback(reservations);
        for (const Id proxy : material.mProxies)
        {
            if (proxy == sInvalid || proxy >= mProxies.size())
                continue;
            Proxy& entry = mProxies[proxy];
            const auto found = std::find(entry.mMaterials.begin(), entry.mMaterials.end(), material.mIndex);
            if (found == entry.mMaterials.end())
                continue;
            entry.mMaterials.erase(found);
            if (entry.mUsers == 0 || --entry.mUsers != 0)
                continue;
            mProxyIds.erase(std::make_pair(entry.mImage.get(), entry.mKey));
            mProxyBytes -= entry.mBytes;
            // Tier zero belongs to the scene.
            for (std::size_t tier = 1; tier < entry.mTiers.size(); ++tier)
                entry.mTiers[tier].mTexture->releaseGLObjects(state);
            entry.mTiers.clear();
            entry.mTexture = nullptr;
            entry.mTemplate = nullptr;
            entry.mSource = nullptr;
            entry.mImage = nullptr;
        }
    }

    /// Collects finished tiers and starts one build.
    void GpuScene::Textures::prepareTiers()
    {
        if (mTierWork != nullptr)
        {
            if (!mTierWork->isDone())
                return;
            attachTiers(*mTierWork);
            mTierWork = nullptr;
        }
        osg::ref_ptr<TierWork> work = pickTiers();
        if (work == nullptr)
            return;
        if (mWorkQueue == nullptr)
        {
            buildTiers(*work);
            attachTiers(*work);
            return;
        }
        mTierWork = work;
        mWorkQueue->addWorkItem(work);
    }

    /// Builds the template and tier zero.
    void GpuScene::Textures::backProxy(Proxy& proxy)
    {
        proxy.mTemplate = new osg::Texture2D(*proxy.mSource, osg::CopyOp::SHALLOW_COPY);
        proxy.mTemplate->setImage(proxy.mImage);
        proxy.mTemplate->setUnRefImageDataAfterApply(false);
        proxy.mTemplate->setDataVariance(osg::Object::STATIC);
        proxy.mTexture = proxy.mSource;
        proxy.mTiers.push_back({ proxy.mImage, proxy.mSource, proxy.mKey.mBytes });
    }

    osg::ref_ptr<GpuScene::Textures::TierWork> GpuScene::Textures::pickTiers()
    {
        for (Id id = 0; id < mProxies.size(); ++id)
        {
            Proxy& proxy = mProxies[id];
            if (proxy.mPrepared || proxy.mImage == nullptr || proxy.mDesired == 0
                || proxy.mDesired < proxy.mTiers.size())
                continue;
            if (proxy.mTiers.empty())
                backProxy(proxy);
            osg::ref_ptr<TierWork> work = new TierWork;
            work->mProxy = id;
            work->mImage = proxy.mImage;
            work->mTemplate = proxy.mTemplate;
            work->mKey = proxy.mKey;
            work->mFloor = proxy.mFloor;
            return work;
        }
        return nullptr;
    }

    /// Runs without the scene mutex.
    void GpuScene::Textures::buildTiers(TierWork& work)
    {
        const osg::Image& source = *work.mImage;
        for (unsigned int level = 1; level <= work.mFloor; ++level)
        {
            osg::ref_ptr<osg::Image> image;
            if (source.isMipmap())
            {
                image = new osg::Image;
                const unsigned int first = source.getMipmapOffset(level);
                image->setImage(std::max(source.s() >> level, 1), std::max(source.t() >> level, 1), 1,
                    source.getInternalTextureFormat(), source.getPixelFormat(), source.getDataType(),
                    const_cast<unsigned char*>(source.getMipmapData(level)), osg::Image::NO_DELETE,
                    source.getPacking());
                osg::Image::MipmapDataType offsets;
                for (unsigned int tail = level + 1; tail < source.getNumMipmapLevels(); ++tail)
                    offsets.push_back(source.getMipmapOffset(tail) - first);
                image->setMipmapLevels(offsets);
                image->setOrigin(source.getOrigin());
            }
            else if (work.mKey.mGenerated && !source.isCompressed())
            {
                image = new osg::Image(source, osg::CopyOp::DEEP_COPY_ALL);
                image->setMipmapLevels({});
                image->scaleImage(std::max(source.s() >> level, 1), std::max(source.t() >> level, 1), 1);
            }
            if (image == nullptr || !image->valid())
                break;
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(*work.mTemplate, osg::CopyOp::SHALLOW_COPY);
            texture->setImage(image);
            texture->setUseHardwareMipMapGeneration(work.mKey.mGenerated);
            texture->setUnRefImageDataAfterApply(false);
            std::size_t bytes = image->getTotalSizeInBytesIncludingMipmaps();
            if (work.mKey.mGenerated)
            {
                bytes = 0;
                const unsigned int levels = osg::Image::computeNumberOfMipmapLevels(image->s(), image->t());
                for (unsigned int mip = 0; mip < levels; ++mip)
                    bytes += osg::Image::computeImageSizeInBytes(std::max(image->s() >> mip, 1),
                        std::max(image->t() >> mip, 1), 1, image->getPixelFormat(), image->getDataType(),
                        image->getPacking());
            }
            work.mTiers.push_back({ image, texture, bytes });
        }
    }

    void GpuScene::Textures::attachTiers(TierWork& work)
    {
        if (work.mProxy >= mProxies.size())
            return;
        Proxy& proxy = mProxies[work.mProxy];
        // The proxy may have been released.
        if (proxy.mImage != work.mImage || proxy.mPrepared)
            return;
        for (Proxy::Tier& tier : work.mTiers)
            proxy.mTiers.push_back(std::move(tier));
        proxy.mPrepared = true;
        proxy.mFloor = static_cast<unsigned int>(proxy.mTiers.size() - 1);
        proxy.mDesired = std::min(proxy.mDesired, proxy.mFloor);
    }

    void GpuScene::Textures::switchTier(GpuScene& scene, Id proxyId, unsigned int tier)
    {
        Proxy& proxy = mProxies[proxyId];
        if (tier >= proxy.mTiers.size() || tier == proxy.mCurrent || proxy.mRetiring)
            return;
        const osg::ref_ptr<osg::Texture2D> oldTexture = proxy.mTexture;
        const std::size_t oldBytes = proxy.mBytes;
        const std::size_t newBytes = proxy.mTiers[tier].mBytes;
        bool changed = false;
        for (const Id material : proxy.mMaterials)
        {
            if (material >= scene.mMaterials.size() || scene.mMaterialStates[material] == MaterialState::Published)
                continue;
            PendingMaterial pending = scene.mMaterials[material];
            for (unsigned int map = 0; map < sMapCount; ++map)
                if (pending.mProxies[map] == proxyId)
                    pending.mTextures[map] = proxy.mTiers[tier].mTexture;
            if (pending.mMode == Shader::MaterialMode::Bindless)
                scene.mPendingMaterials.push_back(pending);
            else if (pending.mMode == Shader::MaterialMode::Legacy && scene.mMaterialSources[material] != nullptr)
            {
                const auto surface = scene.mSurfaces.find(scene.mMaterialPaths[material]);
                if (surface != scene.mSurfaces.end())
                    for (unsigned int map = 0; map < sMapCount; ++map)
                    {
                        const int unit = scene.mMaterialSources[material]->mUnits[map];
                        // The clone carries other sampler state.
                        if (pending.mProxies[map] == proxyId && unit >= 0)
                            scene.mBucketEdits.push_back(BucketEdit{ surface->second.mBucket,
                                static_cast<unsigned int>(unit), proxy.mTiers[tier].mTexture, pending.mSamplers[map] });
                    }
            }
            scene.mMaterials[material] = std::move(pending);
            changed = true;
        }
        if (!changed)
            return;
        PendingRetire retired;
        retired.mMaterial.mIndex = sInvalid;
        retired.mMaterial.mProxies.fill(sInvalid);
        retired.mMaterial.mArrays.fill(sInvalid);
        // The fence frees the old storage.
        retired.mMaterial.mTextures[0] = oldTexture;
        retired.mBytes = oldBytes;
        mPendingBytes += oldBytes;
        retired.mProxy = proxyId;
        scene.mPendingRetired.push_back(std::move(retired));
        mProxyBytes = mProxyBytes - oldBytes + newBytes;
        proxy.mTexture = proxy.mTiers[tier].mTexture;
        proxy.mBytes = newBytes;
        proxy.mCurrent = tier;
        proxy.mDesired = tier;
        proxy.mRetiring = true;
        ++mMipChanges;
    }

    void GpuScene::Textures::manage(GpuScene& scene, unsigned int frame)
    {
        if (!mResolved || mBudgetBytes == 0)
            return;
        const auto active = [&](const Proxy& proxy) {
            return std::any_of(proxy.mMaterials.begin(), proxy.mMaterials.end(), [&](Id material) {
                return material < scene.mMaterialStates.size()
                    && scene.mMaterialStates[material] != MaterialState::Published
                    && (scene.mMaterialUsers[material] != 0 || frame <= scene.mMaterialLast[material] + 3);
            });
        };
        for (Proxy& proxy : mProxies)
            for (const Id material : proxy.mMaterials)
                if (material < scene.mMaterialLast.size())
                    proxy.mLastVisible = std::max(proxy.mLastVisible, scene.mMaterialLast[material]);

        // Retiring bytes leave after their fence.
        while (mArrayBytes + mProxyBytes > mBudgetBytes)
        {
            Id candidate = sInvalid;
            for (Id id = 0; id < mProxies.size(); ++id)
            {
                const Proxy& proxy = mProxies[id];
                if (proxy.mImage == nullptr || proxy.mRetiring || !active(proxy) || proxy.mCurrent >= proxy.mFloor)
                    continue;
                if (candidate == sInvalid || proxy.mLastVisible < mProxies[candidate].mLastVisible)
                    candidate = id;
            }
            if (candidate == sInvalid)
                break;
            Proxy& proxy = mProxies[candidate];
            proxy.mDesired = proxy.mCurrent + 1;
            if (proxy.mDesired >= proxy.mTiers.size())
                break;
            switchTier(scene, candidate, proxy.mDesired);
        }

        const std::size_t promoteLimit = mBudgetBytes * 85 / 100;
        while (mArrayBytes + mProxyBytes + pendingBytes() < promoteLimit)
        {
            Id candidate = sInvalid;
            for (Id id = 0; id < mProxies.size(); ++id)
            {
                const Proxy& proxy = mProxies[id];
                if (proxy.mImage == nullptr || proxy.mRetiring || !active(proxy) || proxy.mCurrent == 0)
                    continue;
                const std::size_t projected = mArrayBytes + mProxyBytes + pendingBytes() - proxy.mBytes
                    + proxy.mTiers[proxy.mCurrent - 1].mBytes;
                if (projected > promoteLimit)
                    continue;
                if (candidate == sInvalid || proxy.mLastVisible > mProxies[candidate].mLastVisible)
                    candidate = id;
            }
            if (candidate == sInvalid)
                break;
            switchTier(scene, candidate, mProxies[candidate].mCurrent - 1);
        }
    }

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

    GpuScene::PendingMaterial GpuScene::makeMaterial(MaterialSource& source, Id index)
    {
        PendingMaterial material;
        material.mIndex = index;
        material.mProxies.fill(sInvalid);
        material.mArrays.fill(sInvalid);
        material.mLayers.fill(0);
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
                    : resolvePair<osg::StateSet::RefAttributePair>(
                          source.mPath,
                          [&](const osg::StateSet& stateSet) {
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
        // Texture unit per map, four bits each.
        std::uint32_t unitsLow = 0;
        std::uint32_t unitsHigh = 0;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            const std::uint32_t unit = source.mUnits[i] > 0 ? static_cast<std::uint32_t>(source.mUnits[i]) & 15u : 0u;
            if (i < 8)
                unitsLow |= unit << (i * 4);
            else
                unitsHigh |= unit << ((i - 8) * 4);
        }
        record.mModes[1] = static_cast<std::int32_t>(unitsLow);
        record.mModes[2] = static_cast<std::int32_t>(unitsHigh);
        for (unsigned int unit = 0; unit < 8; ++unit)
        {
            const osg::Matrixf texMat = uniformValue(source.mTexMats[unit], osg::Matrixf::identity());
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 4; ++col)
                    record.mTexMatRows[unit * 2 + row][col] = texMat(col, row);
        }
        return material;
    }

    void GpuScene::refreshMaterialLocked(Id material, unsigned int frame, bool samplers)
    {
        if (material >= mMaterials.size() || mMaterialFrames[material] == frame)
            return;
        mMaterialFrames[material] = frame;
        const Shader::MaterialMode mode = mMaterials[material].mMode;
        if (mMaterialSources[material] == nullptr)
            return;
        if (mode == Shader::MaterialMode::Array)
        {
            // Array refresh is not transactional yet.
            if (samplers)
                rebindSamplersLocked(material);
            PendingMaterial pending = makeMaterial(*mMaterialSources[material], material);
            PendingMaterial& current = mMaterials[material];
            if (pending.mTextures != current.mTextures)
                return;
            std::copy(std::begin(current.mRecord.mMaps), std::end(current.mRecord.mMaps), pending.mRecord.mMaps);
            if (std::memcmp(&pending.mRecord, &current.mRecord, sizeof(GpuMaterial)) != 0)
            {
                current.mRecord = pending.mRecord;
                mPendingMaterials.push_back(current);
            }
            return;
        }
        const bool bindless = mode == Shader::MaterialMode::Bindless;
        // A demoted map keeps its bucket sampler.
        if (samplers && !bindless)
            rebindSamplersLocked(material);
        const bool forced = samplers && bindless;
        PendingMaterial pending = makeMaterial(*mMaterialSources[material], material);
        pending.mMode = mode;
        const PendingMaterial& current = mMaterials[material];
        const auto changed = [&] {
            if (bindless && std::memcmp(&pending.mRecord, &current.mRecord, sizeof(GpuMaterial)) != 0)
                return true;
            for (unsigned int i = 0; i < sMapCount; ++i)
                if (pending.mTextures[i].get() != current.mTextures[i].get())
                    return true;
            return false;
        };
        // Demoted maps publish a tier texture.
        if (!forced && !mTextures->demoted(current) && !changed())
            return;
        if (bindless)
            mTextures->makeBindless(pending);
        else
        {
            // Bucket edits carry the bindings instead.
            const osg::ref_ptr<osg::StateSet> scratch = new osg::StateSet;
            mTextures->makeLegacy(pending, *mMaterialSources[material], *scratch);
        }
        if (!forced && !changed())
            return;
        if (!bindless)
            rebindMapsLocked(material, pending);
        PendingMaterial retired = current;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            if (retired.mProxies[i] == pending.mProxies[i])
                retired.mProxies[i] = sInvalid;
            if (retired.mArrays[i] == pending.mArrays[i] && retired.mLayers[i] == pending.mLayers[i])
                retired.mArrays[i] = sInvalid;
        }
        mPendingRetired.push_back(PendingRetire{ std::move(retired), false });
        mMaterials[material] = pending;
        // Legacy shaders read no material record.
        if (bindless)
            mPendingMaterials.push_back(std::move(pending));
    }

    /// Rebinds changed maps on a live bucket.
    void GpuScene::rebindMapsLocked(Id material, const PendingMaterial& pending)
    {
        const auto surface = mSurfaces.find(mMaterialPaths[material]);
        if (surface == mSurfaces.end() || surface->second.mBucket >= mBuckets.size()
            || !mBuckets[surface->second.mBucket].mLive)
            return;
        const MaterialSource& source = *mMaterialSources[material];
        const PendingMaterial& current = mMaterials[material];
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            const int unit = source.mUnits[i];
            if (unit < 0 || pending.mTextures[i] == nullptr || pending.mTextures[i] == current.mTextures[i])
                continue;
            // The clone carries other sampler state.
            const osg::ref_ptr<osg::Sampler> replacement
                = mTextures->demoted(pending, i) ? pending.mSamplers[i] : nullptr;
            mBucketEdits.push_back(BucketEdit{
                surface->second.mBucket, static_cast<unsigned int>(unit), pending.mTextures[i], replacement });
        }
    }

    /// Rebinds bucket samplers after a filter change.
    void GpuScene::rebindSamplersLocked(Id material)
    {
        const auto surface = mSurfaces.find(mMaterialPaths[material]);
        if (surface == mSurfaces.end() || surface->second.mBucket >= mBuckets.size())
            return;
        const Id bucket = surface->second.mBucket;
        PendingMaterial& current = mMaterials[material];
        const MaterialSource& source = *mMaterialSources[material];
        const bool legacy = current.mMode == Shader::MaterialMode::Legacy;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            const int unit = source.mUnits[i];
            if (current.mTextures[i] == nullptr || unit < 0)
                continue;
            // Only a demoted legacy map holds one.
            const bool demoted = mTextures->demoted(current, i);
            if (legacy && !demoted)
                continue;
            const osg::Texture* origin = current.mTextures[i].get();
            if (demoted && source.mTextures[i] != nullptr)
                origin = dynamic_cast<const osg::Texture*>(source.mTextures[i]->first.get());
            if (origin == nullptr)
                continue;
            const osg::ref_ptr<osg::Sampler> replacement = mTextures->sampler(*origin);
            if (replacement == current.mSamplers[i])
                continue;
            current.mSamplers[i] = replacement;
            // Materials sharing a unit share its sampler.
            const auto queued = [&](const BucketEdit& edit) {
                return edit.mBucket == bucket && edit.mUnit == static_cast<unsigned int>(unit);
            };
            if (std::none_of(mBucketEdits.begin(), mBucketEdits.end(), queued))
                mBucketEdits.push_back(BucketEdit{ bucket, static_cast<unsigned int>(unit), nullptr, replacement });
        }
    }

    void GpuScene::refreshSamplers()
    {
        if (!isSupported())
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        for (Id material = 0; material < mMaterials.size(); ++material)
        {
            if (mMaterialStates[material] == MaterialState::Published)
                continue;
            mMaterialFrames[material] = std::numeric_limits<unsigned int>::max();
            refreshMaterialLocked(material, mLifetimeFrame, true);
        }
    }

    void GpuScene::addMaterialLocked(Id material)
    {
        if (material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published)
            return;
        ++mMaterialUsers[material];
        mMaterialStates[material] = MaterialState::Active;
    }

    void GpuScene::dropMaterialLocked(Id material)
    {
        if (material < mMaterialUsers.size() && mMaterialUsers[material] != 0)
            --mMaterialUsers[material];
    }

    void GpuScene::markMaterialLocked(Id material, unsigned int frame)
    {
        if (material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published)
            return;
        if (mMaterialLast[material] == frame)
            return;
        mMaterialLast[material] = std::max(mMaterialLast[material], frame);
        mMaterialStates[material] = MaterialState::Active;
    }

    void GpuScene::retireMaterialsLocked(unsigned int frame)
    {
        constexpr unsigned int grace = 3;
        for (Id material = 0; material < mMaterialStates.size(); ++material)
        {
            MaterialState& state = mMaterialStates[material];
            if (state == MaterialState::Published)
                continue;
            if (mMaterialUsers[material] != 0 || frame <= mMaterialLast[material] + grace)
            {
                state = MaterialState::Active;
                continue;
            }
            if (state == MaterialState::Active)
            {
                state = MaterialState::Queued;
                continue;
            }
            publishMaterialLocked(material);
        }
    }

    /// Retires the slot; its id never returns.
    void GpuScene::publishMaterialLocked(Id material)
    {
        const auto surface = mSurfaces.find(mMaterialPaths[material]);
        if (surface != mSurfaces.end())
        {
            const Id bucketId = surface->second.mBucket;
            mSurfaces.erase(surface);
            if (bucketId < mBuckets.size() && mBuckets[bucketId].mMaterials != 0 && --mBuckets[bucketId].mMaterials == 0
                && mBuckets[bucketId].mMode == Shader::MaterialMode::Legacy)
            {
                mBucketEdits.push_back(BucketEdit{ bucketId, 0, nullptr, nullptr });
            }
        }
        std::erase_if(mPendingMaterials, [&](const PendingMaterial& pending) { return pending.mIndex == material; });
        mPendingRetired.push_back(PendingRetire{ std::move(mMaterials[material]), true });
        mMaterials[material] = PendingMaterial{};
        mMaterials[material].mIndex = material;
        mMaterialSources[material].reset();
        mMaterialPaths[material].clear();
        mMaterialStates[material] = MaterialState::Published;
    }

    bool GpuScene::bucketEdited(Id bucket) const
    {
        return std::any_of(
            mBucketEdits.begin(), mBucketEdits.end(), [&](const BucketEdit& edit) { return edit.mBucket == bucket; });
    }

    /// Runs on the graphics thread.
    void GpuScene::applyBucketEdits()
    {
        for (const BucketEdit& edit : mBucketEdits)
        {
            if (edit.mBucket >= mBuckets.size())
                continue;
            osg::StateSet& bucket = *mBuckets[edit.mBucket].mStateSet;
            if (edit.mTexture != nullptr)
                bucket.setTextureAttribute(edit.mUnit, edit.mTexture);
            if (edit.mSampler != nullptr)
            {
                bucket.setTextureAttribute(edit.mUnit, edit.mSampler);
                for (bool full : { false, true })
                {
                    const auto actor = mActorBuckets.find({ edit.mBucket, full });
                    if (actor != mActorBuckets.end() && actor->second != edit.mBucket)
                    {
                        osg::StateSet& actorState = *mBuckets[actor->second].mStateSet;
                        actorState.setTextureAttribute(edit.mUnit, edit.mSampler);
                        mGl->mPrepassStates.erase(&actorState);
                    }
                }
            }
            if (edit.mTexture == nullptr && edit.mSampler == nullptr)
            {
                for (unsigned int unit = 0; unit < bucket.getNumTextureAttributeLists(); ++unit)
                {
                    bucket.removeTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                    bucket.removeTextureAttribute(unit, osg::StateAttribute::SAMPLER);
                }
                bucket.removeAttribute(osg::StateAttribute::MATERIAL);
            }
            // The next prepass rebuilds the clone.
            mGl->mPrepassStates.erase(&bucket);
            ++mAppliedEdits;
        }
        mBucketEdits.clear();
    }

    GpuScene::Surface GpuScene::getSurface(
        const std::vector<const osg::StateSet*>& statePath, int order, MaterialUpdate updates)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return getSurfaceLocked(statePath, order, updates);
    }

    GpuScene::Surface GpuScene::getSurfaceLocked(
        const std::vector<const osg::StateSet*>& statePath, int order, MaterialUpdate updates)
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
        if (program == nullptr)
        {
            mSurfaces.emplace(std::move(key), surface);
            return surface;
        }

        const Id materialId = static_cast<Id>(mMaterials.size());
        std::unique_ptr<MaterialSource> source = makeSource(statePath, *program);
        PendingMaterial material = makeMaterial(*source, materialId);
        Shader::MaterialMode materialMode = mMaterialMode;
        // Array refresh is not transactional yet.
        if (updates == MaterialUpdate::State && materialMode == Shader::MaterialMode::Array)
            materialMode = Shader::MaterialMode::Legacy;
        osg::ref_ptr<osg::StateSet> pipeline = new osg::StateSet(*merged, osg::CopyOp::SHALLOW_COPY);
        if (materialMode == Shader::MaterialMode::Bindless)
            mTextures->makeBindless(material);
        else if (materialMode == Shader::MaterialMode::Array && !mTextures->makeArray(material, *source, *pipeline))
        {
            materialMode = Shader::MaterialMode::Legacy;
            pipeline = merged;
            material = makeMaterial(*source, materialId);
        }
        if (materialMode == Shader::MaterialMode::Legacy)
            mTextures->makeLegacy(material, *source, *pipeline);
        material.mMode = materialMode;
        osg::ref_ptr<osg::Program> instanced = mShaderManager.getInstancedProgram(*program, materialMode);
        if (instanced == nullptr && materialMode == Shader::MaterialMode::Array)
        {
            mTextures->release(material);
            materialMode = Shader::MaterialMode::Legacy;
            pipeline = merged;
            material = makeMaterial(*source, materialId);
            mTextures->makeLegacy(material, *source, *pipeline);
            material.mMode = materialMode;
            instanced = mShaderManager.getInstancedProgram(*program, materialMode);
        }
        if (instanced == nullptr || (updates == MaterialUpdate::Record && materialMode == Shader::MaterialMode::Legacy))
        {
            mTextures->release(material);
            mSurfaces.emplace(std::move(key), surface);
            return surface;
        }

        if (materialMode != Shader::MaterialMode::Legacy)
        {
            std::vector<unsigned int> materialUnits;
            for (unsigned int i = 0; i < sMapCount; ++i)
            {
                const int unit = source->mUnits[i];
                if (unit < 0)
                    continue;
                materialUnits.push_back(static_cast<unsigned int>(unit));
                if (materialMode == Shader::MaterialMode::Bindless)
                    pipeline->removeUniform(Shader::sMaterialMapNames[i]);
            }
            if (materialMode == Shader::MaterialMode::Bindless)
            {
                std::sort(materialUnits.begin(), materialUnits.end());
                materialUnits.erase(std::unique(materialUnits.begin(), materialUnits.end()), materialUnits.end());
                for (const unsigned int unit : materialUnits)
                {
                    pipeline->removeTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                    pipeline->removeTextureMode(unit, GL_TEXTURE_2D);
                }
            }
            pipeline->removeAttribute(osg::StateAttribute::MATERIAL);
            for (const char* name : sMaterialUniforms)
                pipeline->removeUniform(name);
        }
        surface.mMaterial = material.mIndex;
        mMaterials.push_back(material);
        mMaterialSources.push_back(std::move(source));
        mMaterialFrames.push_back(std::numeric_limits<unsigned int>::max());
        mMaterialUsers.push_back(0);
        mMaterialLast.push_back(mLifetimeFrame);
        mMaterialStates.push_back(MaterialState::Active);
        mMaterialPaths.push_back(key);
        if (materialMode != Shader::MaterialMode::Legacy)
            mPendingMaterials.push_back(std::move(material));
        pipeline->setAttributeAndModes(instanced, osg::StateAttribute::ON);
        if (order != 0)
            pipeline->setRenderBinDetails(order - 1, "RenderBin");

        // Drop empty lists so states compare equal.
        osg::ref_ptr<osg::StateSet> canonical = new osg::StateSet;
        for (const auto& [mode, value] : pipeline->getModeList())
            canonical->setMode(mode, value);
        for (const auto& [typeMember, attrPair] : pipeline->getAttributeList())
            canonical->setAttribute(const_cast<osg::StateAttribute*>(attrPair.first.get()), attrPair.second);
        for (const auto& [name, uniformPair] : pipeline->getUniformList())
            canonical->addUniform(const_cast<osg::Uniform*>(uniformPair.first->asUniform()), uniformPair.second);
        for (unsigned int unit = 0; unit < pipeline->getTextureModeList().size(); ++unit)
            for (const auto& [mode, value] : pipeline->getTextureModeList()[unit])
                canonical->setTextureMode(unit, mode, value);
        for (unsigned int unit = 0; unit < pipeline->getNumTextureAttributeLists(); ++unit)
            for (const auto& [typeMember, attrPair] : pipeline->getTextureAttributeList()[unit])
                canonical->setTextureAttribute(
                    unit, const_cast<osg::StateAttribute*>(attrPair.first.get()), attrPair.second);
        if (pipeline->useRenderBinDetails())
            canonical->setRenderBinDetails(
                pipeline->getBinNumber(), pipeline->getBinName(), pipeline->getRenderBinMode());
        else
            canonical->setRenderingHint(pipeline->getRenderingHint());
        canonical->setNestRenderBins(pipeline->getNestRenderBins());
        pipeline = canonical;

        const bool ownBucket = updates != MaterialUpdate::None && materialMode == Shader::MaterialMode::Legacy;
        surface.mBucket = getBucketLocked(*pipeline, *instanced, order, materialMode, ownBucket);
        ++mBuckets[surface.mBucket].mMaterials;
        mSurfaces.emplace(std::move(key), surface);
        return surface;
    }

    GpuScene::Id GpuScene::getBucketLocked(
        osg::StateSet& pipeline, const osg::Program& program, int order, Shader::MaterialMode mode, bool live)
    {
        Id bucketId = sInvalid;
        for (Id id = 0; !live && id < mBuckets.size(); ++id)
        {
            if (bucketEdited(id) || mBuckets[id].mLive)
                continue;
            if (mBuckets[id].mStateSet->compare(pipeline, true) == 0)
            {
                bucketId = id;
                break;
            }
        }
        if (bucketId == sInvalid)
        {
            bucketId = static_cast<Id>(mBuckets.size());
            Bucket bucket;
            bucket.mStateSet = &pipeline;
            bucket.mProgram = &program;
            bucket.mMode = mode;
            bucket.mLive = live;
            bucket.mDelta = pipeline.getUniformList().empty()
                && std::all_of(pipeline.getTextureModeList().begin(), pipeline.getTextureModeList().end(),
                    [](const osg::StateSet::ModeList& modes) { return modes.empty(); })
                && std::all_of(pipeline.getTextureAttributeList().begin(), pipeline.getTextureAttributeList().end(),
                    [](const osg::StateSet::AttributeList& attributes) { return attributes.empty(); });
            // Every bucket state feeds the reset lists.
            for (const auto& [glMode, value] : pipeline.getModeList())
                if (std::find(mDeltaModes.begin(), mDeltaModes.end(), glMode) == mDeltaModes.end())
                    mDeltaModes.push_back(glMode);
            for (const auto& [typeMember, attribute] : pipeline.getAttributeList())
                if (typeMember.first != osg::StateAttribute::PROGRAM
                    && std::find(mDeltaAttributes.begin(), mDeltaAttributes.end(), typeMember)
                        == mDeltaAttributes.end())
                    mDeltaAttributes.push_back(typeMember);
            mProgramsToLink.push_back(const_cast<osg::Program*>(&program));
            mBuckets.push_back(std::move(bucket));

            BinKey binKey;
            binKey.mHint = pipeline.getRenderingHint();
            binKey.mNumber = pipeline.getBinNumber();
            binKey.mMode = pipeline.getRenderBinMode();
            binKey.mName = pipeline.getBinName();
            binKey.mNested = pipeline.getNestRenderBins();
            auto dispatcher = std::find_if(mDispatchers.begin(), mDispatchers.end(),
                [&](const Dispatcher& candidate) { return candidate.mKey == binKey; });
            if (dispatcher == mDispatchers.end())
            {
                const Id id = static_cast<Id>(mDispatchers.size());
                Dispatcher created;
                created.mKey = binKey;
                created.mStateSet = new osg::StateSet;
                if (pipeline.useRenderBinDetails())
                    created.mStateSet->setRenderBinDetails(
                        pipeline.getBinNumber(), pipeline.getBinName(), pipeline.getRenderBinMode());
                else
                    created.mStateSet->setRenderingHint(pipeline.getRenderingHint());
                created.mStateSet->setNestRenderBins(pipeline.getNestRenderBins());
                created.mDrawable = new DispatchDrawable(*this, id);
                created.mDrawable->setStateSet(created.mStateSet);
                created.mOrdered = order != 0;
                mPendingDrawables.emplace_back(created.mDrawable, created.mOrdered);
                mDispatchers.push_back(std::move(created));
                dispatcher = std::prev(mDispatchers.end());
            }
            dispatcher->mBuckets.push_back(bucketId);
        }
        return bucketId;
    }

    GpuScene::Id GpuScene::getActorBucket(Id bucket, bool full)
    {
        const auto found = mActorBuckets.find({ bucket, full });
        if (found != mActorBuckets.end())
            return found->second;
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet(*mBuckets[bucket].mStateSet, osg::CopyOp::SHALLOW_COPY);
        state->removeAttribute(osg::StateAttribute::FRONTFACE);
        for (unsigned int unit = 0; unit < state->getNumTextureAttributeLists(); ++unit)
            state->removeTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
        auto& attributes = state->getTextureAttributeList();
        while (!attributes.empty() && attributes.back().empty())
            attributes.pop_back();
        osg::ref_ptr<const osg::Program> program = mBuckets[bucket].mProgram;
        if (full)
        {
            program = mShaderManager.getInstancedProgram(*program, mMaterialMode, true);
            if (program == nullptr)
                return sInvalid;
            state->setAttributeAndModes(const_cast<osg::Program*>(program.get()), osg::StateAttribute::ON);
        }
        const Id result = getBucketLocked(*state, *program, 0, mBuckets[bucket].mMode, false);
        mActorBuckets.emplace(std::make_pair(bucket, full), result);
        return result;
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
        if (match != nullptr
            && (match->mSurface.mMaterial >= scene.mMaterialStates.size()
                || scene.mMaterialStates[match->mSurface.mMaterial] == GpuScene::MaterialState::Published))
            match = nullptr;
        if (match != nullptr)
        {
            match->mMesh = mesh;
            if (liveMaterial)
                scene.refreshMaterialLocked(match->mSurface.mMaterial, frame);
            return { mesh, match->mSurface };
        }

        const GpuScene::Surface surface = scene.getSurfaceLocked(
            path, 0, liveMaterial ? GpuScene::MaterialUpdate::State : GpuScene::MaterialUpdate::None);
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
        Terrain::TerrainDrawable& drawable, const osg::Vec3f& worldOffset, bool lit)
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
            instance.mBox = box;
            instance.mSmallFeatureCulling = true;
            instance.mGeometry = &drawable;
            instance.mBucket = surface.mBucket;
            instance.mMaterial = surface.mMaterial;
            instances.push_back(instance);
        }
        osg::ref_ptr<osg::Node> chunk = createChunk(std::move(instances), drawable.getBound(), lit);
        if (chunk && lit)
            chunk->addCullCallback(new SceneUtil::LightListCallback);
        return chunk;
    }

    void GpuScene::setCastingPrograms(const std::vector<osg::ref_ptr<osg::Program>>& programs)
    {
        mCastingProgramList = programs;
        for (const osg::ref_ptr<osg::Program>& program : programs)
        {
            const std::array modes = { mMaterialMode, Shader::MaterialMode::Legacy };
            for (std::size_t i = 0; i < modes.size() && (i == 0 || modes[i] != modes[0]); ++i)
            {
                if (osg::ref_ptr<osg::Program> instanced = mShaderManager.getInstancedProgram(*program, modes[i]))
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mProgramsToLink.push_back(std::move(instanced));
                }
            }
        }
    }

    unsigned int GpuScene::getPendingWork() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return static_cast<unsigned int>(mPendingMeshes.size() + mPendingInstances.size() + mPendingSkins.size()
            + mPendingMaterials.size() + mPendingRetired.size() + mProgramsToLink.size());
    }

    void GpuScene::reportStats(unsigned int frame, osg::Stats& stats) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const std::size_t bytes = mTextures->mArrayBytes + mTextures->mProxyBytes + mTextures->pendingBytes();
        const std::size_t overage
            = mTextures->mBudgetBytes != 0 && bytes > mTextures->mBudgetBytes ? bytes - mTextures->mBudgetBytes : 0;
        std::size_t legacy = 0;
        for (Id material = 0; material < mMaterialStates.size(); ++material)
            if (mMaterialStates[material] != MaterialState::Published
                && mMaterials[material].mMode == Shader::MaterialMode::Legacy)
                ++legacy;
        stats.setAttribute(frame, "GPU texture bytes", static_cast<double>(bytes));
        stats.setAttribute(frame, "GPU texture budget", static_cast<double>(mTextures->mBudgetBytes));
        stats.setAttribute(frame, "GPU receiver slots", static_cast<double>(mReceiverSlots.mExtent));
        stats.setAttribute(frame, "GPU array bytes", static_cast<double>(mTextures->mArrayBytes));
        const std::size_t pools = static_cast<std::size_t>(std::count_if(mTextures->mPools.begin(),
            mTextures->mPools.end(), [](const auto& pool) { return pool != nullptr && pool->mCapacity != 0; }));
        stats.setAttribute(frame, "GPU array pools", static_cast<double>(pools));
        stats.setAttribute(frame, "GPU material buckets", static_cast<double>(mBuckets.size()));
        std::size_t commands = 0;
        for (const Bucket& bucket : mBuckets)
            commands += bucket.mCommandCount;
        stats.setAttribute(frame, "GPU draw commands", static_cast<double>(commands));
        stats.setAttribute(frame, "GPU texture handles", static_cast<double>(mTextureHandles.load()));
        stats.setAttribute(frame, "GPU mip changes", static_cast<double>(mTextures->mMipChanges));
        stats.setAttribute(frame, "GPU bucket edits", static_cast<double>(mAppliedEdits));
        stats.setAttribute(frame, "GPU legacy materials", static_cast<double>(legacy));
        stats.setAttribute(frame, "GPU texture overage", static_cast<double>(overage));
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

    namespace
    {
        /// Accepts chains of UV controllers only.
        bool collectLiveUpdaters(osg::Callback& callback, std::vector<std::uintptr_t>& key,
            std::vector<osg::ref_ptr<osg::Callback>>& updaters)
        {
            const auto accept = [&](const SceneUtil::StateSetUpdater* controller) {
                const auto* uv = dynamic_cast<const NifOsg::UVController*>(controller);
                if (uv == nullptr)
                    return false;
                for (const void* part : uv->getAnimationKey())
                    key.push_back(reinterpret_cast<std::uintptr_t>(part));
                for (const unsigned int unit : uv->getTextureUnits())
                    key.push_back(unit);
                key.push_back(0);
                return true;
            };
            for (osg::Callback* cb = &callback; cb != nullptr; cb = cb->getNestedCallback())
            {
                if (auto* composite = dynamic_cast<SceneUtil::CompositeStateSetUpdater*>(cb))
                {
                    for (std::size_t i = 0; i < composite->getNumControllers(); ++i)
                        if (!accept(composite->getController(i)))
                            return false;
                }
                else if (!accept(dynamic_cast<const SceneUtil::StateSetUpdater*>(cb)))
                    return false;
                updaters.emplace_back(cb);
            }
            return !updaters.empty();
        }
    }

    osg::ref_ptr<const osg::StateSet> GpuScene::liveStateSet(const osg::Callback& callback)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        // Legacy shaders read no material record.
        if (mMaterialMode == Shader::MaterialMode::Legacy)
            return nullptr;
        std::vector<std::uintptr_t> key;
        std::vector<osg::ref_ptr<osg::Callback>> updaters;
        // Updaters mutate their interpolator caches.
        if (!collectLiveUpdaters(const_cast<osg::Callback&>(callback), key, updaters))
            return nullptr;
        LiveMaterials& entry = mLiveMaterials[key];
        if (entry.mStateSet == nullptr)
        {
            entry.mStateSet = new osg::StateSet;
            entry.mUpdaters = std::move(updaters);
            for (const osg::ref_ptr<osg::Callback>& updater : entry.mUpdaters)
                dynamic_cast<SceneUtil::StateSetUpdater&>(*updater).setDefaults(entry.mStateSet);
        }
        return entry.mStateSet;
    }

    void GpuScene::bindLiveMaterial(const osg::StateSet& stateSet, Id material)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& [key, entry] : mLiveMaterials)
        {
            if (entry.mStateSet != &stateSet)
                continue;
            if (std::find(entry.mMaterials.begin(), entry.mMaterials.end(), material) == entry.mMaterials.end())
                entry.mMaterials.push_back(material);
            return;
        }
    }

    void GpuScene::refreshLiveMaterials(osg::NodeVisitor& nv)
    {
        const unsigned int frame = nv.getFrameStamp()->getFrameNumber();
        std::lock_guard<std::mutex> lock(mMutex);
        if (mLiveFrame == frame)
            return;
        mLiveFrame = frame;
        for (auto it = mLiveMaterials.begin(); it != mLiveMaterials.end();)
        {
            LiveMaterials& entry = it->second;
            std::erase_if(entry.mMaterials, [&](Id material) {
                return material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published;
            });
            if (entry.mMaterials.empty())
            {
                if (entry.mStateSet->referenceCount() == 1)
                    it = mLiveMaterials.erase(it);
                else
                    ++it;
                continue;
            }
            const bool visible = std::any_of(entry.mMaterials.begin(), entry.mMaterials.end(),
                [&](Id material) { return mMaterialUsers[material] != 0 || frame <= mMaterialLast[material] + 3; });
            if (visible)
            {
                for (const osg::ref_ptr<osg::Callback>& updater : entry.mUpdaters)
                    dynamic_cast<SceneUtil::StateSetUpdater&>(*updater).apply(entry.mStateSet, &nv);
                for (const Id material : entry.mMaterials)
                    refreshMaterialLocked(material, frame);
            }
            ++it;
        }
    }

    bool GpuScene::isTemplateLive(const GpuTemplate& tmpl) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const GpuGeometry& geometry : tmpl.mGeometries)
        {
            const Id material = geometry.mSurface.mMaterial;
            if (material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published)
                return false;
        }
        return true;
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
            instance.mBox = geometry.mBounds;
            instance.mSmallFeatureCulling = smallFeatureCulling;
            instance.mGeometry = geometry.mGeometry;
            instance.mBucket = geometry.mSurface.mBucket;
            instance.mMaterial = geometry.mSurface.mMaterial;
            out.push_back(std::move(instance));
        }
    }

    osg::ref_ptr<osg::Node> GpuScene::createChunk(
        std::vector<Instance>&& instances, const osg::BoundingSphere& bound, bool lit)
    {
        if (instances.empty())
            return nullptr;

        std::lock_guard<std::mutex> lock(mMutex);
        for (const Instance& instance : instances)
            if (getMeshLocked(*instance.mGeometry) == sInvalid || instance.mMaterial >= mMaterialStates.size()
                || mMaterialStates[instance.mMaterial] == MaterialState::Published)
                return nullptr;
        Chunk chunk;
        chunk.mGeneration = mNextGeneration++;
        if (mNextGeneration == 0)
            mNextGeneration = 1;
        chunk.mInstanceCount = static_cast<std::uint32_t>(instances.size());
        chunk.mFirstInstance = mInstances.allocate(chunk.mInstanceCount);
        if (lit)
        {
            // Slot zero stays the unlit sentinel.
            if (mReceiverSlots.mExtent == 0)
                mReceiverSlots.allocate(1);
            chunk.mReceiverSlot = mReceiverSlots.allocate(1);
            if (chunk.mReceiverSlot == sInvalid)
                chunk.mReceiverSlot = 0;
        }

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
        pending.mOcclusion.reserve(instances.size());
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            const Instance& instance = instances[i];
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
            chunk.mMaterials.push_back(instance.mMaterial);
            pending.mRecords.push_back(makeRecord(instance, batch, slot, chunk.mGeneration, chunk.mReceiverSlot));
            pending.mOcclusion.push_back(makeOcclusion(instance));
            chunk.mBounds.expandBy(instance.mBounds);
        }
        chunk.mBatchCounts.assign(batchCounts.begin(), batchCounts.end());
        std::sort(chunk.mMeshes.begin(), chunk.mMeshes.end());
        chunk.mMeshes.erase(std::unique(chunk.mMeshes.begin(), chunk.mMeshes.end()), chunk.mMeshes.end());
        for (const Id meshId : chunk.mMeshes)
            ++mMeshes[meshId].mUsers;
        std::sort(chunk.mMaterials.begin(), chunk.mMaterials.end());
        chunk.mMaterials.erase(std::unique(chunk.mMaterials.begin(), chunk.mMaterials.end()), chunk.mMaterials.end());
        for (const Id material : chunk.mMaterials)
            addMaterialLocked(material);
        mResidentBound.expandBy(chunk.mBounds);
        mBoundDirty = true;
        mBatchesDirty = true;
        mPendingInstances.push_back(std::move(pending));
        mChunks[slot] = std::move(chunk);
        osg::ref_ptr<GpuChunkNode> node = new GpuChunkNode(*this, slot, mChunks[slot].mGeneration, lit, bound);
        mChunkNodes.insert(node.get());
        return node;
    }

    GpuScene::GpuInstance GpuScene::makeRecord(const Instance& instance, Id batch, Id slot, Id generation, Id lightSlot)
    {
        GpuInstance record{};
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
        record.mMaterial[1] = lightSlot;
        return record;
    }

    GpuScene::GpuOcclusion GpuScene::makeOcclusion(const Instance& instance)
    {
        GpuOcclusion record{};
        for (int i = 0; i < 3; ++i)
        {
            record.mMin[i] = instance.mBox._min[i];
            record.mMax[i] = instance.mBox._max[i];
        }
        return record;
    }

    bool GpuScene::updateChunk(
        osg::Node& node, const std::vector<Instance>& instances, const osg::BoundingSphere& bound)
    {
        auto* chunkNode = dynamic_cast<GpuChunkNode*>(&node);
        if (chunkNode == nullptr)
            return false;
        std::lock_guard<std::mutex> lock(mMutex);
        const Id slot = chunkNode->getSlot();
        Chunk& chunk = mChunks[slot];
        if (chunk.mInstanceCount != instances.size())
            return false;
        std::map<Id, std::uint32_t> counts;
        for (const Instance& instance : instances)
        {
            const auto mesh = mMeshIds.find(instance.mGeometry.get());
            if (mesh == mMeshIds.end()
                || std::find(chunk.mMaterials.begin(), chunk.mMaterials.end(), instance.mMaterial)
                    == chunk.mMaterials.end())
                return false;
            const auto batch = mBatchIds.find({ instance.mBucket, mesh->second });
            if (batch == mBatchIds.end())
                return false;
            ++counts[batch->second];
        }
        // Changed batches need new slot ranges.
        if (!std::equal(counts.begin(), counts.end(), chunk.mBatchCounts.begin(), chunk.mBatchCounts.end(),
                [](const auto& left, const auto& right) {
                    return left.first == right.first && left.second == right.second;
                }))
            return false;
        chunkNode->setBound(bound);
        if (chunkNode->mResident != GpuChunkNode::sNone)
            mResidents[chunkNode->mResident].mBound = bound;
        PendingInstances pending;
        pending.mFirst = chunk.mFirstInstance;
        pending.mRecords.reserve(instances.size());
        pending.mOcclusion.reserve(instances.size());
        chunk.mBounds = osg::BoundingSphere();
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            const Instance& instance = instances[i];
            const Id meshId = mMeshIds.at(instance.mGeometry.get());
            const Id batch = mBatchIds.at(std::make_pair(instance.mBucket, meshId));
            pending.mRecords.push_back(makeRecord(instance, batch, slot, chunk.mGeneration, chunk.mReceiverSlot));
            pending.mOcclusion.push_back(makeOcclusion(instance));
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
        removeResidentLocked(node);
        mChunkNodes.erase(&node);
        Chunk& chunk = mChunks[slot];
        for (const auto& [batch, count] : chunk.mBatchCounts)
            mBatches[batch].mResident -= count;
        for (const Id meshId : chunk.mMeshes)
            releaseMeshLocked(meshId);
        for (const Id material : chunk.mMaterials)
            dropMaterialLocked(material);
        mInstances.free(chunk.mFirstInstance, chunk.mInstanceCount);
        if (chunk.mReceiverSlot != 0)
            mReceiverSlots.free(chunk.mReceiverSlot, 1);
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
        // Runs beside dynamic refreshes.
        refreshLiveMaterials(cv);
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
                it = mViews.erase(it);
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
            viewIt->second.mReceivers.clear();
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
        view.mOcclusion = cv.getCurrentCamera() == mPrepassCamera && cv.getProjectionMatrix() != nullptr
            && cv.getModelViewMatrix() != nullptr;
        if (view.mOcclusion)
        {
            view.mViewProj = osg::Matrixf(*cv.getModelViewMatrix() * *cv.getProjectionMatrix());
        }
        view.mHasParams = true;
        const bool nearFar = cv.getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR;
        const osg::Matrix& modelView = *cv.getModelViewMatrix();
        if (cv.getTraversalMask() & (Mask_Static | Mask_Object | Mask_Terrain))
        {
            for (const Resident& resident : mResidents)
            {
                if (cv.isCulled(resident.mBound))
                    continue;
                if (resident.mSlot >= mChunks.size() || mChunks[resident.mSlot].mGeneration != resident.mGeneration)
                    continue;
                const Chunk& chunk = mChunks[resident.mSlot];
                view.mChunks.emplace_back(resident.mSlot, resident.mGeneration);
                if (resident.mLit && mLightManager && mLightManager->getClassicLighting())
                {
                    osg::BoundingSphere bound = resident.mBound;
                    SceneUtil::transformBoundingSphere(modelView, bound);
                    view.mReceivers.push_back({ chunk.mReceiverSlot, mLightManager->addReceiver(cv, bound) });
                }
                for (const Id material : chunk.mMaterials)
                    markMaterialLocked(material, frame);
                if (nearFar && resident.mBound.valid())
                {
                    osg::BoundingBox box;
                    box.expandBy(resident.mBound);
                    cv.updateCalculatedNearFar(modelView, box);
                }
            }
        }
        recordRegisteredRigs(cv, view, nearFar, modelView);
    }

    void GpuScene::recordRegisteredRigs(
        osgUtil::CullVisitor& cv, ViewRecord& view, bool nearFar, const osg::Matrix& modelView)
    {
        if (!isEnabled() || !mSkinSupported.load(std::memory_order_relaxed))
            return;
        if (!(cv.getTraversalMask() & (Mask_Actor | Mask_Player)))
            return;
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        std::size_t first = 0;
        while (first < mRigRegistry.size())
        {
            SceneUtil::PositionAttitudeTransform& actor = *mRigRegistry[first].mActor;
            std::size_t last = first + 1;
            while (last < mRigRegistry.size() && mRigRegistry[last].mActor == &actor)
                ++last;
            if (!(actor.getNodeMask() & cv.getTraversalMask()) || cv.isCulled(actor.getBound()))
            {
                first = last;
                continue;
            }

            osg::Matrix matrix;
            actor.computeLocalToWorldMatrix(matrix, nullptr);
            const osg::Matrixf world(matrix);
            std::uint32_t receiver = 0;
            if (mLightManager && mLightManager->getClassicLighting())
            {
                const RigEntry& entry = mRigRegistry[first];
                osg::BoundingSphere bound = entry.mLightNode->getBound();
                SceneUtil::transformBoundingSphere(world * modelView, bound);
                receiver = mLightManager->addReceiver(
                    cv, bound, entry.mLightList ? &entry.mLightList->getIgnoredLightSources() : nullptr);
            }
            const auto batches = mActorBatches.find(&actor);
            if (batches != mActorBatches.end())
            {
                for (const auto& batch : batches->second)
                    recordActorBatch(cv, view, batch, world, receiver);
                if (nearFar)
                {
                    osg::BoundingBox bound;
                    bound.expandBy(actor.getBound());
                    cv.updateCalculatedNearFar(modelView, bound);
                }
            }
            for (std::size_t i = first; i < last; ++i)
            {
                const RigEntry& entry = mRigRegistry[i];
                if (entry.mBatched)
                    continue;
                SceneUtil::RigGeometry& rig = *entry.mRig;
                if (rig.getNodeMask() != Mask_GpuProxy)
                    continue;
                unsigned int captureFrame = 0;
                osg::BoundingBox local;
                if (!rig.getLatestBound(captureFrame, local))
                    continue;
                osg::BoundingBox bound = worldBound(local, world);
                if (cv.isCulled(bound))
                    continue;

                SkinUpdate skin;
                if (!getSkinPose(frame, rig, skin.mPose, captureFrame))
                    continue;
                if (skin.mPose.mSnapshot != captureFrame)
                {
                    if (!rig.getBound(skin.mPose.mSnapshot, local))
                        continue;
                    bound = worldBound(local, world);
                    if (cv.isCulled(bound))
                        continue;
                }

                DynamicInstance instance;
                instance.mWorld = world;
                instance.mMesh = entry.mMesh;
                instance.mBucket = entry.mSurface.mBucket;
                instance.mMaterial = entry.mSurface.mMaterial;
                instance.mReceiver = receiver;
                view.mDynamic.push_back(std::move(instance));
                markMaterialLocked(entry.mSurface.mMaterial, frame);
                skin.mMesh = entry.mMesh;
                view.mSkins.push_back(skin);
                if (nearFar && bound.valid())
                    cv.updateCalculatedNearFar(modelView, bound);
            }
            first = last;
        }
    }

    void GpuScene::recordActorBatch(osgUtil::CullVisitor& cv, ViewRecord& view,
        const std::shared_ptr<ActorBatch>& batch, const osg::Matrixf& world, std::uint32_t receiver)
    {
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        SkinUpdate skin;
        skin.mMesh = batch->mMesh;
        skin.mBatch = batch;
        if (!getBatchPose(frame, *batch, skin.mPose))
            return;
        for (const ActorPart& part : batch->mParts)
        {
            if (part.mMorph != nullptr)
            {
                SkinUpdate morph;
                morph.mMesh = batch->mMesh;
                morph.mVertex = part.mVertex;
                morph.mMaterial = part.mMaterial;
                morph.mPalette = part.mPalette;
                morph.mMorph = &part.mMorph->morph(cv.getTraversalNumber());
                view.mSkins.push_back(std::move(morph));
            }
            markMaterialLocked(part.mMaterial, frame);
        }
        view.mSkins.push_back(skin);
        DynamicInstance instance;
        instance.mBatch = batch;
        instance.mWorld = world;
        instance.mMesh = batch->mMesh;
        instance.mBucket = batch->mBucket;
        instance.mReceiver = receiver;
        view.mDynamic.push_back(std::move(instance));
    }

    void GpuScene::addResident(osg::Node& node)
    {
        auto* chunk = dynamic_cast<GpuChunkNode*>(&node);
        if (chunk == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        chunk->mResident = mResidents.size();
        mResidents.push_back(Resident{ chunk->getSlot(), chunk->getGeneration(), chunk->getBound(), chunk->isLit() });
        mResidentNodes.push_back(chunk);
    }

    void GpuScene::removeResident(osg::Node& node)
    {
        auto* chunk = dynamic_cast<GpuChunkNode*>(&node);
        if (chunk == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        removeResidentLocked(*chunk);
    }

    void GpuScene::removeResidentLocked(GpuChunkNode& chunk)
    {
        if (chunk.mResident == GpuChunkNode::sNone)
            return;
        const std::size_t index = chunk.mResident;
        mResidents[index] = mResidents.back();
        mResidentNodes[index] = mResidentNodes.back();
        mResidentNodes[index]->mResident = index;
        mResidents.pop_back();
        mResidentNodes.pop_back();
        chunk.mResident = GpuChunkNode::sNone;
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

    bool GpuScene::getSkinPose(unsigned int frame, SceneUtil::RigGeometry& rig, SkinPose& out, unsigned int snapshot)
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
        unsigned int paletteFrame = 0;
        if (snapshot == sInvalid)
        {
            if (!rig.getLatestPalette(paletteFrame, matrices))
                return false;
        }
        else
        {
            paletteFrame = snapshot;
            if (!rig.getPalette(snapshot, matrices))
                return false;
        }
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
            out = { frame, paletteFrame, static_cast<std::uint32_t>(slot.mMatrices.size()),
                static_cast<std::uint32_t>(pose.size()) };
            slot.mMatrices.insert(slot.mMatrices.end(), pose.begin(), pose.end());
            slot.mPoses.emplace(geometry, out);
        }
        return true;
    }

    bool GpuScene::record(osgUtil::CullVisitor& cv, osg::Geometry& geometry, bool updateMesh,
        const GpuCaptureCallback& callback, SkinUpdate* skin)
    {
        static const std::string sLightCluster = "lightCluster";
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
                || stateSet->getUniform(sLightCluster) != nullptr)
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
        if (updateMesh)
            instance.mGeometry = &geometry;
        instance.mWorld = world;
        instance.mUpdateMesh = updateMesh;
        if (mLightManager && mLightManager->getClassicLighting())
            instance.mReceiver = mLightManager->currentReceiver();

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
        markMaterialLocked(surface.mMaterial, frame);
        ViewRecord& view = mViews[ViewKey(frame, cv.getCurrentCamera())];
        view.mDynamic.push_back(std::move(instance));
        if (skin != nullptr && skin->mPose.mCount != 0)
        {
            skin->mMesh = mesh;
            view.mSkins.push_back(*skin);
        }
        return true;
    }

    void GpuScene::markChunk(osgUtil::CullVisitor& cv, Id slot, Id generation, bool lit)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        ViewRecord& view = mViews[ViewKey(frame, cv.getCurrentCamera())];
        view.mChunks.emplace_back(slot, generation);
        if (slot >= mChunks.size() || mChunks[slot].mGeneration != generation)
            return;
        const Chunk& chunk = mChunks[slot];
        if (lit && mLightManager && mLightManager->getClassicLighting())
            view.mReceivers.push_back({ chunk.mReceiverSlot, mLightManager->currentReceiver() });
        for (const Id material : chunk.mMaterials)
            markMaterialLocked(material, frame);
    }

    void GpuScene::update(unsigned int frame)
    {
        // Releases drain on the next enable.
        if (!isEnabled())
            return;
        std::vector<std::pair<osg::ref_ptr<osg::Drawable>, bool>> drawables;
        bool boundDirty = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mLifetimeFrame = std::max(mLifetimeFrame, frame);
            retireMaterialsLocked(frame);
            mTextures->prepareTiers();
            mTextures->manage(*this, frame);
            if ((!mSupported.load(std::memory_order_relaxed) || !mSkinSupported.load(std::memory_order_relaxed))
                && !mRigRegistry.empty())
            {
                restoreRigsLocked();
                while (!mRigRegistry.empty())
                    removeActorLocked(*mRigRegistry.front().mActor);
                updateRigCullLocked();
            }
            mActorSnapshots.clear();
            std::size_t first = 0;
            while (first < mRigRegistry.size())
            {
                SceneUtil::PositionAttitudeTransform& actor = *mRigRegistry[first].mActor;
                std::size_t last = first + 1;
                while (last < mRigRegistry.size() && mRigRegistry[last].mActor == &actor)
                    ++last;
                osg::Matrix matrix;
                actor.computeLocalToWorldMatrix(matrix, nullptr);
                mActorSnapshots.emplace(&actor, ActorSnapshot{ actor.getNodeMask(), osg::Matrixf(matrix) });
                first = last;
            }
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

    void GpuScene::rebuildBatches(PendingWork& work)
    {
        std::vector<GpuBatch>& table = work.mBatches;
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
        work.mSlotCount = slot;
        work.mCommandCount = command;
        work.mBatchesRebuilt = true;
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
        take(mPendingRetired, work.mRetired);
        if (mBatchesDirty && mPendingMeshes.empty() && mPendingInstances.empty() && mPendingSkins.empty()
            && mPendingMaterials.empty() && mPendingRetired.empty())
        {
            rebuildBatches(work);
        }
    }

    void GpuScene::upload(osg::State& state, PendingWork& work)
    {
        probeArrays(state);
        Gl& gl = *mGl;
        // Array states leave a sampler bound otherwise.
        if (!gl.mSamplerResetBound)
        {
            gl.mSamplerResetBound = true;
            gl.mSamplerReset = new SamplerReset;
            for (unsigned int unit = 0; unit < sSamplerUnits; ++unit)
                state.setGlobalDefaultTextureAttribute(unit, gl.mSamplerReset);
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            applyBucketEdits();
            mTextures->sync(*this, state);
        }

        std::size_t vertexBytes = 0;
        std::size_t indexBytes = 0;
        for (const PendingMesh& mesh : work.mMeshes)
        {
            vertexBytes = std::max(vertexBytes, mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size());
            indexBytes = std::max(indexBytes, (mesh.mIndexOffset + mesh.mIndices.size()) * sizeof(std::uint32_t));
        }
        std::size_t instanceBytes = 0;
        std::size_t occlusionBytes = 0;
        for (const PendingInstances& pending : work.mInstances)
        {
            instanceBytes = std::max(instanceBytes, (pending.mFirst + pending.mRecords.size()) * sizeof(GpuInstance));
            occlusionBytes
                = std::max(occlusionBytes, (pending.mFirst + pending.mOcclusion.size()) * sizeof(GpuOcclusion));
        }
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
        gl.grow(gl.mInstanceBuffer, gl.mInstanceCapacity, instanceBytes, GL_DYNAMIC_COPY);
        gl.grow(gl.mOcclusionBuffer, gl.mOcclusionCapacity, occlusionBytes, GL_DYNAMIC_COPY);
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
        {
            gl.upload(gl.mInstanceBuffer, pending.mFirst * sizeof(GpuInstance), pending.mRecords.data(),
                pending.mRecords.size() * sizeof(GpuInstance));
            gl.upload(gl.mOcclusionBuffer, pending.mFirst * sizeof(GpuOcclusion), pending.mOcclusion.data(),
                pending.mOcclusion.size() * sizeof(GpuOcclusion));
        }
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
            if (material.mMode == Shader::MaterialMode::Bindless)
                gl.setMaterial(state, material);
            gl.upload(
                gl.mMaterialBuffer, material.mIndex * sizeof(GpuMaterial), &material.mRecord, sizeof(GpuMaterial));
        }
        for (PendingRetire& retired : work.mRetired)
            gl.retireMaterial(std::move(retired));
        mTextureHandles.store(gl.mHandles.size(), std::memory_order_relaxed);
        mCompletedWork += static_cast<unsigned int>(work.mMeshes.size() + work.mInstances.size() + work.mSkins.size()
            + work.mMaterials.size() + work.mRetired.size());
        if (work.mBatchesRebuilt)
        {
            const std::vector<GpuBatch>& batches = work.mBatches;
            const std::size_t batchBytes = std::max<std::size_t>(batches.size(), 1) * sizeof(GpuBatch);
            gl.grow(gl.mBatchBuffer, gl.mBatchCapacity, batchBytes, GL_DYNAMIC_COPY);
            if (!batches.empty())
                gl.upload(gl.mBatchBuffer, 0, batches.data(), batches.size() * sizeof(GpuBatch));
            gl.mBatchCount = static_cast<std::uint32_t>(batches.size());
            gl.mSlotCount = work.mSlotCount;
            gl.mCommandCount = work.mCommandCount;
            gl.mViewCapacity = 0;
        }
    }

    void GpuScene::compile(osg::State& state)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTextures->resolve(state);
            const unsigned int frame = state.getFrameStamp() != nullptr ? state.getFrameStamp()->getFrameNumber() : 0;
            mTextures->prepareTiers();
            mTextures->manage(*this, frame);
        }
        if (!mGl->mLimitsChecked)
        {
            mGl->mLimitsChecked = true;
            if (osg::isGLExtensionSupported(mGl->mExt->contextID, "GL_KHR_robustness")
                || osg::isGLExtensionSupported(mGl->mExt->contextID, "GL_ARB_robustness"))
                osg::setGLExtensionFuncPtr(
                    mGl->mGetResetStatus, "glGetGraphicsResetStatus", "glGetGraphicsResetStatusARB");
            GLint storageBindings = 0;
            GLint vertexStorageBlocks = 0;
            GLint fragmentStorageBlocks = 0;
            GLint combinedStorageBlocks = 0;
            glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &storageBindings);
            glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertexStorageBlocks);
            glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragmentStorageBlocks);
            glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, &combinedStorageBlocks);
            GLint64 storageLimit = 0;
            mGl->mExt->glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &storageLimit);
            mGl->mStorageLimit = static_cast<std::size_t>(storageLimit);
            Log(Debug::Info) << "GPU storage: bindings=" << storageBindings << " vertexBlocks=" << vertexStorageBlocks
                             << " fragmentBlocks=" << fragmentStorageBlocks
                             << " combinedBlocks=" << combinedStorageBlocks << " blockBytes=" << storageLimit;
            if (storageBindings <= sMaxStorageBinding || vertexStorageBlocks < sVertexStorageBlocks
                || fragmentStorageBlocks < sFragmentStorageBlocks
                || combinedStorageBlocks < sVertexStorageBlocks + sFragmentStorageBlocks)
            {
                Log(Debug::Warning) << "GPU-driven object paging unavailable: too few storage buffer bindings";
                mSupported = false;
                return;
            }
        }
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

    void GpuScene::releaseGLObjects(osg::State* state)
    {
        if (state == nullptr || mGl == nullptr)
            return;
        Gl& gl = *mGl;
        gl.cleanup(*this, *state);
        mTextureHandles.store(0, std::memory_order_relaxed);
        GLuint buffers[] = { gl.mVertexBuffer, gl.mIndexBuffer, gl.mInstanceBuffer, gl.mBatchBuffer, gl.mChunkBuffer,
            gl.mCountBuffer, gl.mVisibleBuffer, gl.mCommandBuffer, gl.mViewBuffer, gl.mOcclusionBuffer,
            gl.mMaterialBuffer, gl.mDynamicInstanceBuffer, gl.mDynamicVisibleBuffer, gl.mDynamicCommandBuffer,
            gl.mSkinSourceBuffer, gl.mSkinGroupBuffer, gl.mSkinRangeBuffer, gl.mSkinInfluenceBuffer, gl.mSkinTaskBuffer,
            gl.mSkinMatrixBuffer, gl.mRayBuffer, gl.mRayTaskBuffer };
        gl.mExt->glDeleteBuffers(static_cast<GLsizei>(std::size(buffers)), buffers);
        gl.mExt->glDeleteBuffers(static_cast<GLsizei>(gl.mReceiverBuffers.size()), gl.mReceiverBuffers.data());
        gl.mReceiverBuffers.clear();
        gl.mVertexBuffer = 0;
        gl.mIndexBuffer = 0;
        gl.mInstanceBuffer = 0;
        gl.mBatchBuffer = 0;
        gl.mChunkBuffer = 0;
        gl.mCountBuffer = 0;
        gl.mVisibleBuffer = 0;
        gl.mCommandBuffer = 0;
        gl.mViewBuffer = 0;
        gl.mOcclusionBuffer = 0;
        gl.mMaterialBuffer = 0;
        gl.mDynamicInstanceBuffer = 0;
        gl.mDynamicVisibleBuffer = 0;
        gl.mDynamicCommandBuffer = 0;
        gl.mSkinSourceBuffer = 0;
        gl.mSkinGroupBuffer = 0;
        gl.mSkinRangeBuffer = 0;
        gl.mSkinInfluenceBuffer = 0;
        gl.mSkinTaskBuffer = 0;
        gl.mSkinMatrixBuffer = 0;
        gl.mRayBuffer = 0;
        gl.mRayTaskBuffer = 0;
        GLuint vaos[] = { gl.mVao, gl.mDynamicVao };
        gl.mExt->glDeleteVertexArrays(static_cast<GLsizei>(std::size(vaos)), vaos);
        gl.mVao = 0;
        gl.mDynamicVao = 0;
        if (gl.mPyramid != 0)
            glDeleteTextures(1, &gl.mPyramid);
        gl.mPyramid = 0;
        std::lock_guard<std::mutex> lock(mMutex);
        mTextures->releaseGLObjects(state);
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

        class ProbeOperation : public osg::GraphicsOperation
        {
        public:
            explicit ProbeOperation(GpuScene& scene)
                : osg::GraphicsOperation("GpuSceneProbe", true)
                , mScene(scene)
            {
            }

            void operator()(osg::GraphicsContext* context) override
            {
                mScene.probeArrays(*context->getState());
                setKeep(false);
            }

        private:
            GpuScene& mScene;
        };

        constexpr std::string_view sProbeVertex = R"GLSL(#version 430 compatibility
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

        constexpr std::string_view sProbeFragment = R"GLSL(#version 430 compatibility
uniform sampler2DArray probeMap;
void main()
{
    gl_FragColor = texture(probeMap, vec3(gl_FragCoord.xy * 0.25, 0.0));
}
)GLSL";

        /// Object defines an array material variant needs.
        Shader::ShaderManager::DefineMap arrayDefines()
        {
            Shader::ShaderManager::DefineMap defines;
            for (const char* name : Shader::sMaterialMapNames)
            {
                defines[name] = "1";
                defines[std::string(name) + "UV"] = "0";
            }
            for (const char* name : { "additiveBlending", "alphaToCoverage", "diffuseParallax", "parallax",
                     "particleOcclusion", "reconstructNormalZ", "softParticles" })
                defines[name] = "0";
            defines["alphaFunc"] = std::to_string(GL_GREATER);
            defines["adjustCoverage"] = "1";
            defines["useGPUShader4"] = "1";
            defines["gpuUber"] = "1";
            Shader::ShaderManager::applyInstancedDefines(defines, Shader::MaterialMode::Array);
            return defines;
        }
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createCompileOperation(unsigned int target)
    {
        mCompletedWork = 0;
        return new CompileOperation(*this, target);
    }

    bool GpuScene::getBatchPose(unsigned int frame, const ActorBatch& batch, SkinPose& out)
    {
        SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
        std::lock_guard<std::mutex> lock(slot.mMutex);
        if (!slot.mValid || slot.mFrame != frame)
        {
            slot.mFrame = frame;
            slot.mValid = true;
            slot.mPoses.clear();
            slot.mMatrices.clear();
        }
        const auto found = slot.mPoses.find(&batch);
        if (found != slot.mPoses.end())
        {
            out = found->second;
            return true;
        }
        const auto first = static_cast<std::uint32_t>(slot.mMatrices.size());
        const auto append = [&](const osg::Matrixf& matrix) {
            GpuSkinMatrix packed;
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 4; ++row)
                    packed.mCols[col][row] = matrix(row, col);
            slot.mMatrices.push_back(packed);
        };
        thread_local std::vector<osg::Matrixf> palette;
        for (const ActorPart& part : batch.mParts)
        {
            if (part.mRig == nullptr)
                continue;
            unsigned int captured;
            if (!part.mRig->getLatestPalette(captured, palette) || palette.size() != part.mMatrixCount)
            {
                slot.mMatrices.resize(first);
                return false;
            }
            for (const osg::Matrixf& matrix : palette)
                append(matrix);
        }
        thread_local std::vector<osg::Matrix> matrices;
        matrices.resize(batch.mTransforms.size());
        for (std::size_t i = 0; i < batch.mTransforms.size(); ++i)
        {
            const ActorBatch::Transform& transform = batch.mTransforms[i];
            matrices[i] = transform.mParent != sInvalid ? matrices[transform.mParent] : osg::Matrix::identity();
            transform.mNode->computeLocalToWorldMatrix(matrices[i], nullptr);
            append(matrices[i]);
        }
        append(osg::Matrixf::identity());
        out = { frame, frame, first, batch.mMatrixCount };
        slot.mPoses.emplace(&batch, out);
        return true;
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createProbeOperation()
    {
        if (mArraysProbed.load(std::memory_order_relaxed))
            return nullptr;
        return new ProbeOperation(*this);
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createRetireOperation()
    {
        if (!mSupported || mRetireOperation != nullptr)
            return nullptr;
        mRetireOperation = new RetireOperation(*this);
        return mRetireOperation;
    }

    void GpuScene::finishFrame(osg::State& state)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTextures->resolve(state);
        }
        if (mGl != nullptr)
            mGl->fenceRetired();
    }

    bool GpuScene::linkArrayShaders(osg::State& state)
    {
        const auto linked = [&](osg::Program* program) {
            if (program == nullptr)
                return false;
            state.applyAttribute(program);
            osg::Program::PerContextProgram* pcp = program->getPCP(state);
            return pcp != nullptr && pcp->isLinked();
        };
        const Shader::ShaderManager::DefineMap defines = arrayDefines();
        osg::ref_ptr<osg::Shader> vertex = mShaderManager.getShader("objects.vert", defines);
        osg::ref_ptr<osg::Shader> fragment = mShaderManager.getShader("objects.frag", defines);
        if (vertex == nullptr || fragment == nullptr)
            return false;
        osg::ref_ptr<osg::Program> objects = mShaderManager.getProgram(vertex, fragment);
        if (objects != nullptr && !objects->getAttribBindingList().contains("instanceSlot"))
            objects->addBindAttribLocation("instanceSlot", Shader::ShaderManager::sInstanceSlotAttribute);
        if (!linked(objects))
            return false;
        const auto casting = std::find_if(mCastingProgramList.begin(), mCastingProgramList.end(),
            [](const osg::ref_ptr<osg::Program>& program) { return program != nullptr; });
        if (casting == mCastingProgramList.end())
            return true;
        return linked(mShaderManager.getInstancedProgram(**casting, Shader::MaterialMode::Array));
    }

    bool GpuScene::sampleArrayLayer(osg::State& state)
    {
        osg::GLExtensions& ext = *mGl->mExt;
        if (ext.glTexStorage3D == nullptr || ext.glTexSubImage3D == nullptr || ext.glGenFramebuffers == nullptr
            || ext.glGenRenderbuffers == nullptr || ext.glGenVertexArrays == nullptr)
            return false;
        constexpr GLsizei size = 4;
        constexpr std::size_t pixelBytes = static_cast<std::size_t>(size) * size * 4;
        std::array<std::uint8_t, pixelBytes> pattern{};
        for (std::size_t i = 0; i < pattern.size(); ++i)
            pattern[i] = static_cast<std::uint8_t>(i * 11 + 3);
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX, std::string(sProbeVertex)));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT, std::string(sProbeFragment)));

        GLuint layers = 0;
        GLuint frame = 0;
        GLuint color = 0;
        GLuint vao = 0;
        glGenTextures(1, &layers);
        ext.glGenFramebuffers(1, &frame);
        ext.glGenRenderbuffers(1, &color);
        ext.glGenVertexArrays(1, &vao);
        clearErrors();
        state.setActiveTextureUnit(sArrayUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, layers);
        if (ext.glBindSampler != nullptr)
            ext.glBindSampler(sArrayUnit, 0);
        ext.glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, size, size, 1);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        resetUnpack();
        ext.glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
        state.applyAttribute(program);
        osg::Program::PerContextProgram* pcp = program->getPCP(state);
        bool valid = pcp != nullptr && pcp->isLinked() && glGetError() == GL_NO_ERROR;
        if (valid)
        {
            std::array<std::uint8_t, pixelBytes> read{};
            const osg::ref_ptr<osg::Uniform> map = new osg::Uniform("probeMap", static_cast<int>(sArrayUnit));
            pcp->apply(*map);
            glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT);
            GLint previousDraw = 0;
            GLint previousRead = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &previousDraw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &previousRead);
            ext.glBindRenderbuffer(GL_RENDERBUFFER_EXT, color);
            ext.glRenderbufferStorage(GL_RENDERBUFFER_EXT, GL_RGBA8, size, size);
            ext.glBindRenderbuffer(GL_RENDERBUFFER_EXT, 0);
            ext.glBindFramebuffer(GL_FRAMEBUFFER_EXT, frame);
            ext.glFramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
            valid = ext.glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
            if (valid)
            {
                for (const GLenum mode : { GL_SCISSOR_TEST, GL_CULL_FACE, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
                         GL_DITHER, GL_COLOR_LOGIC_OP, GL_ALPHA_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE_ARB })
                    glDisable(mode);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glViewport(0, 0, size, size);
                const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                    ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                    : 0;
                state.bindVertexArrayObject(vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                state.bindVertexArrayObject(previousVao);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
                valid = glGetError() == GL_NO_ERROR && read == pattern;
            }
            ext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, static_cast<GLuint>(previousDraw));
            ext.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, static_cast<GLuint>(previousRead));
            glPopAttrib();
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        glDeleteTextures(1, &layers);
        ext.glDeleteFramebuffers(1, &frame);
        ext.glDeleteRenderbuffers(1, &color);
        ext.glDeleteVertexArrays(1, &vao);
        program->releaseGLObjects(&state);
        clearErrors();
        // Probe objects invalidate OSG state tracking.
        state.setLastAppliedProgramObject(nullptr);
        state.dirtyAllAttributes();
        return valid;
    }

    void GpuScene::probeArrays(osg::State& state)
    {
        if (mArraysProbed.load(std::memory_order_relaxed))
            return;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mMaterialMode != Shader::MaterialMode::Array)
            {
                mArraysProbed.store(true, std::memory_order_relaxed);
                return;
            }
        }
        const bool valid = linkArrayShaders(state) && sampleArrayLayer(state);
        mArraysProbed.store(true, std::memory_order_relaxed);
        if (valid)
        {
            Log(Debug::Info) << "GPU paging uses texture arrays.";
            return;
        }
        Log(Debug::Warning) << "GPU texture arrays unavailable; using material draws.";
        std::lock_guard<std::mutex> lock(mMutex);
        mMaterialMode = Shader::MaterialMode::Legacy;
        // Admitted array surfaces rebuild as legacy.
        for (const std::unique_ptr<Textures::Pool>& pool : mTextures->mPools)
            if (pool != nullptr)
                mTextures->dropMaterials(*this, pool->mId, 0);
    }

    bool GpuScene::runSkin(osg::State& state, const std::vector<GpuSkinTask>& tasks,
        const std::vector<GpuSkinMatrix>& matrices, std::uint32_t maxVertices)
    {
        if (tasks.empty())
            return true;
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

        gl.stream(gl.mSkinMatrixBuffer, matrices.data(), matrices.size() * sizeof(GpuSkinMatrix));
        gl.stream(gl.mSkinTaskBuffer, tasks.data(), tasks.size() * sizeof(GpuSkinTask));
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
                if (skin.mMesh >= mMeshes.size())
                    continue;
                const Mesh& mesh = mMeshes[skin.mMesh];
                const auto registered = mSkinMeshes.find(skin.mMesh);
                const std::uint64_t last = static_cast<std::uint64_t>(mesh.mBaseVertex) + mesh.mVertexCount;
                osg::ref_ptr<const osg::Geometry> geometry = mesh.mGeometry;
                if (mesh.mBaseVertex < 0 || last > mVertices.mExtent || geometry == nullptr)
                    continue;
                if (skin.mMorph != nullptr)
                {
                    const osg::Array* vertices = skin.mMorph->getVertexArray();
                    const auto value = std::make_pair(vertices, vertices->getModifiedCount());
                    const auto [it, inserted]
                        = mMorphVersions.try_emplace(std::make_pair(skin.mMesh, skin.mVertex), value);
                    if (inserted || it->second != value)
                    {
                        it->second = value;
                        std::vector<std::uint8_t> bytes;
                        std::vector<std::uint32_t> indices;
                        if (!packGeometry(*skin.mMorph, bytes, indices))
                            continue;
                        auto* packed = reinterpret_cast<GpuVertex*>(bytes.data());
                        for (std::size_t i = 0; i < bytes.size() / sizeof(GpuVertex); ++i)
                        {
                            packed[i].mMaterial = skin.mMaterial;
                            packed[i].mPalette = skin.mPalette;
                            packed[i].mGroup = sInvalid;
                        }
                        mGl->upload(mGl->mSkinSourceBuffer, (mesh.mBaseVertex + skin.mVertex) * sizeof(GpuVertex),
                            bytes.data(), bytes.size());
                    }
                    continue;
                }
                if (skin.mPose.mCount == 0
                    || (!skin.mBatch
                        && (registered == mSkinMeshes.end() || registered->second.mBoneCount + 1 != skin.mPose.mCount))
                    || !mUpdatedMeshes.insert(geometry.get()).second)
                    continue;
                unsigned int flags = 0;
                if (geometry->getNormalArray() != nullptr)
                    flags |= 1;
                if (dynamic_cast<const osg::Vec4Array*>(geometry->getTexCoordArray(7)) != nullptr)
                    flags |= 2;
                const GpuSkinTask task{ static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount,
                    static_cast<std::uint32_t>(matrixCount), flags | (skin.mBatch ? 4u : 0u) };
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
            // Recycled poses skip deformation.
            if (!slot.mValid || slot.mFrame != frame)
                return true;
            for (const SkinPose& pose : poses)
            {
                const std::uint64_t last = static_cast<std::uint64_t>(pose.mFirst) + pose.mCount;
                if (pose.mFrame != frame || last > slot.mMatrices.size())
                    return true;
                matrices.insert(matrices.end(), slot.mMatrices.begin() + pose.mFirst, slot.mMatrices.begin() + last);
            }
        }

        return runSkin(state, tasks, matrices, maxVertices);
    }

    bool GpuScene::applyRaySkin(osg::State& state, const RayBatch& batch)
    {
        if (batch.mSkins.empty())
            return true;
        std::vector<GpuSkinTask> tasks;
        std::uint32_t maxVertices = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const RaySkin& skin : batch.mSkins)
            {
                if (skin.mMesh >= mMeshes.size() || skin.mCount == 0
                    || static_cast<std::uint64_t>(skin.mFirst) + skin.mCount > batch.mSkinMatrices.size())
                    return false;
                const Mesh& mesh = mMeshes[skin.mMesh];
                const auto registered = mSkinMeshes.find(skin.mMesh);
                if (mesh.mGeometry != skin.mGeometry || mesh.mBaseVertex < 0 || registered == mSkinMeshes.end()
                    || registered->second.mBoneCount + 1 != skin.mCount)
                    return false;
                unsigned int flags = 0;
                if (mesh.mGeometry->getNormalArray() != nullptr)
                    flags |= 1;
                if (dynamic_cast<const osg::Vec4Array*>(mesh.mGeometry->getTexCoordArray(7)) != nullptr)
                    flags |= 2;
                tasks.push_back(
                    { static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount, skin.mFirst, flags });
                maxVertices = std::max(maxVertices, mesh.mVertexCount);
            }
        }
        return runSkin(state, tasks, batch.mSkinMatrices, maxVertices);
    }

    bool GpuScene::prepareDynamic(osg::State& state, std::vector<DynamicInstance>& dynamic,
        const std::vector<SkinUpdate>& skins, std::uint32_t receiverBase, std::vector<ReceiverSlot>& receivers)
    {
        Gl& gl = *mGl;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mUpdateFrame != frame)
            {
                mUpdateFrame = frame;
                mUpdatedMeshes.clear();
            }
        }
        if (mSkinSupported.load(std::memory_order_relaxed) && !applySkin(state, skins))
        {
            Log(Debug::Error) << "GPU skin shader failed";
            mSkinSupported.store(false, std::memory_order_relaxed);
            mRaySupported.store(false, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            gl.mDynamicRanges.assign(mBuckets.size(), { 0, 0 });
        }
        if (dynamic.empty())
            return true;
        if (dynamic.size() > sInvalid - receiverBase)
        {
            Log(Debug::Warning) << "GPU light slots exceed index limits";
            return false;
        }

        std::vector<std::array<Id, 3>> order;
        order.reserve(dynamic.size());
        for (Id i = 0; i < dynamic.size(); ++i)
            order.push_back({ dynamic[i].mBucket, dynamic[i].mMesh, i });
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
        instances.reserve(dynamic.size());

        std::size_t first = 0;
        while (first < order.size())
        {
            const DynamicInstance& head = dynamic[order[first][2]];
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
                const DynamicInstance& instance = dynamic[order[i][2]];
                GpuInstance record{};
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col)
                        record.mRows[row][col] = instance.mWorld(col, row);
                record.mMaterial[1] = receiverBase + static_cast<std::uint32_t>(instances.size());
                record.mMaterial[0] = instance.mMaterial;
                instances.push_back(record);
                if (instance.mReceiver != 0)
                    receivers.push_back({ record.mMaterial[1], instance.mReceiver });
                if (instance.mUpdateMesh)
                {
                    // Skip uploads for unchanged vertex data.
                    const osg::Array* vertices = instance.mGeometry->getVertexArray();
                    const unsigned int version = vertices != nullptr ? vertices->getModifiedCount() : 0;
                    std::lock_guard<std::mutex> versionLock(mMutex);
                    if (mUpdatedMeshes.insert(instance.mGeometry.get()).second)
                    {
                        const auto [it, inserted] = mMeshVersions.try_emplace(instance.mGeometry.get(), version);
                        if (inserted || it->second != version)
                        {
                            it->second = version;
                            meshUploads.push_back(MeshUpload{ instance.mGeometry,
                                static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount, mesh.mIndexCount });
                        }
                    }
                }
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
        if (commands.size() > gl.mStorageLimit / sizeof(GpuCommand)
            || instances.size() > gl.mStorageLimit / sizeof(GpuInstance))
        {
            Log(Debug::Error) << "GPU dynamic buffers exceed storage limit";
            return false;
        }
        std::vector<std::uint32_t> visible(instances.size());
        std::iota(visible.begin(), visible.end(), 0);

        gl.stream(gl.mDynamicInstanceBuffer, instances.data(), instances.size() * sizeof(GpuInstance));
        gl.stream(gl.mDynamicVisibleBuffer, visible.data(), visible.size() * sizeof(std::uint32_t));
        gl.stream(gl.mDynamicCommandBuffer, commands.data(), commands.size() * sizeof(GpuCommand));

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

    bool GpuScene::prepareReceivers(std::uint32_t view, const std::vector<ReceiverSlot>& receivers)
    {
        Gl& gl = *mGl;
        // Unmapped slots receive only sunlight.
        std::size_t slots = 1;
        for (const ReceiverSlot& receiver : receivers)
            slots = std::max(slots, static_cast<std::size_t>(receiver.mSlot) + 1);
        if (slots > gl.mStorageLimit / sizeof(std::uint32_t))
        {
            Log(Debug::Warning) << "GPU light slots exceed storage limits";
            return false;
        }
        std::vector<std::uint32_t> indices(slots, 0);
        for (const ReceiverSlot& receiver : receivers)
            indices[receiver.mSlot] = receiver.mReceiver;
        if (gl.mReceiverBuffers.size() <= view)
            gl.mReceiverBuffers.resize(view + 1, 0);
        gl.stream(gl.mReceiverBuffers[view], indices.data(), indices.size() * sizeof(std::uint32_t));
        return true;
    }

    GpuScene::RayHit GpuScene::decodeRay(const RayBatch& batch, std::size_t ray, const void* data, bool valid)
    {
        constexpr std::uint32_t noHit = std::numeric_limits<std::uint32_t>::max();
        constexpr float ratioScale = 4294967040.f;
        const auto* hit = static_cast<const RayBatch::GpuHit*>(data);
        RayHit result;
        result.mId = batch.mRequests[ray].mId;
        result.mValid = valid;
        if (valid && hit != nullptr && hit->mRatio != noHit)
        {
            const std::uint32_t task = hit->mTask;
            if (task >= batch.mTasks.size() || task >= batch.mActors.size() || batch.mTasks[task].mMeta[0] != ray)
                result.mValid = false;
            else
            {
                const float baryX = std::bit_cast<float>(hit->mBaryX);
                const float baryY = std::bit_cast<float>(hit->mBaryY);
                result.mHit = true;
                result.mActor = batch.mActors[task];
                result.mRatio = static_cast<float>(hit->mRatio) / ratioScale;
                result.mNormal = { std::bit_cast<float>(hit->mNormalX), std::bit_cast<float>(hit->mNormalY),
                    std::bit_cast<float>(hit->mNormalZ) };
                if (hit->mTriangle >= batch.mTasks[task].mMeta[3] || !std::isfinite(result.mRatio)
                    || !std::isfinite(baryX) || !std::isfinite(baryY) || baryX < 0.f || baryY < 0.f
                    || baryX + baryY > 1.f || !result.mNormal.valid() || result.mNormal.length2() == 0.f)
                    result.mValid = false;
                else
                    result.mNormal.normalize();
            }
        }
        return result;
    }

    void GpuScene::finishRay(
        const std::shared_ptr<RayState>& state, const RayBatch& batch, const void* data, bool valid)
    {
        const auto* hits = static_cast<const RayBatch::GpuHit*>(data);
        std::vector<RayHit> completed;
        completed.reserve(batch.mRequests.size());
        for (std::size_t ray = 0; ray < batch.mRequests.size(); ++ray)
            completed.push_back(decodeRay(batch, ray, hits != nullptr ? hits + ray : nullptr, valid));
        std::lock_guard<std::mutex> lock(state->mMutex);
        if (state->mAlive)
            state->mCompleted.insert(state->mCompleted.end(), std::make_move_iterator(completed.begin()),
                std::make_move_iterator(completed.end()));
    }

    bool GpuScene::runRay(osg::State& state, osg::GraphicsContext& context, std::unique_ptr<RayBatch>& batch)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;
        const auto disable = [&] { mRaySupported.store(false, std::memory_order_relaxed); };

        if (!gl.mLimitsChecked)
            compile(state);
        if (!mSupported.load(std::memory_order_relaxed))
        {
            disable();
            return false;
        }
        PendingWork work;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            takePendingWork(work, std::numeric_limits<std::size_t>::max());
        }
        upload(state, work);
        if (!applyRaySkin(state, *batch))
        {
            disable();
            return false;
        }

        if (gl.mRayProgram == nullptr)
        {
            gl.mRayProgram = new osg::Program;
            gl.mRayProgram->addShader(mShaderManager.getShader("gpuscene_raycast.comp"));
            gl.mRayPassUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "rayPass");
            gl.mRayTaskBaseUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "taskBase");
        }
        state.applyAttribute(gl.mRayProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mRayProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
        {
            Log(Debug::Error) << "GPU ray shader failed";
            disable();
            return false;
        }

        gl.stream(gl.mRayBuffer, batch->mRays.data(), batch->mRays.size() * sizeof(RayBatch::GpuRay));
        gl.stream(gl.mRayTaskBuffer, batch->mTasks.data(), batch->mTasks.size() * sizeof(RayBatch::GpuTask));
        RayBatch::GpuHit empty{};
        empty.mRatio = std::numeric_limits<std::uint32_t>::max();
        empty.mTask = std::numeric_limits<std::uint32_t>::max();
        empty.mTriangle = std::numeric_limits<std::uint32_t>::max();
        std::vector<RayBatch::GpuHit> initial(batch->mRequests.size(), empty);
        GLuint hitBuffer = 0;
        gl.stream(hitBuffer, initial.data(), initial.size() * sizeof(RayBatch::GpuHit), GL_STREAM_READ);

        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, gl.mVertexBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, gl.mIndexBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, gl.mRayBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, gl.mRayTaskBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, hitBuffer);
        constexpr std::size_t maxTaskGroups = 65535;
        const GLuint triangleGroups = (batch->mMaxTriangles + sSkinGroupSize - 1) / sSkinGroupSize;
        for (unsigned int pass = 0; pass < 2; ++pass)
        {
            gl.mRayPassUniform->set(pass);
            pcp->apply(*gl.mRayPassUniform);
            for (std::size_t base = 0; base < batch->mTasks.size(); base += maxTaskGroups)
            {
                const GLuint count = static_cast<GLuint>(std::min(maxTaskGroups, batch->mTasks.size() - base));
                gl.mRayTaskBaseUniform->set(static_cast<unsigned int>(base));
                pcp->apply(*gl.mRayTaskBaseUniform);
                ext.glDispatchCompute(triangleGroups, count, 1);
            }
            ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        GLsync fence = ext.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence == nullptr)
        {
            ext.glDeleteBuffers(1, &hitBuffer);
            disable();
            return false;
        }

        context.add(new RayReadOperation(mRayState, std::move(batch), ext, hitBuffer, fence));
        return true;
    }

    GpuScene::GpuView GpuScene::makeView(const ViewRecord& record)
    {
        GpuView view{};
        for (std::size_t i = 0; i < record.mPlanes.size(); ++i)
            view.mPlanes[i] = record.mPlanes[i];
        view.mPixelSizeVector = record.mPixelSizeVector;
        view.mViewPoint = osg::Vec4f(record.mViewPoint, record.mLodScale);
        view.mParams.x() = record.mSmallFeaturePixelSize;
        view.mMeta[0] = static_cast<std::uint32_t>(record.mPlanes.size());
        view.mMeta[1] = record.mOcclusion ? 1u : 0u;
        for (int column = 0; column < 4; ++column)
        {
            view.mViewProj[column] = osg::Vec4f(record.mViewProj(column, 0), record.mViewProj(column, 1),
                record.mViewProj(column, 2), record.mViewProj(column, 3));
        }
        return view;
    }

    bool GpuScene::buildPyramid(osg::State& state, const osg::Texture& source)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;
        const bool multisample = source.getTextureTarget() == GL_TEXTURE_2D_MULTISAMPLE;
        if (source.getTextureTarget() != GL_TEXTURE_2D && !multisample)
            return false;
        osg::Texture::TextureObject* object = source.getTextureObject(state.getContextID());
        const int sourceWidth = source.getTextureWidth();
        const int sourceHeight = source.getTextureHeight();
        const int width = std::max(sourceWidth / 2, 1);
        const int height = std::max(sourceHeight / 2, 1);
        if (object == nullptr || sourceWidth <= 1 || sourceHeight <= 1 || ext.glTexStorage2D == nullptr
            || ext.glBindImageTexture == nullptr || ext.glMemoryBarrier == nullptr)
            return false;
        if (gl.mDownsampleProgram == nullptr)
        {
            gl.mDownsampleProgram = new osg::Program;
            gl.mDownsampleProgram->addShader(
                mShaderManager.getShader("gpuscene_downsample.comp", { { "multisample", multisample ? "1" : "0" } }));
            gl.mFirstPass = new osg::Uniform("firstPass", 0);
            gl.mSourceDepthUniform = new osg::Uniform("sourceDepth", 0);
        }
        if (gl.mPyramid == 0 || gl.mPyramidWidth != width || gl.mPyramidHeight != height)
        {
            if (gl.mPyramid != 0)
                glDeleteTextures(1, &gl.mPyramid);
            glGenTextures(1, &gl.mPyramid);
            gl.mPyramidWidth = width;
            gl.mPyramidHeight = height;
            gl.mPyramidLevels = std::min(8, 1 + static_cast<int>(std::floor(std::log2(std::max(width, height)))));
            state.setActiveTextureUnit(30);
            glBindTexture(GL_TEXTURE_2D, gl.mPyramid);
            ext.glTexStorage2D(GL_TEXTURE_2D, gl.mPyramidLevels, GL_R32F, width, height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            state.setActiveTextureUnit(0);
        }
        ext.glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        state.applyAttribute(gl.mDownsampleProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mDownsampleProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        state.setActiveTextureUnit(29);
        source.apply(state);
        if (gl.mSourceSamplesUniform == nullptr)
        {
            GLint samples = 1;
            if (multisample)
                glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_SAMPLES, &samples);
            gl.mSourceSamplesUniform = new osg::Uniform("sourceSamples", samples);
        }
        state.setActiveTextureUnit(0);
        pcp->apply(*gl.mSourceSamplesUniform);
        gl.mSourceDepthUniform->set(29);
        int levelWidth = gl.mPyramidWidth;
        int levelHeight = gl.mPyramidHeight;
        for (int level = 0; level < gl.mPyramidLevels; ++level)
        {
            gl.mFirstPass->set(level == 0 ? 1 : 0);
            pcp->apply(*gl.mFirstPass);
            pcp->apply(*gl.mSourceDepthUniform);
            if (level > 0)
                ext.glBindImageTexture(0, gl.mPyramid, level - 1, GL_FALSE, 0, GL_READ_ONLY_ARB, GL_R32F);
            ext.glBindImageTexture(1, gl.mPyramid, level, GL_FALSE, 0, GL_WRITE_ONLY_ARB, GL_R32F);
            ext.glDispatchCompute((levelWidth + 15) / 16, (levelHeight + 15) / 16, 1);
            ext.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            levelWidth = std::max(levelWidth / 2, 1);
            levelHeight = std::max(levelHeight / 2, 1);
        }
        ext.glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        return true;
    }

    bool GpuScene::dispatch(osg::State& state, const std::vector<GpuView>& views, std::uint32_t viewBase,
        std::uint32_t instanceExtent, const std::vector<std::uint32_t>& chunkData, CullPass pass, bool occlusionReady)
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
            gl.mViewBase = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "viewBase");
            gl.mViewCount = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "viewCount");
            gl.mSlotCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "slotCount");
            gl.mBatchCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "batchCount");
            gl.mCommandCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "commandCount");
            gl.mOccluderPixelUniform = new osg::Uniform("occluderPixelSize", sOccluderPixelSize);
            gl.mCullPassUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "cullPass");
            gl.mOcclusionReadyUniform = new osg::Uniform("occlusionReady", 0);
            gl.mPyramidLevelsUniform = new osg::Uniform("pyramidLevels", 0);
        }

        if (gl.mVertexBuffer == 0 || gl.mBatchCount == 0)
            return true;
        if (pass != CullPass::Resolve)
        {
            if (!gl.resizeViews(viewBase + static_cast<std::uint32_t>(views.size())))
                return false;
            gl.createVao(state);
            gl.stream(gl.mChunkBuffer, chunkData.data(), chunkData.size() * sizeof(std::uint32_t));
            gl.stream(gl.mViewBuffer, views.data(), views.size() * sizeof(GpuView));
            gl.zero(gl.mCountBuffer);
        }

        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, gl.mBatchBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, gl.mChunkBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, gl.mCountBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, gl.mVisibleBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, gl.mCommandBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, gl.mViewBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, gl.mOcclusionBuffer);

        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        state.applyAttribute(gl.mCullProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mCullProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        gl.mInstanceCount->set(instanceExtent);
        gl.mChunkCount->set(static_cast<unsigned int>(chunkData.size() / 2));
        gl.mViewBase->set(viewBase);
        gl.mViewCount->set(static_cast<unsigned int>(views.size()));
        gl.mSlotCountUniform->set(gl.mSlotCount);
        gl.mBatchCountUniform->set(gl.mBatchCount);
        gl.mCommandCountUniform->set(gl.mCommandCount);
        gl.mCullPassUniform->set(static_cast<unsigned int>(pass));
        gl.mOcclusionReadyUniform->set(occlusionReady ? 1 : 0);
        for (const osg::Uniform* uniform : { gl.mInstanceCount.get(), gl.mChunkCount.get(), gl.mViewBase.get(),
                 gl.mViewCount.get(), gl.mSlotCountUniform.get(), gl.mBatchCountUniform.get(),
                 gl.mOccluderPixelUniform.get(), gl.mCullPassUniform.get(), gl.mOcclusionReadyUniform.get() })
            pcp->apply(*uniform);
        if (!state.setActiveTextureUnit(30))
            return false;
        glBindTexture(GL_TEXTURE_2D, gl.mPyramid);
        state.setActiveTextureUnit(0);
        state.haveAppliedTextureAttribute(30, osg::StateAttribute::TEXTURE);
        if (occlusionReady)
        {
            gl.mPyramidLevelsUniform->set(gl.mPyramidLevels);
            pcp->apply(*gl.mPyramidLevelsUniform);
        }
        if (instanceExtent != 0)
            ext.glDispatchCompute((instanceExtent + sWorkGroupSize - 1) / sWorkGroupSize, 1, 1);
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        state.applyAttribute(gl.mCommandProgram.get());
        pcp = gl.mCommandProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        for (const osg::Uniform* uniform : { gl.mViewBase.get(), gl.mViewCount.get(), gl.mSlotCountUniform.get(),
                 gl.mBatchCountUniform.get(), gl.mCommandCountUniform.get() })
            pcp->apply(*uniform);
        ext.glDispatchCompute(
            (gl.mBatchCount + sWorkGroupSize - 1) / sWorkGroupSize, static_cast<GLuint>(views.size()), 1);
        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        return true;
    }

    void GpuScene::Gl::draw(
        osg::State& state, const char* pass, Shader::MaterialMode mode, GLuint buffer, std::size_t first, GLsizei count)
    {
        mExt->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffer);
        const bool logDraw = mDrawLogs != 0 || mTraceDraws;
        const osg::Program::PerContextProgram* pcp = state.getLastAppliedProgramObject();
        if (logDraw)
        {
            if (mDrawLogs != 0)
                --mDrawLogs;
            GLint vao = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            Log(Debug::Info) << "GPU draw: context=" << state.getContextID()
                             << " frame=" << state.getFrameStamp()->getFrameNumber() << " pass=" << pass
                             << " material=" << static_cast<int>(mode)
                             << " program=" << (pcp != nullptr ? pcp->getHandle() : 0) << " vao=" << vao
                             << " indirect=" << buffer << " offset=" << first * sizeof(GpuCommand) << " count=" << count
                             << " stride=" << sizeof(GpuCommand);
            Log(Debug::Info) << "GPU buffers: vertex=" << mVertexBuffer << '/' << mVertexCapacity
                             << " index=" << mIndexBuffer << '/' << mIndexCapacity << " instance=" << mInstanceBuffer
                             << '/' << mInstanceCapacity << " dynamicInstance=" << mDynamicInstanceBuffer
                             << " material=" << mMaterialBuffer << '/' << mMaterialCapacity
                             << " commands=" << mCommandBuffer << '/' << mCommandBytes;
            if (pcp != nullptr)
            {
                const osg::Program* program = pcp->getProgram();
                for (unsigned int i = 0; i < program->getNumShaders(); ++i)
                    Log(Debug::Info) << "GPU program " << pcp->getHandle()
                                     << " shader=" << program->getShader(i)->getName();
            }
        }
        mExt->glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(first * sizeof(GpuCommand)), count, sizeof(GpuCommand));
        if (mTraceDraws)
            glFinish();
        if (logDraw)
        {
            if (pcp != nullptr)
            {
                std::string log;
                pcp->getInfoLog(log);
                if (!log.empty())
                    Log(Debug::Warning) << "GPU program " << pcp->getHandle() << " driver log: " << log;
            }
            if (mGetResetStatus != nullptr)
            {
                const GLenum status = mGetResetStatus();
                if (status != GL_NO_ERROR)
                    Log(Debug::Error) << "GPU reset status=" << status;
            }
            if (mTraceDraws)
                Log(Debug::Info) << "GPU draw finished: program=" << (pcp != nullptr ? pcp->getHandle() : 0);
        }
    }

    void GpuScene::draw(osg::RenderInfo& renderInfo, Id dispatcherId)
    {
        osg::State& state = *renderInfo.getState();
        if (state.getFrameStamp() == nullptr || !isEnabled())
            return;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        const osg::Camera* camera = renderInfo.getCurrentCamera();
        const bool prepass = dispatcherId == sPrepass;
        Gl& gl = *mGl;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTextures->resolve(state);
        }
        gl.advance(*this, state);
        mTextureHandles.store(gl.mHandles.size(), std::memory_order_relaxed);

        std::vector<GpuView> views;
        std::uint32_t viewBase = 0;
        std::uint32_t viewIndex = 0;
        std::vector<std::uint32_t> chunkData;
        PendingWork work;
        std::uint32_t instanceExtent = 0;
        std::uint32_t receiverBase = 0;
        bool needPrepare = false;
        std::vector<DynamicInstance> dynamic;
        std::vector<SkinUpdate> skins;
        std::vector<ReceiverSlot> receivers;
        const bool classicLighting = mLightManager != nullptr && mLightManager->getClassicLighting();
        std::unique_ptr<RayBatch> rayBatch;
        std::vector<RayRequest> invalidated;
        struct DrawRange
        {
            Id mBucket;
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<osg::StateSet> mPrepassState;
            osg::ref_ptr<const osg::Program> mProgram;
            Shader::MaterialMode mMode;
            std::uint32_t mOffset;
            std::uint32_t mCount;
            bool mDelta;
        };
        std::vector<DrawRange> ranges;
        std::vector<osg::StateAttribute::GLMode> deltaModes;
        std::vector<osg::StateAttribute::TypeMemberPair> deltaAttributes;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto found = mViews.find(ViewKey(frame, camera));
            if (prepass && camera != mPrepassCamera)
                return;
            if (found == mViews.end() || !found->second.mHasParams || (!prepass && dispatcherId >= mDispatchers.size()))
                return;
            ViewRecord& record = found->second;
            if (prepass && !mPendingRays.empty())
            {
                std::vector<RayRequest> requests;
                requests.swap(mPendingRays);
                if (mRayInvalidate)
                {
                    mRayInvalidate = false;
                    invalidated = std::move(requests);
                }
                else
                    rayBatch = makeRayBatchLocked(std::move(requests));
            }
            if (!record.mDispatched)
            {
                // Cull compatible frame views together.
                if (mDispatchFrame != frame)
                {
                    mDispatchFrame = frame;
                    mDispatchedViews = 0;
                }
                viewBase = mDispatchedViews;
                chunkData.assign(mChunks.size() * 2, 0);
                const auto add = [&](ViewRecord& pending) {
                    pending.mDispatched = true;
                    pending.mViewIndex = viewBase + static_cast<std::uint32_t>(views.size());
                    const std::uint32_t bit = 1u << views.size();
                    for (const auto& [slot, generation] : pending.mChunks)
                    {
                        if (slot >= mChunks.size() || mChunks[slot].mGeneration != generation)
                            continue;
                        const std::size_t offset = static_cast<std::size_t>(slot) * 2;
                        chunkData[offset] = generation;
                        chunkData[offset + 1] |= bit;
                    }
                    views.push_back(makeView(pending));
                };
                add(record);
                for (auto& [key, pending] : mViews)
                    if (key.first == frame && !pending.mDispatched && pending.mHasParams
                        && pending.mOcclusion == record.mOcclusion && views.size() < sMaxGroupViews)
                        add(pending);
                mDispatchedViews += static_cast<std::uint32_t>(views.size());
                takePendingWork(work, std::numeric_limits<std::size_t>::max());
                instanceExtent = mInstances.mExtent;
            }
            viewIndex = record.mViewIndex;
            if (!record.mPrepared)
            {
                record.mPrepared = true;
                needPrepare = true;
                dynamic = std::move(record.mDynamic);
                skins = std::move(record.mSkins);
                receivers = std::move(record.mReceivers);
                receiverBase = std::max(mReceiverSlots.mExtent, 1u);
            }
            if (prepass)
            {
                for (Id bucketId = 0; bucketId < mBuckets.size(); ++bucketId)
                {
                    const Bucket& bucket = mBuckets[bucketId];
                    const osg::StateSet& bucketState = *bucket.mStateSet;
                    if (!prepassBucket(bucketState) || bucket.mCommandCount == 0)
                        continue;
                    osg::ref_ptr<osg::StateSet>& prepassState = gl.mPrepassStates[bucket.mStateSet.get()];
                    if (prepassState == nullptr)
                    {
                        osg::ref_ptr<osg::StateSet> created = makePrepassState(bucketState);
                        const auto same
                            = std::find_if(gl.mPrepassStates.begin(), gl.mPrepassStates.end(), [&](const auto& item) {
                                  return item.second != nullptr && item.second->compare(*created, true) == 0;
                              });
                        prepassState = same != gl.mPrepassStates.end() ? same->second : created;
                    }
                    if (!ranges.empty() && ranges.back().mPrepassState == prepassState
                        && ranges.back().mMode == bucket.mMode
                        && ranges.back().mOffset + ranges.back().mCount == bucket.mCommandOffset)
                        ranges.back().mCount += bucket.mCommandCount;
                    else
                        ranges.push_back(DrawRange{ bucketId, bucket.mStateSet, prepassState, bucket.mProgram,
                            bucket.mMode, bucket.mCommandOffset, bucket.mCommandCount, bucket.mDelta });
                }
            }
            else
                for (const Id bucketId : mDispatchers[dispatcherId].mBuckets)
                {
                    const Bucket& bucket = mBuckets[bucketId];
                    ranges.push_back(DrawRange{ bucketId, bucket.mStateSet, nullptr, bucket.mProgram, bucket.mMode,
                        bucket.mCommandOffset, bucket.mCommandCount, bucket.mDelta });
                }
            deltaModes = mDeltaModes;
            deltaAttributes = mDeltaAttributes;
        }

        if (!views.empty())
            upload(state, work);
        if (!invalidated.empty())
        {
            RayBatch failed;
            failed.mRequests = std::move(invalidated);
            finishRay(mRayState, failed, nullptr, false);
        }
        if (rayBatch != nullptr)
        {
            bool valid = rayBatch->mValid && mRaySupported.load(std::memory_order_relaxed)
                && mSkinSupported.load(std::memory_order_relaxed);
            if (valid && rayBatch->mTasks.empty())
                finishRay(mRayState, *rayBatch, nullptr, true);
            else if (!valid || state.getGraphicsContext() == nullptr
                || !runRay(state, *state.getGraphicsContext(), rayBatch))
                finishRay(mRayState, *rayBatch, nullptr, false);
        }
        if (needPrepare)
        {
            if (!prepareDynamic(state, dynamic, skins, receiverBase, receivers)
                || (classicLighting && !prepareReceivers(viewIndex, receivers)))
            {
                mSupported = false;
                return;
            }
        }
        CullPass pass = CullPass::Full;
        osg::ref_ptr<const osg::Texture> depthSource;
        const unsigned int frameId = frame % 2;
        if (prepass && mOcclusionEnabled && !views.empty() && mPostProcessor != nullptr)
        {
            depthSource = mPostProcessor->getPrimaryFbo(frameId)
                              ->getAttachment(osg::Camera::PACKED_DEPTH_STENCIL_BUFFER)
                              .getTexture();
            if (depthSource != nullptr
                && (depthSource->getTextureTarget() == GL_TEXTURE_2D
                    || depthSource->getTextureTarget() == GL_TEXTURE_2D_MULTISAMPLE))
            {
                pass = CullPass::Seed;
            }
            else
                depthSource = nullptr;
        }
        if (!views.empty() && !dispatch(state, views, viewBase, instanceExtent, chunkData, pass, false))
        {
            Log(Debug::Error) << "GPU scene dispatch failed";
            mSupported = false;
        }
        if (!mSupported)
            return;
        if (prepass && pass != CullPass::Seed)
            return;
        const std::size_t commandBase = static_cast<std::size_t>(viewIndex) * gl.mCommandCount;

        osg::GLExtensions& ext = *gl.mExt;
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, gl.mMaterialBuffer);
        if (prepass)
        {
            const auto resolve = [&](bool ready) {
                if (!dispatch(state, views, viewBase, instanceExtent, chunkData, CullPass::Resolve, ready))
                {
                    Log(Debug::Error) << "GPU scene dispatch failed";
                    mSupported = false;
                }
            };
            const osg::ref_ptr<osg::Program> base = mShaderManager.getProgram("gpuscene_prepass", {}, nullptr, {},
                Shader::ProgramCapability::Instanced | Shader::ProgramCapability::ArrayMaterial);
            if (base == nullptr)
            {
                resolve(false);
                return;
            }
            for (const DrawRange& range : ranges)
            {
                osg::ref_ptr<osg::Program>& program = gl.mPrepassPrograms[range.mMode];
                if (program == nullptr)
                    program = mShaderManager.getInstancedProgram(*base, range.mMode);
                if (program == nullptr)
                {
                    resolve(false);
                    return;
                }
                osg::ref_ptr<osg::StateSet>& programState = gl.mPrepassStateSets[range.mMode];
                if (programState == nullptr)
                {
                    programState = new osg::StateSet;
                    programState->setAttributeAndModes(program,
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                    programState->setAttribute(
                        mPrepassDrawable->getStateSet()->getAttribute(osg::StateAttribute::POLYGONMODE),
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                }
            }
            const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            for (const DrawRange& range : ranges)
            {
                state.pushStateSet(gl.mPrepassStateSets[range.mMode]);
                state.pushStateSet(range.mPrepassState);
                state.apply();
                if (range.mCount != 0 && gl.mVao != 0)
                {
                    ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
                    state.bindVertexArrayObject(gl.mVao);
                    gl.draw(
                        state, "prepass", range.mMode, gl.mCommandBuffer, commandBase + range.mOffset, range.mCount);
                }
                state.popStateSet();
                state.popStateSet();
            }
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
            state.bindVertexArrayObject(previousVao);
            ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            state.apply();
            const bool ready = depthSource != nullptr && buildPyramid(state, *depthSource);
            resolve(ready);
            return;
        }
        if (classicLighting)
        {
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, gl.mReceiverBuffers.at(viewIndex));
        }
        // Stack values applied before the first range.
        std::vector<bool> baseModes(deltaModes.size());
        std::vector<bool> lockedModes(deltaModes.size());
        std::vector<const osg::StateAttribute*> baseAttributes(deltaAttributes.size());
        std::vector<bool> lockedAttributes(deltaAttributes.size());
        for (std::size_t i = 0; i < deltaModes.size(); ++i)
        {
            baseModes[i] = state.getLastAppliedMode(deltaModes[i]);
            const auto found = state.getModeMap().find(deltaModes[i]);
            lockedModes[i] = found != state.getModeMap().end() && !found->second.valueVec.empty()
                && (found->second.valueVec.back() & osg::StateAttribute::OVERRIDE) != 0;
        }
        for (std::size_t i = 0; i < deltaAttributes.size(); ++i)
        {
            baseAttributes[i] = state.getLastAppliedAttribute(deltaAttributes[i].first, deltaAttributes[i].second);
            const auto found = state.getAttributeMap().find(deltaAttributes[i]);
            lockedAttributes[i] = found != state.getAttributeMap().end() && !found->second.attributeVec.empty()
                && (found->second.attributeVec.back().second & osg::StateAttribute::OVERRIDE) != 0;
        }
        const auto castingBase = [&](const osg::StateSet& bucket, bool& useAlpha, bool& alphaBlend) {
            const SceneUtil::ShadowsBin::CastingState casting = SceneUtil::ShadowsBin::castingState(bucket);
            alphaBlend = casting.mAlphaBlend;
            useAlpha = casting.mUseAlpha;
            // Alpha shadows need a bound diffuse map.
            if (useAlpha && bucket.getTextureAttribute(0, osg::StateAttribute::TEXTURE) == nullptr)
                useAlpha = false;
            return mCastingProgramList[casting.mFunction - GL_NEVER].get();
        };
        bool first = true;
        bool override = false;
        bool shadowPass = false;
        const osg::Program* currentBase = nullptr;
        Shader::MaterialMode currentMode = Shader::MaterialMode::Legacy;
        bool currentUseAlpha = false;
        bool currentAlphaBlend = false;
        osg::ref_ptr<const osg::Program> overrideProgram;
        for (const DrawRange& range : ranges)
        {
            const std::pair<std::uint32_t, std::uint32_t> dynamicRange = range.mBucket < gl.mDynamicRanges.size()
                ? gl.mDynamicRanges[range.mBucket]
                : std::pair<std::uint32_t, std::uint32_t>();
            if (range.mCount == 0 && dynamicRange.second == 0)
                continue;

            bool canDraw = true;
            osg::ref_ptr<osg::StateSet> casting;
            bool delta = range.mDelta && !first;
            if (delta && shadowPass)
            {
                bool useAlpha = false;
                bool alphaBlend = false;
                const osg::Program* base = castingBase(*range.mStateSet, useAlpha, alphaBlend);
                delta = base == currentBase && range.mMode == currentMode && useAlpha == currentUseAlpha
                    && alphaBlend == currentAlphaBlend;
            }
            if (delta)
            {
                const osg::StateSet& stateSet = *range.mStateSet;
                for (std::size_t i = 0; i < deltaModes.size(); ++i)
                {
                    const auto found = stateSet.getModeList().find(deltaModes[i]);
                    state.applyMode(deltaModes[i],
                        found != stateSet.getModeList().end() && !lockedModes[i]
                            ? (found->second & osg::StateAttribute::ON) != 0
                            : baseModes[i]);
                }
                for (std::size_t i = 0; i < deltaAttributes.size(); ++i)
                {
                    const auto found = stateSet.getAttributeList().find(deltaAttributes[i]);
                    const osg::StateAttribute* attribute
                        = found != stateSet.getAttributeList().end() && !lockedAttributes[i] ? found->second.first.get()
                                                                                             : baseAttributes[i];
                    if (attribute == nullptr)
                        attribute
                            = state.getGlobalDefaultAttribute(deltaAttributes[i].first, deltaAttributes[i].second);
                    if (attribute != nullptr)
                        state.applyAttribute(attribute);
                }
                const osg::Program* program = override ? overrideProgram.get() : range.mProgram.get();
                canDraw = program != nullptr;
                if (canDraw && state.applyAttribute(program))
                {
                    const osg::Program::PerContextProgram* pcp = state.getLastAppliedProgramObject();
                    canDraw = pcp != nullptr;
                    for (const auto& [name, stack] : state.getUniformMap())
                        if (canDraw && !stack.uniformVec.empty())
                            pcp->apply(*stack.uniformVec.back().first);
                }
            }
            else
            {
                state.pushStateSet(range.mStateSet);
                state.apply();
                const osg::Program::PerContextProgram* appliedPcp = state.getLastAppliedProgramObject();
                const osg::Program* applied = appliedPcp != nullptr ? appliedPcp->getProgram() : nullptr;
                canDraw = applied != nullptr;
                override = applied != nullptr && applied != range.mProgram;
                overrideProgram = nullptr;
                if (override)
                {
                    shadowPass = std::find(mCastingProgramList.begin(), mCastingProgramList.end(), applied)
                        != mCastingProgramList.end();
                    bool useAlpha = false;
                    bool alphaBlend = false;
                    const osg::Program* base
                        = shadowPass ? castingBase(*range.mStateSet, useAlpha, alphaBlend) : applied;
                    currentBase = base;
                    currentMode = range.mMode;
                    currentUseAlpha = useAlpha;
                    currentAlphaBlend = alphaBlend;
                    const auto cacheKey = std::make_pair(base, range.mMode);
                    const auto cached = gl.mCastingPrograms.find(cacheKey);
                    osg::ref_ptr<osg::Program> instanced = cached != gl.mCastingPrograms.end()
                        ? cached->second
                        : gl.mCastingPrograms.emplace(cacheKey, mShaderManager.getInstancedProgram(*base, range.mMode))
                              .first->second;
                    if (instanced == nullptr)
                        canDraw = false;
                    else if (instanced != applied)
                    {
                        osg::ref_ptr<osg::StateSet>& cachedCasting
                            = gl.mCastingStateSets[{ base, alphaBlend, range.mMode }];
                        if (cachedCasting == nullptr)
                        {
                            cachedCasting = new osg::StateSet;
                            cachedCasting->setAttributeAndModes(instanced,
                                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
                                    | osg::StateAttribute::PROTECTED);
                            if (shadowPass)
                            {
                                cachedCasting->addUniform(new osg::Uniform("useDiffuseMapForShadowAlpha", useAlpha),
                                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                                cachedCasting->addUniform(new osg::Uniform("alphaTestShadows", alphaBlend),
                                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                            }
                        }
                        casting = cachedCasting;
                        state.pushStateSet(casting);
                        state.apply();
                    }
                    if (canDraw)
                        overrideProgram = instanced;
                }
                first = false;
            }

            const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            if (canDraw && range.mCount != 0 && gl.mVao != 0)
            {
                ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
                state.bindVertexArrayObject(gl.mVao);
                gl.draw(state, shadowPass ? "shadow" : "main", range.mMode, gl.mCommandBuffer,
                    commandBase + range.mOffset, range.mCount);
            }
            if (canDraw && dynamicRange.second != 0 && gl.mDynamicVao != 0)
            {
                ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mDynamicInstanceBuffer);
                state.bindVertexArrayObject(gl.mDynamicVao);
                gl.draw(state, shadowPass ? "shadow-dynamic" : "main-dynamic", range.mMode, gl.mDynamicCommandBuffer,
                    dynamicRange.first, dynamicRange.second);
            }
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
            state.bindVertexArrayObject(previousVao);

            if (casting != nullptr)
                state.popStateSet();
            if (!delta)
                state.popStateSet();
        }
        ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        if (!first)
            state.apply();
    }
}
