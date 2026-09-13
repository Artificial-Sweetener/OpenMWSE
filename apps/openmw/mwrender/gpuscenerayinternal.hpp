#ifndef OPENMW_MWRENDER_GPUSCENERAYINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENERAYINTERNAL_H

namespace MWRender
{
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
}

#endif
