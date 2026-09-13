#include "gpusceneinternal.hpp"

namespace MWRender
{
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
                osg::ref_ptr<const osg::Geometry> geometry;
                if (!mesh.mGeometry.lock(geometry) || mesh.mFailed || mesh.mBaseVertex < 0 || mesh.mIndexCount < 3)
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
                skin.mGeometry = geometry;
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
                if (mesh.mGeometry.get() != skin.mGeometry.get() || mesh.mBaseVertex < 0 || mesh.mFailed
                    || registered == mSkinMeshes.end() || registered->second.mBoneCount + 1 != skin.mCount)
                    return false;
                unsigned int flags = 0;
                if (skin.mGeometry->getNormalArray() != nullptr)
                    flags |= 1;
                if (dynamic_cast<const osg::Vec4Array*>(skin.mGeometry->getTexCoordArray(7)) != nullptr)
                    flags |= 2;
                tasks.push_back(
                    { static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount, skin.mFirst, flags });
                maxVertices = std::max(maxVertices, mesh.mVertexCount);
            }
        }
        return runSkin(state, tasks, batch.mSkinMatrices, maxVertices);
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
            takePendingWork(work);
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
}
