#ifndef OPENMW_MWRENDER_GPUSCENE_H
#define OPENMW_MWRENDER_GPUSCENE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/GL>
#include <osg/GLDefines>
#include <osg/Group>
#include <osg/LOD>
#include <osg/Matrixf>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <components/shader/materialmap.hpp>
#include <components/shader/materialmode.hpp>
#include <components/terrain/chunkmanager.hpp>

namespace osg
{
    class Callback;
    class Camera;
    class Drawable;
    class Geometry;
    class GraphicsContext;
    struct GraphicsOperation;
    class Node;
    class NodeVisitor;
    class Program;
    class Sampler;
    class ShaderStorageBufferBinding;
    class Texture;
    class RenderInfo;
    class State;
    class StateSet;
    class Stats;
}

namespace osgUtil
{
    class CullVisitor;
    class RenderStage;
}

namespace SceneUtil
{
    class LightListCallback;
    class LightManager;
    class Material;
    class PositionAttitudeTransform;
    class RigGeometry;
    class MorphGeometry;
    class WorkQueue;
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

    class GpuCaptureCallback;
    class GpuChunkNode;
    class GpuSceneNode;
    class PostProcessor;
    struct GpuTemplate;

    /// Stores GPU scene geometry and instances.
    class GpuScene : public Terrain::GpuChunkFactory
    {
    public:
        /// Select independently governed GPU-scene policies at construction time.
        struct Options
        {
            bool mGpuSkinning = false;
            bool mPackedTextures = false;
            bool mStorageBudget = false;
            bool mLowEndIndirectBatching = false;
            bool mLightOcclusion = false;
            bool mDistantPaging = false;
        };

        using Id = std::uint32_t;
        static constexpr Id sInvalid = 0xffffffffu;
        static constexpr Id sPrepass = 0xfffffffeu;
        static constexpr std::size_t sMapCount = Shader::sMaterialMapCount;
        using RayId = std::uint64_t;
        static constexpr RayId sInvalidRay = ~RayId{ 0 };

        /// Identifies the renderer owner represented by a static GPU chunk.
        enum class ChunkKind
        {
            Object,
            Terrain,
            Groundcover,
            Actor,
        };

        struct RayHit
        {
            RayId mId = sInvalidRay;
            osg::ref_ptr<SceneUtil::PositionAttitudeTransform> mActor;
            osg::Vec3f mNormal;
            float mRatio = 0.f;
            bool mHit = false;
            bool mValid = false;
        };

        struct Instance
        {
            osg::Matrixf mWorld;
            osg::BoundingSphere mBounds;
            osg::BoundingBox mBox;
            osg::Vec3f mLodCenter;
            float mLodMin = 0.f;
            float mLodMax = 0.f;
            bool mLod = false;
            /// Resolve this LOD pair from the live OpenMWSE groundcover distances.
            bool mGroundcoverLod = false;
            bool mSmallFeatureCulling = false;
            bool mTerrainExtra = false;
            bool mOpaqueShadow = false;
            /// Paged night-day modes; zero means always.
            unsigned int mModes = 0;
            osg::ref_ptr<const osg::Geometry> mGeometry;
            Id mBucket = sInvalid;
            Id mMaterial = sInvalid;
        };

        struct Surface
        {
            Id mBucket = sInvalid;
            Id mMaterial = sInvalid;
        };

        enum class MaterialUpdate
        {
            None,
            Record,
            State
        };

        /// Construct the GPU scene with a snapshot of independently selected policies.
        GpuScene(Shader::ShaderManager& shaderManager, osg::Stats& stats, const Options& options);
        ~GpuScene() override;

        osg::ref_ptr<osg::Node> createTerrainChunk(
            Terrain::TerrainDrawable& drawable, const osg::Vec3f& worldOffset, bool lit) override;

        bool isSupported() const { return mSupported; }
        bool isEnabled() const { return mSupported && mEnabled; }

        /// True once after a draw-time support loss.
        bool takeDemotion() { return mDemoted.exchange(false, std::memory_order_relaxed); }
        void setEnabled(bool enabled);
        void setOcclusionEnabled(bool enabled) { mOcclusionEnabled = enabled; }
        osg::Group* getNode();
        void capture(osg::Node& node);
        /// Capture one actor's eligible GPU geometry while retaining explicit CPU fallbacks.
        void captureActor(osg::Node& node, SceneUtil::PositionAttitudeTransform& actor);
        /// Release one actor's GPU resources, restore its baseline nodes, and retire its diagnostics.
        void removeActor(SceneUtil::PositionAttitudeTransform& actor);
        RayId queueRay(const osg::Vec3f& from, const osg::Vec3f& to, bool ignorePlayer,
            std::span<SceneUtil::PositionAttitudeTransform* const> ignore);
        std::vector<RayHit> takeRayHits();

