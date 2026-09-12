#include "gpuscene.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <osg/BufferObject>
#include <osg/Drawable>
#include <osg/GLDefines>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/PrimitiveSet>
#include <osg/State>
#include <osg/VertexArrayState>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/shader/shadermanager.hpp>

#include "renderbin.hpp"

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
    namespace
    {
        struct GpuVertex
        {
            float mPosition[3];
            float mNormal[3];
            unsigned char mColor[4];
            float mUv0[2];
            float mUv1[2];
            float mTangent[4];
            float mPad;
        };
        static_assert(sizeof(GpuVertex) == 64);

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
        constexpr unsigned int sMaxPlanes = 16;

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
            if (normals == nullptr)
                return false;

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

            const osg::Vec2Array* uv[2] = { nullptr, nullptr };
            const osg::Vec4Array* tangents = nullptr;
            for (unsigned int unit = 0; unit < geometry.getNumTexCoordArrays(); ++unit)
            {
                const osg::Array* array = geometry.getTexCoordArray(unit);
                if (array == nullptr)
                    continue;
                if (unit < 2)
                {
                    uv[unit] = perVertexArray<osg::Vec2Array>(array, count);
                    if (uv[unit] == nullptr)
                        return false;
                }
                else if (unit == 7)
                {
                    tangents = perVertexArray<osg::Vec4Array>(array, count);
                    if (tangents == nullptr)
                        return false;
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
                const osg::Vec3f& n = (*normals)[i];
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
                for (int unit = 0; unit < 2; ++unit)
                {
                    if (uv[unit] == nullptr)
                        continue;
                    const osg::Vec2f& t = (*uv[unit])[i];
                    (unit == 0 ? v.mUv0 : v.mUv1)[0] = t.x();
                    (unit == 0 ? v.mUv0 : v.mUv1)[1] = t.y();
                }
                if (tangents != nullptr)
                {
                    const osg::Vec4f& t = (*tangents)[i];
                    for (int k = 0; k < 4; ++k)
                        v.mTangent[k] = t[k];
                }
            }
            return true;
        }

        bool isTransparent(const osg::StateSet& stateSet)
        {
            if (stateSet.getMode(GL_BLEND) & osg::StateAttribute::ON)
                return true;
            if (stateSet.getRenderingHint() == osg::StateSet::TRANSPARENT_BIN)
                return true;
            return stateSet.useRenderBinDetails() && stateSet.getBinNumber() >= RenderBin_DepthSorted;
        }
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

    namespace
    {
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
                    scene->releaseChunk(mSlot);
            }

            void traverse(osg::NodeVisitor& nv) override
            {
                GpuScene* const scene = mScene.load(std::memory_order_acquire);
                if (scene == nullptr || nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
                    return;
                osgUtil::CullVisitor& cv = static_cast<osgUtil::CullVisitor&>(nv);
                scene->markChunk(nv.getFrameStamp()->getFrameNumber(), cv.getCurrentCamera(), mSlot, mGeneration);
            }

            osg::BoundingSphere computeBound() const override { return mBound; }

        private:
            std::atomic<GpuScene*> mScene{ nullptr };
            GpuScene::Id mSlot = 0;
            GpuScene::Id mGeneration = 0;
            osg::BoundingSphere mBound;
        };

        class BucketDrawable : public osg::Drawable
        {
        public:
            BucketDrawable() = default;
            BucketDrawable(const BucketDrawable& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
                : osg::Drawable(copy, copyop)
                , mScene(copy.mScene.load(std::memory_order_relaxed))
                , mBucket(copy.mBucket)
            {
            }
            META_Node(MWRender, BucketDrawable)

            BucketDrawable(GpuScene& scene, GpuScene::Id bucket)
                : mScene(&scene)
                , mBucket(bucket)
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
                    scene->draw(renderInfo, mBucket);
            }

        private:
            std::atomic<GpuScene*> mScene{ nullptr };
            GpuScene::Id mBucket = 0;
        };
    }

    struct GpuScene::Gl
    {
        osg::GLExtensions* mExt = nullptr;
        ClearBufferDataFn mClearBufferData = nullptr;
        CopyBufferSubDataFn mCopyBufferSubData = nullptr;
        GLuint mVao = 0;
        GLuint mVertexBuffer = 0;
        GLuint mIndexBuffer = 0;
        GLuint mInstanceBuffer = 0;
        GLuint mBatchBuffer = 0;
        GLuint mChunkBuffer = 0;
        GLuint mCountBuffer = 0;
        GLuint mVisibleBuffer = 0;
        GLuint mCommandBuffer = 0;
        std::size_t mVertexCapacity = 0;
        std::size_t mIndexCapacity = 0;
        std::size_t mInstanceCapacity = 0;
        std::size_t mBatchCapacity = 0;
        std::uint32_t mBatchCount = 0;
        osg::ref_ptr<osg::Program> mCullProgram;
        osg::ref_ptr<osg::Program> mCommandProgram;
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
        }

        void upload(GLuint buffer, std::size_t offset, const void* data, std::size_t size)
        {
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            mExt->glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        void createVao(osg::State& state)
        {
            if (mVao != 0)
                return;
            mExt->glGenVertexArrays(1, &mVao);
            const GLuint previous = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            state.bindVertexArrayObject(mVao);
            mExt->glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
            const GLsizei stride = sizeof(GpuVertex);
            glVertexPointer(3, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mPosition)));
            glEnableClientState(GL_VERTEX_ARRAY);
            glNormalPointer(GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mNormal)));
            glEnableClientState(GL_NORMAL_ARRAY);
            glColorPointer(4, GL_UNSIGNED_BYTE, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mColor)));
            glEnableClientState(GL_COLOR_ARRAY);
            mExt->glClientActiveTexture(GL_TEXTURE0);
            glTexCoordPointer(2, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mUv0)));
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            mExt->glClientActiveTexture(GL_TEXTURE0 + 1);
            glTexCoordPointer(2, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mUv1)));
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            mExt->glClientActiveTexture(GL_TEXTURE0 + 7);
            glTexCoordPointer(4, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mTangent)));
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            mExt->glClientActiveTexture(GL_TEXTURE0);
            mExt->glBindBuffer(GL_ARRAY_BUFFER, mVisibleBuffer);
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
            && ext.glVertexAttribIPointer != nullptr && ext.glClientActiveTexture != nullptr;
        if (!mSupported)
        {
            Log(Debug::Warning) << "GPU-driven object paging unavailable: OpenGL 4.3 features missing";
            return;
        }
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
    }

    GpuScene::~GpuScene()
    {
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

    GpuScene::Id GpuScene::getMesh(const osg::Geometry& geometry)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mMeshIds.find(&geometry);
        if (found != mMeshIds.end())
            return found->second;

        PendingMesh pending;
        Id id = sInvalid;
        if (packGeometry(geometry, pending.mVertices, pending.mIndices))
        {
            pending.mVertexOffset = mVertexCount;
            pending.mIndexOffset = mIndexCount;
            const std::uint32_t vertexCount = static_cast<std::uint32_t>(pending.mVertices.size() / sizeof(GpuVertex));
            id = static_cast<Id>(mMeshes.size());
            mMeshes.push_back(Mesh{ &geometry, mIndexCount, static_cast<std::uint32_t>(pending.mIndices.size()),
                static_cast<std::int32_t>(mVertexCount) });
            mVertexCount += vertexCount;
            mIndexCount += static_cast<std::uint32_t>(pending.mIndices.size());
            mPendingMeshes.push_back(std::move(pending));
        }
        mMeshIds.emplace(&geometry, id);
        return id;
    }

    GpuScene::Id GpuScene::getBucket(const std::vector<const osg::StateSet*>& statePath)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mBucketIds.find(statePath);
        if (found != mBucketIds.end())
            return found->second;

        Id id = sInvalid;
        osg::ref_ptr<osg::StateSet> merged = new osg::StateSet;
        for (const osg::StateSet* stateSet : statePath)
            merged->merge(*stateSet);
        const osg::Program* program
            = dynamic_cast<const osg::Program*>(merged->getAttribute(osg::StateAttribute::PROGRAM));
        if (program != nullptr && !isTransparent(*merged))
        {
            if (osg::ref_ptr<osg::Program> instanced
                = mShaderManager.getInstancedProgram(*program, Shader::MaterialMode::Legacy))
            {
                merged->setAttributeAndModes(instanced, osg::StateAttribute::ON);
                id = static_cast<Id>(mBuckets.size());
                Bucket bucket;
                bucket.mStateSet = merged;
                bucket.mProgram = instanced;
                bucket.mDrawable = new BucketDrawable(*this, id);
                bucket.mDrawable->setStateSet(merged);
                mPendingDrawables.push_back(bucket.mDrawable);
                mBuckets.push_back(std::move(bucket));
            }
        }
        mBucketIds.emplace(statePath, id);
        return id;
    }

    std::uint32_t GpuScene::allocateInstances(std::uint32_t count)
    {
        for (auto it = mFreeInstances.begin(); it != mFreeInstances.end(); ++it)
        {
            if (it->second < count)
                continue;
            const std::uint32_t first = it->first;
            const std::uint32_t remaining = it->second - count;
            mFreeInstances.erase(it);
            if (remaining > 0)
                mFreeInstances.emplace(first + count, remaining);
            return first;
        }
        const std::uint32_t first = mInstanceExtent;
        mInstanceExtent += count;
        return first;
    }

    void GpuScene::freeInstances(std::uint32_t first, std::uint32_t count)
    {
        auto inserted = mFreeInstances.emplace(first, count).first;
        auto next = std::next(inserted);
        if (next != mFreeInstances.end() && inserted->first + inserted->second == next->first)
        {
            inserted->second += next->second;
            mFreeInstances.erase(next);
        }
        if (inserted != mFreeInstances.begin())
        {
            auto prev = std::prev(inserted);
            if (prev->first + prev->second == inserted->first)
            {
                prev->second += inserted->second;
                mFreeInstances.erase(inserted);
            }
        }
    }

    osg::ref_ptr<osg::Node> GpuScene::createChunk(std::vector<Instance>&& instances, const osg::Vec3f& origin)
    {
        if (instances.empty())
            return nullptr;

        std::lock_guard<std::mutex> lock(mMutex);
        Chunk chunk;
        chunk.mGeneration = mNextGeneration++;
        if (mNextGeneration == 0)
            mNextGeneration = 1;
        chunk.mInstanceCount = static_cast<std::uint32_t>(instances.size());
        chunk.mFirstInstance = allocateInstances(chunk.mInstanceCount);

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
        osg::BoundingSphere localBound;
        for (const Instance& instance : instances)
        {
            const auto key = std::make_pair(instance.mBucket, instance.mMesh);
            auto batchIt = mBatchIds.find(key);
            if (batchIt == mBatchIds.end())
            {
                const Id batchId = static_cast<Id>(mBatches.size());
                mBatches.push_back(Batch{ instance.mBucket, instance.mMesh, 0 });
                mBuckets[instance.mBucket].mBatches.push_back(batchId);
                batchIt = mBatchIds.emplace(key, batchId).first;
            }
            const Id batch = batchIt->second;
            ++mBatches[batch].mResident;
            ++batchCounts[batch];

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
            record.mMeta[2] = chunk.mGeneration;
            std::memcpy(&record.mMeta[3], &instance.mLodMax, sizeof(float));
            pending.mRecords.push_back(record);

            chunk.mBounds.expandBy(instance.mBounds);
            localBound.expandBy(osg::BoundingSphere(instance.mBounds.center() - origin, instance.mBounds.radius()));
        }
        chunk.mBatchCounts.assign(batchCounts.begin(), batchCounts.end());
        mResidentBound.expandBy(chunk.mBounds);
        mBoundDirty = true;
        mBatchesDirty = true;
        mPendingInstances.push_back(std::move(pending));
        mChunks[slot] = std::move(chunk);
        return new GpuChunkNode(*this, slot, mChunks[slot].mGeneration, localBound);
    }

    void GpuScene::releaseChunk(Id slot)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        Chunk& chunk = mChunks[slot];
        for (const auto& [batch, count] : chunk.mBatchCounts)
            mBatches[batch].mResident -= count;
        freeInstances(chunk.mFirstInstance, chunk.mInstanceCount);
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

        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = mViews.begin(); it != mViews.end();)
        {
            if (it->first.first + 2 < frame)
                it = mViews.erase(it);
            else
                ++it;
        }
        ViewRecord& view = mViews[ViewKey(frame, cv.getCurrentCamera())];
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

    void GpuScene::markChunk(unsigned int frame, const osg::Camera* camera, Id slot, Id generation)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mViews[ViewKey(frame, camera)].mChunks.emplace_back(slot, generation);
    }

    void GpuScene::update()
    {
        // Releases drain on the next enable.
        if (!isEnabled())
            return;
        std::vector<osg::ref_ptr<osg::Drawable>> drawables;
        bool boundDirty = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            drawables.swap(mPendingDrawables);
            boundDirty = mBoundDirty;
            mBoundDirty = false;
        }
        for (const osg::ref_ptr<osg::Drawable>& drawable : drawables)
            mNode->addChild(drawable);
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

    void GpuScene::dispatch(osg::State& state, const ViewRecord& view, std::uint32_t instanceExtent,
        const std::vector<std::uint32_t>& chunkGenerations, std::vector<PendingMesh>& meshes,
        std::vector<PendingInstances>& instances, const std::vector<GpuBatch>* batches)
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

        std::size_t vertexBytes = 0;
        std::size_t indexBytes = 0;
        for (const PendingMesh& mesh : meshes)
        {
            vertexBytes = std::max(vertexBytes, mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size());
            indexBytes = std::max(indexBytes, (mesh.mIndexOffset + mesh.mIndices.size()) * sizeof(std::uint32_t));
        }
        std::size_t instanceBytes = 0;
        for (const PendingInstances& pending : instances)
            instanceBytes = std::max(instanceBytes, (pending.mFirst + pending.mRecords.size()) * sizeof(GpuInstance));
        gl.grow(gl.mVertexBuffer, gl.mVertexCapacity, vertexBytes, GL_STATIC_DRAW);
        gl.grow(gl.mIndexBuffer, gl.mIndexCapacity, indexBytes, GL_STATIC_DRAW);
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
        for (const PendingMesh& mesh : meshes)
        {
            gl.upload(
                gl.mVertexBuffer, mesh.mVertexOffset * sizeof(GpuVertex), mesh.mVertices.data(), mesh.mVertices.size());
            gl.upload(gl.mIndexBuffer, mesh.mIndexOffset * sizeof(std::uint32_t), mesh.mIndices.data(),
                mesh.mIndices.size() * sizeof(std::uint32_t));
        }
        for (const PendingInstances& pending : instances)
            gl.upload(gl.mInstanceBuffer, pending.mFirst * sizeof(GpuInstance), pending.mRecords.data(),
                pending.mRecords.size() * sizeof(GpuInstance));
        if (batches != nullptr)
        {
            const std::size_t batchBytes = std::max<std::size_t>(batches->size(), 1) * sizeof(GpuBatch);
            gl.grow(gl.mBatchBuffer, gl.mBatchCapacity, batchBytes, GL_DYNAMIC_COPY);
            if (!batches->empty())
                gl.upload(gl.mBatchBuffer, 0, batches->data(), batches->size() * sizeof(GpuBatch));
            gl.mBatchCount = static_cast<std::uint32_t>(batches->size());
            if (gl.mCommandBuffer != 0)
                ext.glDeleteBuffers(1, &gl.mCommandBuffer);
            ext.glGenBuffers(1, &gl.mCommandBuffer);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, gl.mCommandBuffer);
            ext.glBufferData(GL_COPY_WRITE_BUFFER, std::max<std::size_t>(batches->size(), 1) * sizeof(GpuCommand),
                nullptr, GL_DYNAMIC_COPY);
            ext.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            if (gl.mCountBuffer == 0)
                ext.glGenBuffers(1, &gl.mCountBuffer);
            ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl.mCountBuffer);
            ext.glBufferData(GL_SHADER_STORAGE_BUFFER,
                std::max<std::size_t>(batches->size(), 1) * sizeof(std::uint32_t), nullptr, GL_DYNAMIC_COPY);
            ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        if (gl.mChunkBuffer == 0)
            ext.glGenBuffers(1, &gl.mChunkBuffer);
        if (gl.mVertexBuffer == 0 || gl.mCommandBuffer == 0)
            return;
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
        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        state.applyAttribute(gl.mCullProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mCullProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return;
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
            return;
        gl.mBatchCountUniform->set(gl.mBatchCount);
        pcp->apply(*gl.mBatchCountUniform);
        ext.glDispatchCompute((gl.mBatchCount + sWorkGroupSize - 1) / sWorkGroupSize, 1, 1);
        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    }

    void GpuScene::draw(osg::RenderInfo& renderInfo, Id bucketId)
    {
        osg::State& state = *renderInfo.getState();
        if (state.getFrameStamp() == nullptr || !isEnabled())
            return;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        const osg::Camera* camera = renderInfo.getCurrentCamera();

        bool needDispatch = false;
        ViewRecord view;
        std::vector<std::uint32_t> chunkGenerations;
        std::vector<PendingMesh> meshes;
        std::vector<PendingInstances> instances;
        std::vector<GpuBatch> batches;
        bool batchesRebuilt = false;
        std::uint32_t instanceExtent = 0;
        std::uint32_t commandOffset = 0;
        std::uint32_t commandCount = 0;
        const osg::Program* bucketProgram = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto found = mViews.find(ViewKey(frame, camera));
            if (found == mViews.end() || !found->second.mHasParams)
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
                meshes.swap(mPendingMeshes);
                instances.swap(mPendingInstances);
                instanceExtent = mInstanceExtent;
                if (mBatchesDirty)
                {
                    rebuildBatches(batches);
                    batchesRebuilt = true;
                }
            }
            const Bucket& bucket = mBuckets[bucketId];
            commandOffset = bucket.mCommandOffset;
            commandCount = bucket.mCommandCount;
            bucketProgram = bucket.mProgram;
        }

        const osg::Program::PerContextProgram* appliedPcp = state.getLastAppliedProgramObject();
        const osg::Program* applied = appliedPcp != nullptr ? appliedPcp->getProgram() : nullptr;

        if (needDispatch)
        {
            dispatch(
                state, view, instanceExtent, chunkGenerations, meshes, instances, batchesRebuilt ? &batches : nullptr);
            if (applied != nullptr)
                state.applyAttribute(applied);
        }

        Gl& gl = *mGl;
        if (commandCount == 0 || gl.mVao == 0 || applied == nullptr)
            return;

        osg::ref_ptr<osg::StateSet> pushed;
        if (applied != bucketProgram)
        {
            osg::ref_ptr<osg::Program> instanced
                = mShaderManager.getInstancedProgram(*applied, Shader::MaterialMode::Legacy);
            if (instanced == nullptr)
                return;
            if (instanced != applied)
            {
                osg::ref_ptr<osg::StateSet>& stateSet = gl.mCastingStateSets[applied];
                if (stateSet == nullptr)
                {
                    stateSet = new osg::StateSet;
                    stateSet->setAttributeAndModes(instanced,
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                }
                pushed = stateSet;
                state.pushStateSet(pushed);
                state.apply();
            }
        }

        osg::GLExtensions& ext = *gl.mExt;
        const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
            ? state.getCurrentVertexArrayState()->getVertexArrayObject()
            : 0;
        state.bindVertexArrayObject(gl.mVao);
        ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gl.mCommandBuffer);
        ext.glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::size_t>(commandOffset) * sizeof(GpuCommand)), commandCount,
            0);
        state.bindVertexArrayObject(previousVao);

        if (pushed != nullptr)
        {
            state.popStateSet();
            state.apply();
        }
    }
}
