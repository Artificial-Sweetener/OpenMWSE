#ifndef OPENMW_MWRENDER_GPUSCENE_H
#define OPENMW_MWRENDER_GPUSCENE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <osg/BoundingSphere>
#include <osg/GL>
#include <osg/GLDefines>
#include <osg/Group>
#include <osg/Matrixf>
#include <osg/ref_ptr>

namespace osg
{
    class Camera;
    class Drawable;
    class Geometry;
    class Program;
    class RenderInfo;
    class State;
    class StateSet;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace Shader
{
    class ShaderManager;
}

namespace MWRender
{
    // GL headers may not declare these types
#ifdef GL_VERSION_3_2
    using GLsync = ::GLsync;
#else
    using GLsync = osg::GLsync;
#endif

#if defined(GL_VERSION_1_5) || defined(GL_ARB_vertex_buffer_object) || defined(GL_ES_VERSION_2_0)                      \
    || defined(OPENGLES_1_1_FOUND)
    using GLintptr = ::GLintptr;
    using GLsizeiptr = ::GLsizeiptr;
#else
    using GLintptr = osg::GLintptr;
    using GLsizeiptr = osg::GLsizeiptr;
#endif

    class GpuSceneNode;

    /// Stores GPU scene geometry and instances.
    class GpuScene
    {
    public:
        using Id = std::uint32_t;
        static constexpr Id sInvalid = 0xffffffffu;

        /// Describes one world-space instance and its optional LOD and small-feature culling policy.
        struct Instance
        {
            osg::Matrixf mWorld;
            osg::BoundingSphere mBounds;
            osg::Vec3f mLodCenter;
            float mLodMin = 0.f;
            float mLodMax = 0.f;
            bool mLod = false;
            bool mSmallFeatureCulling = false;
            Id mMesh = sInvalid;
            Id mBucket = sInvalid;
        };

        /// Create the GPU scene and detect whether the current graphics API exposes its required facilities.
        explicit GpuScene(Shader::ShaderManager& shaderManager);
        ~GpuScene();

        /// Return whether the graphics API supports the GPU scene implementation.
        bool isSupported() const { return mSupported; }
        /// Return whether GPU scene rendering is both supported and enabled.
        bool isEnabled() const { return mSupported && mEnabled; }
        /// Enable or disable GPU scene rendering without destroying resident resources.
        void setEnabled(bool enabled) { mEnabled = enabled; }
        /// Return the scene-graph node that dispatches and draws resident GPU batches.
        osg::Group* getNode();

        /// Return the resident identifier for a supported source geometry, or sInvalid when it cannot be packed.
        Id getMesh(const osg::Geometry& geometry);
        /// Return the resident material bucket for a compatible state path, or sInvalid when it cannot be represented.
        Id getBucket(const std::vector<const osg::StateSet*>& statePath);
        /// Create a paged chunk whose instances are uploaded and retired together.
        osg::ref_ptr<osg::Node> createChunk(std::vector<Instance>&& instances, const osg::Vec3f& origin);

        /// Capture the current view parameters for GPU culling and indirect-command construction.
        void recordView(osgUtil::CullVisitor& cv);
        /// Mark a resident chunk as visible to a camera during a frame.
        void markChunk(unsigned int frame, const osg::Camera* camera, Id slot, Id generation);
        /// Retire a resident chunk slot after its scene-graph owner is released.
        void releaseChunk(Id slot);
        /// Draw one material bucket using the commands produced for the current view.
        void draw(osg::RenderInfo& renderInfo, Id bucket);
        /// Publish queued geometry and instances and retire unused per-view state.
        void update();
        /// Return a bound covering every currently resident GPU instance.
        osg::BoundingSphere getResidentBound() const;

    private:
        /// Records one packed mesh range in shared vertex and index storage.
        struct Mesh
        {
            osg::ref_ptr<const osg::Geometry> mGeometry;
            std::uint32_t mFirstIndex;
            std::uint32_t mIndexCount;
            std::int32_t mBaseVertex;
        };

        /// Owns the render state and indirect batches shared by one material program.
        struct Bucket
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<const osg::Program> mProgram;
            osg::ref_ptr<osg::Drawable> mDrawable;
            std::vector<Id> mBatches;
            std::uint32_t mCommandOffset = 0;
            std::uint32_t mCommandCount = 0;
        };

        /// Identifies one mesh-and-material grouping in the GPU batch table.
        struct Batch
        {
            Id mBucket;
            Id mMesh;
            std::uint32_t mResident = 0;
        };