        Id getMesh(const osg::Geometry& geometry);
        /// Separates ordered or live passes.
        Surface getSurface(const std::vector<const osg::StateSet*>& statePath, int order = 0,
            MaterialUpdate updates = MaterialUpdate::None);
        /// Rebuilds texture samplers.
        void refreshSamplers();
        /// Extracts GPU-drawable model geometry.
        std::unique_ptr<const GpuTemplate> extract(const osg::Node& node, unsigned int mask, bool bakeBillboards);
        /// State of a record-only cull updater chain.
        osg::ref_ptr<const osg::StateSet> liveStateSet(const osg::Callback& callback);
        void bindLiveMaterial(const osg::StateSet& stateSet, Id material);
        void appendInstances(const GpuTemplate& tmpl, const osg::Matrixf& refMatrix, float refScale,
            const osg::LOD::MinMaxPair& refDistances, bool smallFeatureCulling, std::vector<Instance>& out) const;
        /// Append detailed and reduced grass geometry governed by the live groundcover LOD policy.
        void appendGroundcoverInstances(
            const GpuTemplate& tmpl, const osg::Matrixf& refMatrix, float refScale, std::vector<Instance>& out) const;
        /// Update the detailed-to-reduced transition and terminal grass distance without rebuilding instances.
        void setGroundcoverDistances(float detailedDistance, float viewDistance);
        /// False once a cached material retires.
        bool isTemplateLive(const GpuTemplate& tmpl) const;
        /// Create one GPU-resident chunk attributed to its source renderer.
        osg::ref_ptr<osg::Node> createChunk(
            std::vector<Instance>&& instances, const osg::BoundingSphere& bound, bool lit, ChunkKind kind);
        /// Rebakes a chunk's instance records in place.
        bool updateChunk(osg::Node& node, const std::vector<Instance>& instances, const osg::BoundingSphere& bound);
        /// Culls residents without scene graph nodes.
        void addResident(osg::Node& node);
        void removeResident(osg::Node& node);
        void setCastingPrograms(const std::vector<osg::ref_ptr<osg::Program>>& programs);
        void setLightManager(SceneUtil::LightManager* lightManager);
        /// Current night-day mode.
        void setNightDayMode(unsigned int mode) { mNightDayMode.store(mode, std::memory_order_relaxed); }
        /// Background threads build texture tiers.
        void setWorkQueue(SceneUtil::WorkQueue* queue);
        /// Camera for the depth prepass.
        void setPrepassCamera(const osg::Camera* camera) { mPrepassCamera = camera; }
        /// Depth texture owner.
        void setPostProcessor(PostProcessor* postProcessor) { mPostProcessor = postProcessor; }
        /// Supplies inherited geometry material state.
        void setDefaultMaterial(const SceneUtil::Material* material);

        /// Tracks pending GL uploads and links.
        unsigned int getPendingWork() const;
        /// Completes pending uploads before gameplay.
        osg::ref_ptr<osg::GraphicsOperation> createCompileOperation();
        /// One-shot array proof, null once done.
        osg::ref_ptr<osg::GraphicsOperation> createProbeOperation();
        /// Fences retired resources after each frame.
        osg::ref_ptr<osg::GraphicsOperation> createRetireOperation();
        /// Proves array materials in this context.
        void probeArrays(osg::State& state);
        void compile(osg::State& state);
        /// Publish current GPU ownership, deformation fallback, storage, and draw populations.
        void reportStats(unsigned int frame, osg::Stats& stats) const;
        /// Releases context-owned storage and invalidates renderer state tracking for baseline reuse.
        void releaseGLObjects(osg::State* state);

        void draw(osg::RenderInfo& renderInfo, Id bucket);

        /// Maps switch children to night-day modes.
        static unsigned int switchModes(unsigned int childCount, unsigned int child);

        struct RangeAllocator
        {
            std::map<std::uint32_t, std::uint32_t> mFree;
            std::uint32_t mExtent = 0;
            std::uint32_t mUsed = 0;
            /// sInvalid past the limit.
            std::uint32_t allocate(std::uint32_t count, std::uint32_t limit = 0xffffffffu);
            void free(std::uint32_t first, std::uint32_t count);
        };

    private:
        struct Mesh
        {
            /// The scene graph owns the geometry.
            osg::observer_ptr<const osg::Geometry> mGeometry;
            /// Lookup key, never dereferenced.
            const osg::Geometry* mKey = nullptr;
            std::uint32_t mFirstIndex;
            std::uint32_t mIndexCount;
            std::int32_t mBaseVertex;
            std::uint32_t mVertexCount;
            std::uint32_t mUsers = 0;
            /// Last frame a template asked for it.
            unsigned int mLast = 0;
            /// A rejected upload leaves no data.
            bool mFailed = false;
        };

