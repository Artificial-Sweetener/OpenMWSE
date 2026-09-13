#ifndef OPENMW_MWRENDER_GPUSCENEGLINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENEGLINTERNAL_H

namespace MWRender
{
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

        struct DrawStats
        {
            unsigned int mFrame = 0;
            std::size_t mUsed = 0;
            std::vector<GLuint> mQueries;
        };

        std::array<DrawStats, 4> mDrawStats;
        unsigned int mStatsFrame = ~0u;
        bool mCollectStats = false;
        bool beginStats(unsigned int frame, osg::Stats& stats);

        osg::GLExtensions* mExt = nullptr;
        bool mTraceDraws = false;
        bool mFlushUploads = false;
        unsigned int mUploadCount = 0;
        GLenum(GL_APIENTRY* mGetResetStatus)() = nullptr;
        /// Submit GPU-scene commands without leaving OSG's tracked indirect-buffer binding stale.
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
        struct ReceiverBuffer
        {
            GLuint mBuffer = 0;
            std::uint32_t mSlots = 0;
        };
        std::vector<ReceiverBuffer> mReceiverBuffers;
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
        osg::observer_ptr<osg::GraphicsThread> mDrawThread;
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
        osg::ref_ptr<osg::Program> mLightCullProgram;
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
        std::map<std::tuple<const osg::Program*, bool, bool, Shader::MaterialMode>, osg::ref_ptr<osg::StateSet>>
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

        /// Vertex arrays hold the buffer names.
        void deleteVaos();

        void beginUpload();

        /// False leaves the old buffer in place.
        bool grow(GLuint& buffer, std::size_t& capacity, std::size_t required, GLenum usage, std::size_t limit = 0);

        void stream(GLuint& buffer, const void* data, std::size_t size, GLenum usage = GL_STREAM_DRAW);

        void upload(GLuint buffer, std::size_t offset, const void* data, std::size_t size);

        void createVao(osg::State& state, bool dynamic = false);

        HandleEntry& acquire(osg::State& state, const HandleKey& key);

        static bool contains(const std::array<std::optional<HandleKey>, sMapCount>& keys, const HandleKey& key);

        void dropHandle(const HandleKey& key);

        void retireTextureHandles(const osg::Texture* texture);

        void setMaterial(osg::State& state, PendingMaterial& material);

        void retireMaterial(PendingRetire&& retired);

        void pollRetired(GpuScene& scene, osg::State& state);

        void advance(GpuScene& scene, osg::State& state);

        /// One fence covers everything staged this iteration.
        void fenceRetired();

        void cleanup(GpuScene& scene, osg::State& state);

        bool resizeViews(std::uint32_t views);

        void zero(GLuint buffer);
    };
}

#endif
