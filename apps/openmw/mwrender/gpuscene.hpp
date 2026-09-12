#ifndef OPENMW_MWRENDER_GPUSCENE_H
#define OPENMW_MWRENDER_GPUSCENE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    class Texture;
    class RenderInfo;
    class State;
    class StateSet;
    class Stats;
}

namespace osgUtil
{
    class CullVisitor;
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
        using Id = std::uint32_t;
        static constexpr Id sInvalid = 0xffffffffu;
        static constexpr Id sPrepass = 0xfffffffeu;
        static constexpr std::size_t sMapCount = Shader::sMaterialMapCount;
        using RayId = std::uint64_t;
        static constexpr RayId sInvalidRay = ~RayId{ 0 };

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
            bool mSmallFeatureCulling = false;
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

        /// Construct the GPU scene with immutable skinning and material policies.
        GpuScene(Shader::ShaderManager& shaderManager, bool gpuSkinning, bool packedTextures);
        ~GpuScene() override;

        osg::ref_ptr<osg::Node> createTerrainChunk(
            Terrain::TerrainDrawable& drawable, const osg::Vec3f& worldOffset, bool lit) override;

        bool isSupported() const { return mSupported; }
        bool isEnabled() const { return mSupported && mEnabled; }
        void setEnabled(bool enabled);
        /// Enable or disable depth-pyramid occlusion while retaining GPU scene rendering.
        void setOcclusionEnabled(bool enabled) { mOcclusionEnabled = enabled; }
        osg::Group* getNode();
        void capture(osg::Node& node);
        void captureActor(osg::Node& node, SceneUtil::PositionAttitudeTransform& actor);
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
        /// False once a cached material retires.
        bool isTemplateLive(const GpuTemplate& tmpl) const;
        osg::ref_ptr<osg::Node> createChunk(
            std::vector<Instance>&& instances, const osg::BoundingSphere& bound, bool lit);
        /// Rebakes a chunk's instance records in place.
        bool updateChunk(osg::Node& node, const std::vector<Instance>& instances, const osg::BoundingSphere& bound);
        /// Culls residents without scene graph nodes.
        void addResident(osg::Node& node);
        void removeResident(osg::Node& node);
        void setCastingPrograms(const std::vector<osg::ref_ptr<osg::Program>>& programs);
        void setLightManager(SceneUtil::LightManager* lightManager);
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
        unsigned int getCompletedWork() const { return mCompletedWork; }
        /// Spreads pending work across frames.
        osg::ref_ptr<osg::GraphicsOperation> createCompileOperation(unsigned int target);
        /// One-shot array proof, null once done.
        osg::ref_ptr<osg::GraphicsOperation> createProbeOperation();
        /// Fences retired resources after each frame.
        osg::ref_ptr<osg::GraphicsOperation> createRetireOperation();
        /// Proves array materials in this context.
        void probeArrays(osg::State& state);
        void compile(osg::State& state);
        void reportStats(unsigned int frame, osg::Stats& stats) const;
        void releaseGLObjects(osg::State* state);

        void draw(osg::RenderInfo& renderInfo, Id bucket);

    private:
        struct Mesh
        {
            osg::ref_ptr<const osg::Geometry> mGeometry;
            std::uint32_t mFirstIndex;
            std::uint32_t mIndexCount;
            std::int32_t mBaseVertex;
            std::uint32_t mVertexCount;
            std::uint32_t mUsers = 0;
        };

        struct RangeAllocator
        {
            std::map<std::uint32_t, std::uint32_t> mFree;
            std::uint32_t mExtent = 0;
            std::uint32_t allocate(std::uint32_t count);
            void free(std::uint32_t first, std::uint32_t count);
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
            /// Two nulls strip textures and the material.
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
        };

        struct GpuInstance
        {
            float mRows[3][4];
            float mBounds[4];
            float mLod[4];
            std::uint32_t mMeta[4];
            std::uint32_t mMaterial[4];
        };
        // Mirrors the instance record in the shaders.
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
        // Mirrors the cull shader batch table.
        static_assert(sizeof(GpuBatch) == 32);
        static_assert(offsetof(GpuBatch, mFirstSlot) == 12);
        static_assert(offsetof(GpuBatch, mCommand) == 16);

        struct PendingMesh
        {
            std::uint32_t mVertexOffset;
            std::uint32_t mIndexOffset;
            std::vector<std::uint8_t> mVertices;
            std::vector<std::uint32_t> mIndices;
        };

        struct PendingInstances
        {
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