        struct GpuMaterial
        {
            std::uint64_t mMaps[sMapCount + 1];
            float mDiffuse[4];
            float mAmbient[4];
            float mSpecular[4];
            float mEmission[4];
            float mEnvMapColor[4];
            float mParams[4];
            float mLumaBias[4];
            float mBumpMatrix[4];
            std::int32_t mModes[4];
            float mTexMatRows[16][4];
        };
        static_assert(sizeof(GpuMaterial) == 496);
        // Mirrors GpuMaterial in material.glsl.
        static_assert(offsetof(GpuMaterial, mDiffuse) == 96);
        static_assert(offsetof(GpuMaterial, mParams) == 176);
        static_assert(offsetof(GpuMaterial, mModes) == 224);
        static_assert(offsetof(GpuMaterial, mTexMatRows) == 240);

        struct PendingMaterial
        {
            Id mIndex;
            Shader::MaterialMode mMode = Shader::MaterialMode::Legacy;
            GpuMaterial mRecord;
            /// Scene textures before tracking.
            std::array<const osg::Texture*, sMapCount> mSources{};
            std::array<osg::ref_ptr<osg::Texture>, sMapCount> mTextures;
            std::array<osg::ref_ptr<osg::Sampler>, sMapCount> mSamplers;
            std::array<Id, sMapCount> mProxies;
            std::array<Id, sMapCount> mArrays;
            std::array<std::uint32_t, sMapCount> mLayers;
        };

        struct PendingRetire
        {
            PendingMaterial mMaterial;
            bool mFinal = false;
            std::size_t mBytes = 0;
            Id mProxy = sInvalid;
        };

        struct MaterialSource;

        struct Bucket
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<const osg::Program> mProgram;
            Shader::MaterialMode mMode = Shader::MaterialMode::Legacy;
            /// Only modes, attributes and the program differ.
            bool mDelta = false;
            /// A controller-driven surface owns this state.
            bool mLive = false;
            bool mTerrainExtraOnly = false;
            std::vector<Id> mBatches;
            std::uint32_t mMaterials = 0;
            std::uint32_t mCommandOffset = 0;
            std::uint32_t mCommandCount = 0;
        };

        /// Bucket state change the graphics thread applies.
        struct BucketEdit
        {
            Id mBucket = sInvalid;
            unsigned int mUnit = 0;
            /// Null leaves the current texture unchanged unless the sampler is also null.
            osg::ref_ptr<osg::Texture> mTexture;
            osg::ref_ptr<osg::Sampler> mSampler;
        };

        struct BinKey
        {
            int mHint = 0;
            int mNumber = 0;
            int mMode = 0;
            std::string mName;
            bool mNested = true;

            bool operator==(const BinKey& right) const
            {
                return mHint == right.mHint && mNumber == right.mNumber && mMode == right.mMode && mName == right.mName
                    && mNested == right.mNested;
            }
        };

        struct Dispatcher
        {
            BinKey mKey;
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<osg::Drawable> mDrawable;
            std::vector<Id> mBuckets;
            bool mOrdered = false;
        };

        struct Batch
        {
            Id mBucket;
            Id mMesh;
            std::uint32_t mResident = 0;
            Id mCommand = sInvalid;
        };

        struct Chunk
        {
            Id mGeneration = 0;
            std::uint32_t mFirstInstance = 0;
            std::uint32_t mInstanceCount = 0;
            std::vector<std::pair<Id, std::uint32_t>> mBatchCounts;
            std::vector<Id> mMeshes;
            std::vector<Id> mMaterials;
            osg::BoundingSphere mBounds;
            std::uint32_t mReceiverSlot = 0;
            ChunkKind mKind = ChunkKind::Object;
            /// Resident detailed grass records owned by this chunk.
            std::uint64_t mGroundcoverDetailedInstances = 0;
            /// Resident reduced grass records owned by this chunk.
            std::uint64_t mGroundcoverReducedInstances = 0;
            /// Detailed grass triangles represented by this chunk's resident records.
            std::uint64_t mGroundcoverDetailedTriangles = 0;
            /// Reduced grass triangles represented by this chunk's resident records.
            std::uint64_t mGroundcoverReducedTriangles = 0;
        };

        struct GpuInstance
        {
            float mRows[3][4];
            float mBounds[4];
            float mLod[4];
            std::uint32_t mMeta[4];
            std::uint32_t mMaterial[4];
        };
        // Mirrors Instance in gpuscene_types.glsl.
        static_assert(sizeof(GpuInstance) == 112);
        static_assert(offsetof(GpuInstance, mBounds) == 48);
        static_assert(offsetof(GpuInstance, mMeta) == 80);
        static_assert(offsetof(GpuInstance, mMaterial) == 96);