        /// Tracks the instance allocation and visibility generation for one paged chunk.
        struct Chunk
        {
            Id mGeneration = 0;
            std::uint32_t mFirstInstance = 0;
            std::uint32_t mInstanceCount = 0;
            std::vector<std::pair<Id, std::uint32_t>> mBatchCounts;
            osg::BoundingSphere mBounds;
        };

        /// Mirrors one instance record consumed by the GPU culling shader.
        struct GpuInstance
        {
            float mRows[3][4];
            float mBounds[4];
            float mLod[4];
            std::uint32_t mMeta[4];
        };

        /// Mirrors one batch record consumed by culling and command-construction shaders.
        struct GpuBatch
        {
            std::uint32_t mFirstIndex;
            std::uint32_t mIndexCount;
            std::int32_t mBaseVertex;
            std::uint32_t mFirstSlot;
            std::uint32_t mCommand;
            std::uint32_t mPad[3];
        };
        // Mirrors the cull shader batch table.
        static_assert(sizeof(GpuBatch) == 32);
        static_assert(offsetof(GpuBatch, mFirstSlot) == 12);
        static_assert(offsetof(GpuBatch, mCommand) == 16);

        /// Stages one packed mesh until the render thread can upload it.
        struct PendingMesh
        {
            std::uint32_t mVertexOffset;
            std::uint32_t mIndexOffset;
            std::vector<std::uint8_t> mVertices;
            std::vector<std::uint32_t> mIndices;
        };

        /// Stages a contiguous instance allocation until the render thread can upload it.
        struct PendingInstances
        {
            std::uint32_t mFirst;
            std::vector<GpuInstance> mRecords;
        };

        /// Stores one camera's culling parameters and visible chunk generations for a frame.
        struct ViewRecord
        {
            bool mHasParams = false;
            bool mDispatched = false;
            std::vector<osg::Vec4f> mPlanes;
            osg::Vec4f mPixelSizeVector;
            float mSmallFeaturePixelSize = 0.f;
            osg::Vec3f mViewPoint;
            float mLodScale = 1.f;
            std::vector<std::pair<Id, Id>> mChunks;
        };

        /// Owns render-thread-only OpenGL objects and resolved entry points.
        struct Gl;

        using ViewKey = std::pair<unsigned int, const osg::Camera*>;

        /// Reserve a contiguous range in the shared instance-address space.
        std::uint32_t allocateInstances(std::uint32_t count);
        /// Return a contiguous instance range to the reusable free list.
        void freeInstances(std::uint32_t first, std::uint32_t count);
        /// Rebuild the dense GPU batch table after mesh or material topology changes.
        void rebuildBatches(std::vector<GpuBatch>& table);
        /// Upload pending data and dispatch culling for one recorded view.
        void dispatch(osg::State& state, const ViewRecord& view, std::uint32_t instanceExtent,
            const std::vector<std::uint32_t>& chunkGenerations, std::vector<PendingMesh>& meshes,
            std::vector<PendingInstances>& instances, const std::vector<GpuBatch>* batches);

        Shader::ShaderManager& mShaderManager;
        bool mSupported = false;
        std::atomic<bool> mEnabled{ true };
        osg::ref_ptr<GpuSceneNode> mNode;

        mutable std::mutex mMutex;
        std::map<const osg::Geometry*, Id> mMeshIds;
        std::vector<Mesh> mMeshes;
        std::uint32_t mVertexCount = 0;
        std::uint32_t mIndexCount = 0;
        std::map<std::vector<const osg::StateSet*>, Id> mBucketIds;
        std::vector<Bucket> mBuckets;
        std::map<std::pair<Id, Id>, Id> mBatchIds;
        std::vector<Batch> mBatches;
        std::vector<Chunk> mChunks;
        std::vector<Id> mFreeChunks;
        Id mNextGeneration = 1;
        std::map<std::uint32_t, std::uint32_t> mFreeInstances;
        std::uint32_t mInstanceExtent = 0;
        std::vector<PendingMesh> mPendingMeshes;
        std::vector<PendingInstances> mPendingInstances;
        std::vector<osg::ref_ptr<osg::Drawable>> mPendingDrawables;
        bool mBatchesDirty = false;
        bool mBoundDirty = false;
        osg::BoundingSphere mResidentBound;
        std::map<ViewKey, ViewRecord> mViews;

        std::unique_ptr<Gl> mGl;
    };
}

#endif