        struct ReceiverSlot
        {
            Id mSlot;
            std::uint32_t mReceiver;
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
            bool mOcclusion = false;
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
        void releaseMeshLocked(Id mesh);
        bool registerSkinLocked(Id mesh, const SceneUtil::RigGeometry& rig);
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
        void rebindMapsLocked(Id material, const PendingMaterial& pending);
        void rebindSamplersLocked(Id material);
        bool bucketEdited(Id bucket) const;
        void applyBucketEdits();
        void takePendingWork(PendingWork& work, std::size_t limit);
        void upload(osg::State& state, PendingWork& work);
        bool linkArrayShaders(osg::State& state);
        bool sampleArrayLayer(osg::State& state);
        static GpuView makeView(const ViewRecord& record);
        bool buildPyramid(osg::State& state, const osg::Texture& source);
        void capture(osg::Node& node, SceneUtil::PositionAttitudeTransform* actor);
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

        void recordView(osgUtil::CullVisitor& cv);
        void markChunk(osgUtil::CullVisitor& cv, Id slot, Id generation, bool lit);
        void releaseChunk(GpuChunkNode& node, Id slot);
        void removeResidentLocked(GpuChunkNode& chunk);
        void update(unsigned int frame);
        void refreshLiveMaterials(osg::NodeVisitor& nv);
        osg::BoundingSphere getResidentBound() const;

        Shader::ShaderManager& mShaderManager;
        SceneUtil::LightManager* mLightManager = nullptr;
        osg::ref_ptr<const SceneUtil::Material> mDefaultMaterial;
        std::atomic<bool> mSupported{ false };
        Shader::MaterialMode mMaterialMode = Shader::MaterialMode::Legacy;
        std::atomic<bool> mArraysProbed{ false };
        bool mGpuSkinningEnabled = false;
        std::atomic<bool> mEnabled{ true };
        std::atomic<bool> mOcclusionEnabled{ true };
        osg::ref_ptr<GpuSceneNode> mNode;
        /// Ordered pass dispatchers under one opaque bin.
        osg::ref_ptr<osg::Group> mOrderedNode;

        mutable std::mutex mMutex;
        std::unordered_map<const osg::Geometry*, Id> mMeshIds;
        std::vector<Mesh> mMeshes;
        std::vector<Id> mFreeMeshes;
        RangeAllocator mVertices;
        RangeAllocator mIndices;
        RangeAllocator mSkinGroups;
        RangeAllocator mSkinInfluences;
        std::unordered_map<SurfacePath, Surface, PathHash, PathEqual> mSurfaces;
        std::vector<Bucket> mBuckets;
        std::vector<BucketEdit> mBucketEdits;
        std::uint64_t mAppliedEdits = 0;
        const osg::Camera* mPrepassCamera = nullptr;
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
        std::atomic<unsigned int> mCompletedWork{ 0 };
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
        RangeAllocator mReceiverSlots;
        std::deque<PendingMesh> mPendingMeshes;
        std::deque<PendingInstances> mPendingInstances;
        std::deque<PendingSkin> mPendingSkins;
        std::deque<PendingMaterial> mPendingMaterials;
        std::deque<PendingRetire> mPendingRetired;
        std::vector<std::pair<osg::ref_ptr<osg::Drawable>, bool>> mPendingDrawables;
        bool mBatchesDirty = false;
        bool mBoundDirty = false;
        osg::BoundingSphere mResidentBound;
        std::map<ViewKey, ViewRecord> mViews;
        /// Cameras whose subgraph holds a dispatcher.
        std::unordered_set<const osg::Camera*> mDispatchingCameras;
        std::vector<ViewRecord> mFreeViews;
        std::vector<osg::observer_ptr<GpuCaptureCallback>> mCaptureCallbacks;

        unsigned int mUpdateFrame = 0;
        std::unordered_set<const osg::Geometry*> mUpdatedMeshes;
        std::unordered_map<const osg::Geometry*, unsigned int> mMeshVersions;
        unsigned int mDispatchFrame = 0;
        std::uint32_t mDispatchedViews = 0;
        std::unordered_map<Id, SkinMesh> mSkinMeshes;
        std::array<SkinPoseFrame, 4> mSkinPoseFrames;
        std::atomic<bool> mSkinSupported{ true };
        std::atomic<std::size_t> mTextureHandles{ 0 };

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
    };

    struct GpuTemplate
    {
        std::vector<GpuGeometry> mGeometries;
        std::unordered_set<const osg::Drawable*> mClaimed;
        bool mLegacy = false;
    };
}

#endif