        static GpuInstance makeRecord(const Instance& instance, Id batch, Id slot, Id generation, Id lightSlot);

        struct GpuOcclusion
        {
            float mMin[3];
            std::uint32_t mPad0;
            float mMax[3];
            std::uint32_t mPad1;
        };
        static_assert(sizeof(GpuOcclusion) == 32);
        // Mirrors Occlusion in gpuscene_cull.comp.
        static_assert(offsetof(GpuOcclusion, mMax) == 16);

        static GpuOcclusion makeOcclusion(const Instance& instance);

        struct GpuBatch
        {
            std::uint32_t mFirstIndex;
            std::uint32_t mIndexCount;
            std::int32_t mBaseVertex;
            std::uint32_t mFirstSlot;
            std::uint32_t mCommand;
            std::uint32_t mPad[3];
        };
        // Mirrors Batch in gpuscene_types.glsl.
        static_assert(sizeof(GpuBatch) == 32);
        static_assert(offsetof(GpuBatch, mFirstSlot) == 12);
        static_assert(offsetof(GpuBatch, mCommand) == 16);

        struct PendingMesh
        {
            Id mMesh = sInvalid;
            std::uint32_t mVertexOffset;
            std::uint32_t mIndexOffset;
            std::vector<std::uint8_t> mVertices;
            std::vector<std::uint32_t> mIndices;
            /// Skinning reads its own vertex copy.
            bool mSkinSource = false;
        };

        struct PendingInstances
        {
            Id mChunk = sInvalid;
            Id mGeneration = 0;
            std::uint32_t mFirst;
            std::vector<GpuInstance> mRecords;
            std::vector<GpuOcclusion> mOcclusion;
        };

        struct GpuSkinMatrix
        {
            float mCols[3][4];
        };

        struct GpuSkinTask
        {
            std::uint32_t mVertexOffset;
            std::uint32_t mVertexCount;
            std::uint32_t mMatrixOffset;
            std::uint32_t mFlags;
        };

        struct GpuSkinGroup
        {
            std::uint32_t mFirst;
            std::uint32_t mCount;
        };

        struct GpuSkinInfluence
        {
            std::uint32_t mBone;
            float mWeight;
        };
        // Mirrors gpuscene_skin.comp record layouts.
        static_assert(sizeof(GpuSkinMatrix) == 48);
        static_assert(sizeof(GpuSkinTask) == 16);
        static_assert(sizeof(GpuSkinGroup) == 8);
        static_assert(sizeof(GpuSkinInfluence) == 8);

        struct SkinMesh
        {
            std::uint32_t mGroupFirst;
            std::uint32_t mGroupCount;
            std::uint32_t mInfluenceFirst;
            std::uint32_t mInfluenceCount;
            std::uint32_t mBoneCount;
        };

        struct PendingSkin
        {
            Id mMesh = sInvalid;
            std::uint32_t mFirst;
            std::vector<std::uint32_t> mGroups;
            std::uint32_t mGroupFirst;
            std::vector<GpuSkinGroup> mGroupRanges;
            std::uint32_t mInfluenceFirst;
            std::vector<GpuSkinInfluence> mInfluences;
        };

        struct SkinPose
        {
            unsigned int mFrame = 0;
            unsigned int mSnapshot = 0;
            std::uint32_t mFirst = 0;
            std::uint32_t mCount = 0;
        };

        struct SkinPoseFrame
        {
            std::mutex mMutex;
            unsigned int mFrame = 0;
            bool mValid = false;
            std::unordered_map<const void*, SkinPose> mPoses;
            std::vector<GpuSkinMatrix> mMatrices;
        };

        struct ActorBatch;

        struct SkinUpdate
        {
            Id mMesh = sInvalid;
            SkinPose mPose;
            std::uint32_t mVertex = 0;
            osg::ref_ptr<const osg::Geometry> mMorph;
            Id mMaterial = sInvalid;
            std::uint32_t mPalette = 0;
            std::shared_ptr<const ActorBatch> mBatch;
        };

        struct ReceiverSlot
        {
            Id mSlot;
            std::uint32_t mReceiver;
        };

        struct ActorPart
        {
            osg::ref_ptr<osg::Drawable> mDrawable;
            osg::ref_ptr<osg::Geometry> mGeometry;
            SceneUtil::RigGeometry* mRig = nullptr;
            SceneUtil::MorphGeometry* mMorph = nullptr;
            Id mTransform = sInvalid;
            Id mMesh = sInvalid;
            Id mMaterial = sInvalid;
            std::uint32_t mVertex = 0;
            std::uint32_t mPalette = 0;
            std::uint32_t mMatrixCount = 0;
        };

