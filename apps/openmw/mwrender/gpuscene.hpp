#ifndef OPENMW_MWRENDER_GPUSCENE_H
#define OPENMW_MWRENDER_GPUSCENE_H

#include <components/shader/materialmode.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
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
#include <components/terrain/chunkmanager.hpp>

namespace osg
{
    class Callback;
    class Camera;
    class Drawable;
    class Geometry;
    struct GraphicsOperation;
    class Node;
    class Program;
    class Texture;
    class RenderInfo;
    class State;
    class StateSet;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace SceneUtil
{
    class LightManager;
    class Material;
    class RigGeometry;
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

        explicit GpuScene(Shader::ShaderManager& shaderManager);
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

        Id getMesh(const osg::Geometry& geometry);
        /// Separates ordered passes into distinct buckets.
        Surface getSurface(const std::vector<const osg::StateSet*>& statePath, int order = 0);
        /// Extracts GPU-drawable model geometry.
        std::unique_ptr<const GpuTemplate> extract(const osg::Node& node, unsigned int mask, bool bakeBillboards);
        void appendInstances(const GpuTemplate& tmpl, const osg::Matrixf& refMatrix, float refScale,
            const osg::LOD::MinMaxPair& refDistances, bool smallFeatureCulling, std::vector<Instance>& out) const;
        osg::ref_ptr<osg::Node> createChunk(std::vector<Instance>&& instances, const osg::BoundingSphere& bound);
        /// Rebuilds chunk instance records in place.
        bool updateChunk(osg::Node& node, const std::vector<Instance>& instances, const osg::BoundingSphere& bound);
        /// Culls residents without scene graph nodes.
        void addResident(osg::Node& node);
        void removeResident(osg::Node& node);
        void setCastingPrograms(const std::vector<osg::ref_ptr<osg::Program>>& programs);
        void setLightManager(SceneUtil::LightManager* lightManager);
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
        void compile(osg::State& state);

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

        struct PendingMaterial
        {
            Id mIndex;
            GpuMaterial mRecord;
            std::array<osg::ref_ptr<osg::Texture>, sMapCount> mTextures;
        };

        struct MaterialSource;

        struct Bucket
        {
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<const osg::Program> mProgram;
            /// Only modes, attributes and the program differ.
            bool mDelta = false;
            std::vector<Id> mBatches;
            std::uint32_t mCommandOffset = 0;
            std::uint32_t mCommandCount = 0;
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
            osg::BoundingSphere mBounds;
        };

        struct GpuInstance
        {
            float mRows[3][4];
            float mBounds[4];
            float mLod[4];
            std::uint32_t mMeta[4];
            std::uint32_t mMaterial[4];
        };

        static GpuInstance makeRecord(const Instance& instance, Id batch, Id slot, Id generation);

        struct GpuOcclusion
        {
            float mMin[3];
            std::uint32_t mPad0;
            float mMax[3];
            std::uint32_t mPad1;
        };
        static_assert(sizeof(GpuOcclusion) == 32);

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
            std::uint32_t mFirst = 0;
            std::uint32_t mCount = 0;
        };

        struct SkinPoseFrame
        {
            std::mutex mMutex;
            unsigned int mFrame = 0;
            bool mValid = false;
            std::unordered_map<const osg::Geometry*, SkinPose> mPoses;
            std::vector<GpuSkinMatrix> mMatrices;
        };

        struct SkinUpdate
        {
            Id mMesh = sInvalid;
            SkinPose mPose;
        };

        struct DynamicInstance
        {
            osg::ref_ptr<const osg::Geometry> mGeometry;
            osg::Matrixf mWorld;
            Id mMesh = sInvalid;
            Id mBucket = sInvalid;
            Id mMaterial = sInvalid;
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
            std::vector<DynamicInstance> mDynamic;
            std::vector<SkinUpdate> mSkins;
        };

        struct Gl;

        using ViewKey = std::pair<unsigned int, const osg::Camera*>;

        struct PendingWork
        {
            std::vector<PendingMesh> mMeshes;
            std::vector<PendingInstances> mInstances;
            std::vector<PendingSkin> mSkins;
            std::vector<PendingMaterial> mMaterials;
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
        bool registerSkinLocked(Id mesh, const SceneUtil::RigGeometry& rig);
        Surface getSurfaceLocked(const std::vector<const osg::StateSet*>& statePath, int order);
        std::unique_ptr<MaterialSource> makeSource(
            const std::vector<const osg::StateSet*>& statePath, const osg::Program& program) const;
        PendingMaterial makeMaterial(MaterialSource& source, Id index) const;
        void refreshMaterialLocked(Id material, unsigned int frame);
        void takePendingWork(PendingWork& work, std::size_t limit);
        void upload(osg::State& state, PendingWork& work);
        static GpuView makeView(const ViewRecord& record);
        bool buildPyramid(osg::State& state, const osg::Texture& source);
        bool dispatch(osg::State& state, const std::vector<GpuView>& views, std::uint32_t viewBase,
            std::uint32_t instanceExtent, const std::vector<std::uint32_t>& chunkData, CullPass pass,
            bool occlusionReady = false);
        bool record(osgUtil::CullVisitor& cv, osg::Geometry& geometry, bool updateMesh,
            const GpuCaptureCallback& callback, SkinUpdate* skin = nullptr);
        bool recordRig(osgUtil::CullVisitor& cv, SceneUtil::RigGeometry& rig, const GpuCaptureCallback& callback);
        bool getSkinPose(unsigned int frame, SceneUtil::RigGeometry& rig, SkinPose& out);
        bool applySkin(osg::State& state, const std::vector<SkinUpdate>& skins);
        bool prepareDynamic(
            osg::State& state, std::vector<DynamicInstance>& dynamic, const std::vector<SkinUpdate>& skins);

        friend class GpuCaptureCallback;
        friend class GpuSceneNode;
        friend class GpuChunkNode;

        void recordView(osgUtil::CullVisitor& cv);
        void markChunk(osgUtil::CullVisitor& cv, Id slot, Id generation);
        void releaseChunk(GpuChunkNode& node, Id slot);
        void removeResidentLocked(GpuChunkNode& chunk);
        void update();
        osg::BoundingSphere getResidentBound() const;

        Shader::ShaderManager& mShaderManager;
        SceneUtil::LightManager* mLightManager = nullptr;
        osg::ref_ptr<const SceneUtil::Material> mDefaultMaterial;
        std::atomic<bool> mSupported{ false };
        Shader::MaterialMode mMaterialMode = Shader::MaterialMode::Legacy;
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
        const osg::Camera* mPrepassCamera = nullptr;
        PostProcessor* mPostProcessor = nullptr;
        osg::ref_ptr<osg::Drawable> mPrepassDrawable;
        std::vector<osg::StateAttribute::GLMode> mDeltaModes;
        std::vector<osg::StateAttribute::TypeMemberPair> mDeltaAttributes;
        std::vector<Dispatcher> mDispatchers;
        std::vector<PendingMaterial> mMaterials;
        std::vector<std::unique_ptr<MaterialSource>> mMaterialSources;
        std::vector<unsigned int> mMaterialFrames;
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
        };
        std::vector<Resident> mResidents;
        std::vector<GpuChunkNode*> mResidentNodes;
        Id mNextGeneration = 1;
        RangeAllocator mInstances;
        std::deque<PendingMesh> mPendingMeshes;
        std::deque<PendingInstances> mPendingInstances;
        std::deque<PendingSkin> mPendingSkins;
        std::deque<PendingMaterial> mPendingMaterials;
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

        std::unique_ptr<Gl> mGl;
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