        struct ActorBatch
        {
            struct Transform
            {
                osg::ref_ptr<osg::Transform> mNode;
                Id mParent;
            };
            std::vector<Transform> mTransforms;
            osg::ref_ptr<osg::Geometry> mGeometry;
            Id mMesh = sInvalid;
            Id mBucket = sInvalid;
            std::vector<ActorPart> mParts;
            std::uint32_t mMatrixCount = 0;
        };

        struct DynamicInstance
        {
            std::shared_ptr<const ActorBatch> mBatch;
            osg::ref_ptr<const osg::Geometry> mGeometry;
            osg::Matrixf mWorld;
            Id mMesh = sInvalid;
            Id mBucket = sInvalid;
            Id mMaterial = sInvalid;
            std::uint32_t mReceiver = 0;
            bool mUpdateMesh = false;
        };

        struct GpuView
        {
            std::array<osg::Vec4f, 16> mPlanes;
            osg::Vec4f mPixelSizeVector;
            osg::Vec4f mViewPoint;
            osg::Vec4f mParams;
            std::array<std::uint32_t, 4> mMeta;
            std::array<osg::Vec4f, 4> mViewProj;
        };
        // Mirrors View in gpuscene_types.glsl.
        static_assert(sizeof(GpuView) == 384);
        static_assert(offsetof(GpuView, mPixelSizeVector) == 256);
        static_assert(offsetof(GpuView, mViewProj) == 320);

        struct ViewRecord
        {
            /// Cull traversal the records below belong to.
            const osgUtil::RenderStage* mStage = nullptr;
            bool mHasParams = false;
            /// Culled by a frame dispatch.
            bool mDispatched = false;
            bool mPrepared = false;
            std::uint32_t mViewIndex = 0;
            std::vector<osg::Vec4f> mPlanes;
            osg::Vec4f mPixelSizeVector;
            float mSmallFeaturePixelSize = 0.f;
            osg::Vec3f mViewPoint;
            float mLodScale = 1.f;
            osg::Matrixf mViewProj;
            osg::Matrixf mLightProjection;
            osg::ref_ptr<osg::ShaderStorageBufferBinding> mLightReceivers;
            bool mOcclusion = false;
            bool mShadow = false;
            Id mStaticRangeOffset = 0;
            Id mStaticRangeCount = 0;
            std::vector<std::pair<Id, Id>> mChunks;
            std::vector<ReceiverSlot> mReceivers;
            std::vector<DynamicInstance> mDynamic;
            std::vector<SkinUpdate> mSkins;
        };

        struct Gl;
        struct Textures;
        struct RayRequest;
        struct RaySkin;
        struct RayBatch;
        struct RayState;
        class RayReadOperation;
        class RetireOperation;

        using ViewKey = std::pair<unsigned int, const osg::Camera*>;

        struct PendingWork
        {
            std::vector<PendingMesh> mMeshes;
            std::vector<PendingInstances> mInstances;
            std::vector<PendingSkin> mSkins;
            std::vector<PendingMaterial> mMaterials;
            std::vector<PendingRetire> mRetired;
            std::vector<GpuBatch> mBatches;
            std::uint32_t mSlotCount = 0;
            std::uint32_t mCommandCount = 0;
            bool mBatchesRebuilt = false;
        };

        using SurfacePath = std::vector<osg::ref_ptr<const osg::StateSet>>;

        enum class CullPass
        {
            Full,
            Seed,
            Resolve,
        };

        struct PathHash
        {
            using is_transparent = void;

            std::size_t operator()(const SurfacePath& path) const;
            std::size_t operator()(const std::vector<const osg::StateSet*>& path) const;
        };

        struct PathEqual
        {
            using is_transparent = void;

            bool operator()(const SurfacePath& left, const SurfacePath& right) const;
            bool operator()(const SurfacePath& left, const std::vector<const osg::StateSet*>& right) const;
            bool operator()(const std::vector<const osg::StateSet*>& left, const SurfacePath& right) const;
        };

        void rebuildBatches(PendingWork& work);
        Id getMeshLocked(const osg::Geometry& geometry, const osg::Geometry* source = nullptr);
        Id findMeshLocked(const osg::Geometry& geometry);
        Id insertMeshLocked(const osg::Geometry& geometry, PendingMesh&& pending);
        Id adoptPackedMeshLocked(const osg::Geometry& geometry, PendingMesh&& pending);
        void releaseMeshLocked(Id mesh);
        void queueMeshRelease(Id mesh);
        void drainMeshReleasesLocked();
        void dropMeshLocked(Id mesh);
        void dropSkinLocked(Id mesh);
        void dropChunkDrawsLocked(Id slot, Id generation);
        std::size_t evictMeshesLocked(unsigned int frame, std::size_t wanted);
        bool registerSkinLocked(Id mesh, const SceneUtil::RigGeometry& rig);
        bool queueSkinSource(Id mesh, const osg::Geometry& source);
        Surface getSurfaceLocked(const std::vector<const osg::StateSet*>& statePath, int order, MaterialUpdate updates);
        Id getBucketLocked(
            osg::StateSet& pipeline, const osg::Program& program, int order, Shader::MaterialMode mode, bool live);
        Id getActorBucket(Id bucket, bool full);
        std::unique_ptr<MaterialSource> makeSource(
            const std::vector<const osg::StateSet*>& statePath, const osg::Program& program) const;
        PendingMaterial makeMaterial(MaterialSource& source, Id index);
        void addMaterialLocked(Id material);
        void dropMaterialLocked(Id material);
        void markMaterialLocked(Id material, unsigned int frame);
        void retireMaterialsLocked(unsigned int frame);
        void publishMaterialLocked(Id material);
        void refreshMaterialLocked(Id material, unsigned int frame, bool samplers = false);
        /// Queue live bucket bindings for material maps whose tracked textures changed.
        void rebindMapsLocked(Id material, const PendingMaterial& pending);
        void rebindSamplersLocked(Id material);
        bool bucketEdited(Id bucket) const;
        void applyBucketEdits();
        void takePendingWork(PendingWork& work);
        void upload(osg::State& state, PendingWork& work);
        void failUploads(Gl& gl, PendingWork& work);
        /// False where storage buffer bindings fall short.
        bool checkLimits();
        bool linkArrayShaders(osg::State& state);
        bool sampleArrayLayer(osg::State& state);
        GpuView makeView(const ViewRecord& record) const;
        bool buildPyramid(osg::State& state, const osg::Texture& source);
        void cullLights(osg::State& state, std::uint32_t view, const osg::Matrixf& projection,
            osg::ShaderStorageBufferBinding& receivers);
        /// Capture static geometry or one actor at the shared GPU-scene boundary.
        void capture(osg::Node& node, SceneUtil::PositionAttitudeTransform* actor);
        /// Release one actor while the scene mutex is held.
        void removeActorLocked(SceneUtil::PositionAttitudeTransform& actor, std::vector<Id>* retired = nullptr);
        void restoreRigsLocked();
        void updateRigCullLocked();
        void recordRegisteredRigs(
            osgUtil::CullVisitor& cv, ViewRecord& view, bool nearFar, const osg::Matrix& modelView);
        bool dispatch(osg::State& state, const std::vector<GpuView>& views, std::uint32_t viewBase,
            std::uint32_t instanceExtent, const std::vector<std::uint32_t>& chunkData, CullPass pass,
            bool occlusionReady = false);
        bool record(osgUtil::CullVisitor& cv, osg::Geometry& geometry, bool updateMesh,
            const GpuCaptureCallback& callback, SkinUpdate* skin = nullptr);
        bool recordRig(osgUtil::CullVisitor& cv, SceneUtil::RigGeometry& rig, const GpuCaptureCallback& callback);
        bool getSkinPose(
            unsigned int frame, SceneUtil::RigGeometry& rig, SkinPose& out, unsigned int snapshot = sInvalid);
        bool getBatchPose(unsigned int frame, const ActorBatch& batch, SkinPose& out);
        void recordActorBatch(osgUtil::CullVisitor& cv, ViewRecord& view, const std::shared_ptr<ActorBatch>& batch,
            const osg::Matrixf& world, std::uint32_t receiver);
        bool runSkin(osg::State& state, const std::vector<GpuSkinTask>& tasks,
            const std::vector<GpuSkinMatrix>& matrices, std::uint32_t maxVertices);
        bool applySkin(osg::State& state, const std::vector<SkinUpdate>& skins);
        bool applyRaySkin(osg::State& state, const RayBatch& batch);
        bool prepareDynamic(osg::State& state, std::vector<DynamicInstance>& dynamic,
            const std::vector<SkinUpdate>& skins, std::uint32_t receiverBase, std::vector<ReceiverSlot>& receivers);
        bool prepareReceivers(std::uint32_t view, const std::vector<ReceiverSlot>& receivers);
        std::unique_ptr<RayBatch> makeRayBatchLocked(std::vector<RayRequest>&& requests);
        bool runRay(osg::State& state, osg::GraphicsContext& context, std::unique_ptr<RayBatch>& batch);
        static RayHit decodeRay(const RayBatch& batch, std::size_t ray, const void* hit, bool valid);
        static void finishRay(
            const std::shared_ptr<RayState>& state, const RayBatch& batch, const void* hits, bool valid);
        void finishFrame(osg::State& state);

        friend class GpuCaptureCallback;
        friend class GpuSceneNode;
        friend class GpuChunkNode;

        ViewRecord& cullRecord(osgUtil::CullVisitor& cv, unsigned int frame);
        void recordView(osgUtil::CullVisitor& cv);
        void markChunk(osgUtil::CullVisitor& cv, Id slot, Id generation, bool lit);
        void releaseChunk(GpuChunkNode& node, Id slot);
        void removeResidentLocked(GpuChunkNode& chunk);
        void update(unsigned int frame);
        void refreshLiveMaterials(osg::NodeVisitor& nv);
        osg::BoundingSphere getResidentBound() const;

        Shader::ShaderManager& mShaderManager;
        SceneUtil::LightManager* mLightManager = nullptr;
        bool mLightOcclusion = false;
        osg::ref_ptr<const SceneUtil::Material> mDefaultMaterial;
        std::atomic<bool> mSupported{ false };
        std::atomic<bool> mDemoted{ false };
        Shader::MaterialMode mMaterialMode = Shader::MaterialMode::Legacy;
        std::atomic<bool> mArraysProbed{ false };
        bool mGpuSkinningEnabled = false;
        bool mDistantPagingEnabled = false;
        std::atomic<bool> mEnabled{ true };
        std::atomic<bool> mOcclusionEnabled{ true };
        std::atomic<unsigned int> mNightDayMode{ 0 };
        bool mTerrainShadowOnce = false;
        bool mTerrainShadowRange = false;
        bool mShadowActiveRanges = false;
        bool mRegularActiveRanges = false;
        osg::ref_ptr<GpuSceneNode> mNode;
        /// Ordered pass dispatchers under one opaque bin.
        osg::ref_ptr<osg::Group> mOrderedNode;

        mutable std::mutex mMutex;
        std::unordered_map<const osg::Geometry*, Id> mMeshIds;
        std::vector<Mesh> mMeshes;
        std::vector<Id> mFreeMeshes;
        RangeAllocator mVertices;
        RangeAllocator mIndices;
        /// Vertex byte cap; zero is unlimited.
        std::size_t mMeshBudgetBytes = 0;
        /// Draw thread cap after a failed grow.
        std::atomic<std::size_t> mMeshCapBytes{ 0 };
        std::uint64_t mMeshEvictions = 0;
        RangeAllocator mReceiverSlots;
        unsigned int mEvictSweepFrame = std::numeric_limits<unsigned int>::max();
        bool mMeshBudgetWarned = false;
        /// Users dropped by callbacks dying under mMutex.
        std::mutex mReleaseMutex;
        std::vector<Id> mPendingReleases;
        RangeAllocator mSkinGroups;
        RangeAllocator mSkinInfluences;
        std::unordered_map<SurfacePath, Surface, PathHash, PathEqual> mSurfaces;
        /// Paths without a material, by last use.
        std::unordered_map<SurfacePath, unsigned int, PathHash, PathEqual> mFailedSurfaces;
        std::vector<Bucket> mBuckets;
        std::vector<BucketEdit> mBucketEdits;
        std::uint64_t mAppliedEdits = 0;
        const osg::Camera* mPrepassCamera = nullptr;
        /// Cameras whose subgraph holds a dispatcher.
        std::unordered_set<const osg::Camera*> mDispatchingCameras;
        struct RigEntry
        {
            osg::ref_ptr<SceneUtil::RigGeometry> mRig;
            SceneUtil::PositionAttitudeTransform* mActor;
            Id mMesh = sInvalid;
            Surface mSurface;
            osg::ref_ptr<SceneUtil::LightListCallback> mLightList;
            osg::ref_ptr<osg::Node> mLightNode;
            bool mBatched = false;
        };
        std::vector<RigEntry> mRigRegistry;
        /// Counts captured and GPU-owned rigs for one actor.
        struct ActorRigPopulation
        {
            std::size_t mTotal = 0;
            std::size_t mGpu = 0;
        };
        /// Retain captured rig eligibility so diagnostics can distinguish mixed GPU and CPU actor deformation.
        std::unordered_map<const SceneUtil::PositionAttitudeTransform*, ActorRigPopulation> mActorRigPopulations;
        std::unordered_map<const SceneUtil::PositionAttitudeTransform*, std::vector<std::shared_ptr<ActorBatch>>>
            mActorBatches;
        std::map<std::pair<Id, bool>, Id> mActorBuckets;
        std::map<std::pair<Id, std::uint32_t>, std::pair<const osg::Array*, unsigned int>> mMorphVersions;
        struct ActorSnapshot
        {
            std::uint32_t mMask = 0;
            osg::Matrixf mWorld;
        };
        /// Update-thread copies for draw-thread rays.
        std::unordered_map<const SceneUtil::PositionAttitudeTransform*, ActorSnapshot> mActorSnapshots;
        std::vector<RayRequest> mPendingRays;
        bool mRayInvalidate = false;
        RayId mNextRay = 0;
        std::shared_ptr<RayState> mRayState;
        std::atomic<bool> mRaySupported{ false };
        PostProcessor* mPostProcessor = nullptr;
        osg::ref_ptr<osg::Drawable> mPrepassDrawable;
        osg::ref_ptr<RetireOperation> mRetireOperation;
        std::vector<osg::StateAttribute::GLMode> mDeltaModes;
        std::vector<osg::StateAttribute::TypeMemberPair> mDeltaAttributes;
        std::vector<Dispatcher> mDispatchers;
        std::vector<PendingMaterial> mMaterials;
        std::vector<std::unique_ptr<MaterialSource>> mMaterialSources;
        std::vector<unsigned int> mMaterialFrames;
        enum class MaterialState : std::uint8_t
        {
            Active,
            Queued,
            Published,
        };
        std::vector<std::uint32_t> mMaterialUsers;
        std::vector<unsigned int> mMaterialLast;
        std::vector<MaterialState> mMaterialStates;
        /// Slots the sweeps visit; published ids leave.
        std::vector<Id> mActiveMaterials;
        struct LiveMaterials
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            std::vector<osg::ref_ptr<osg::Callback>> mUpdaters;
            std::vector<Id> mMaterials;
        };
        /// Resident materials whose records animate.
        std::map<std::vector<std::uintptr_t>, LiveMaterials> mLiveMaterials;
        unsigned int mLiveFrame = 0;
        std::vector<SurfacePath> mMaterialPaths;
        unsigned int mLifetimeFrame = 0;
        std::vector<osg::ref_ptr<osg::Program>> mProgramsToLink;
        /// Shadow casting programs indexed by alpha function.
        std::vector<osg::ref_ptr<osg::Program>> mCastingProgramList;
        osg::ref_ptr<osg::Stats> mStats;
        std::map<std::pair<Id, Id>, Id> mBatchIds;
        std::vector<Batch> mBatches;
        std::vector<Chunk> mChunks;
        std::vector<Id> mFreeChunks;
        std::unordered_set<GpuChunkNode*> mChunkNodes;
        struct Resident
        {
            Id mSlot;
            Id mGeneration;
            osg::BoundingSphere mBound;
            bool mLit;
        };
        std::vector<Resident> mResidents;
        std::vector<GpuChunkNode*> mResidentNodes;
        Id mNextGeneration = 1;
        RangeAllocator mInstances;
        std::deque<PendingMesh> mPendingMeshes;
        std::deque<PendingInstances> mPendingInstances;
        std::deque<PendingSkin> mPendingSkins;
        std::deque<PendingMaterial> mPendingMaterials;
        std::deque<PendingRetire> mPendingRetired;
        std::vector<std::pair<osg::ref_ptr<osg::Drawable>, bool>> mPendingDrawables;
        bool mBatchesDirty = false;
        bool mBoundDirty = false;
        /// Released chunks leave the bound too large.
        bool mResidentBoundStale = false;
        osg::BoundingSphere mResidentBound;
        std::map<ViewKey, ViewRecord> mViews;
        std::vector<std::pair<Id, Id>> mStaticRanges;
        std::vector<osg::observer_ptr<GpuCaptureCallback>> mCaptureCallbacks;
        /// Size that triggers the next compaction.
        std::size_t mCaptureWatermark = 1024;

        unsigned int mUpdateFrame = 0;
        std::unordered_set<const osg::Geometry*> mUpdatedMeshes;
        std::unordered_map<const osg::Geometry*, unsigned int> mMeshVersions;
        unsigned int mDispatchFrame = 0;
        std::uint32_t mDispatchedViews = 0;
        std::unordered_map<Id, SkinMesh> mSkinMeshes;
        std::array<SkinPoseFrame, 4> mSkinPoseFrames;
        std::atomic<bool> mSkinSupported{ true };
        std::atomic<std::size_t> mTextureHandles{ 0 };
        float mGroundcoverDetailedDistance = 0.f;
        float mGroundcoverViewDistance = 0.f;

        std::unique_ptr<Gl> mGl;
        std::unique_ptr<Textures> mTextures;
    };

    struct GpuGeometry
    {
        osg::ref_ptr<const osg::Geometry> mGeometry;
        GpuScene::Surface mSurface;
        osg::Matrixf mMatrix;
        osg::BoundingBox mBounds;
        const osg::LOD* mLod = nullptr;
        unsigned int mLodChild = 0;
        osg::Vec3f mLodCenter;
        float mLodScale = 1.f;
        const osg::Drawable* mDrawable = nullptr;
        unsigned int mModes = 0;
    };

    struct GpuTemplate
    {
        std::vector<GpuGeometry> mGeometries;
        std::unordered_set<const osg::Drawable*> mClaimed;
        bool mLegacy = false;
    };
}

#endif
