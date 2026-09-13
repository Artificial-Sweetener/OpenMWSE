#include "gpusceneinternal.hpp"

namespace MWRender
{
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
                Batch& batch = mBatches[batchId];
                batch.mCommand = sInvalid;
                const Mesh& mesh = mMeshes[batch.mMesh];
                if (batch.mResident == 0 || mesh.mFailed)
                    continue;
                batch.mCommand = command;
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

    void GpuScene::takePendingWork(PendingWork& work)
    {
        const auto take = [](auto& from, auto& to) {
            to.assign(std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
            from.clear();
        };
        take(mPendingMeshes, work.mMeshes);
        take(mPendingInstances, work.mInstances);
        take(mPendingSkins, work.mSkins);
        take(mPendingMaterials, work.mMaterials);
        take(mPendingRetired, work.mRetired);
        if (mBatchesDirty)
            rebuildBatches(work);
    }

    /// Drops work a full buffer rejects.
    void GpuScene::failUploads(Gl& gl, PendingWork& work)
    {
        const std::size_t vertexCap = gl.mVertexCapacity;
        std::lock_guard<std::mutex> lock(mMutex);
        if (mMeshCapBytes.load(std::memory_order_relaxed) == 0)
            Log(Debug::Warning) << "GPU buffer growth failed; pool capped.";
        mMeshCapBytes.store(vertexCap, std::memory_order_relaxed);
        std::erase_if(work.mMeshes, [&](const PendingMesh& mesh) {
            const bool fits = mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size() <= vertexCap
                && (!mesh.mSkinSource
                    || mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size() <= gl.mSkinSourceCapacity)
                && (mesh.mIndexOffset + mesh.mIndices.size()) * sizeof(std::uint32_t) <= gl.mIndexCapacity;
            if (fits)
                return false;
            // Another mesh may hold the range.
            if (mesh.mMesh < mMeshes.size() && mMeshes[mesh.mMesh].mKey != nullptr
                && static_cast<std::uint32_t>(mMeshes[mesh.mMesh].mBaseVertex) == mesh.mVertexOffset)
            {
                if (mesh.mSkinSource)
                    dropSkinLocked(mesh.mMesh);
                mMeshes[mesh.mMesh].mFailed = true;
                // Rebuild drops the failed batch.
                mBatchesDirty = true;
            }
            return true;
        });
        std::erase_if(work.mInstances, [&](const PendingInstances& pending) {
            const bool fits = (pending.mFirst + pending.mRecords.size()) * sizeof(GpuInstance) <= gl.mInstanceCapacity
                && (pending.mFirst + pending.mOcclusion.size()) * sizeof(GpuOcclusion) <= gl.mOcclusionCapacity;
            if (fits)
                return false;
            dropChunkDrawsLocked(pending.mChunk, pending.mGeneration);
            return true;
        });
        std::erase_if(work.mSkins, [&](const PendingSkin& skin) {
            const bool fits = (skin.mFirst + skin.mGroups.size()) * sizeof(std::uint32_t) <= gl.mSkinGroupCapacity
                && (skin.mGroupFirst + skin.mGroupRanges.size()) * sizeof(GpuSkinGroup) <= gl.mSkinRangeCapacity
                && (skin.mInfluenceFirst + skin.mInfluences.size()) * sizeof(GpuSkinInfluence)
                    <= gl.mSkinInfluenceCapacity;
            const auto registered = mSkinMeshes.find(skin.mMesh);
            // A dropped registration frees these ranges.
            const bool live = registered != mSkinMeshes.end() && registered->second.mGroupFirst == skin.mGroupFirst
                && registered->second.mInfluenceFirst == skin.mInfluenceFirst;
            if (fits && live)
                return false;
            if (live)
                dropSkinLocked(skin.mMesh);
            return true;
        });
        std::erase_if(work.mMaterials, [&](const PendingMaterial& material) {
            if ((material.mIndex + 1) * sizeof(GpuMaterial) <= gl.mMaterialCapacity)
                return false;
            if (material.mIndex < mMaterialStates.size()
                && mMaterialStates[material.mIndex] != MaterialState::Published)
                publishMaterialLocked(material.mIndex);
            return true;
        });
    }

    void GpuScene::upload(osg::State& state, PendingWork& work)
    {
        probeArrays(state);
        if (!mSupported)
            return;
        Gl& gl = *mGl;
        gl.mUploadCount = 0;
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
        }
        mTextures->sync(*this, state);

        std::size_t vertexBytes = 0;
        std::size_t skinSourceBytes = 0;
        std::size_t indexBytes = 0;
        for (const PendingMesh& mesh : work.mMeshes)
        {
            vertexBytes = std::max(vertexBytes, mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size());
            // Only skinned meshes read the source arena.
            if (mesh.mSkinSource)
                skinSourceBytes
                    = std::max(skinSourceBytes, mesh.mVertexOffset * sizeof(GpuVertex) + mesh.mVertices.size());
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
        const std::size_t meshLimit = mTextures->mBudgetBytes / 8;
        bool grown = gl.grow(gl.mVertexBuffer, gl.mVertexCapacity, vertexBytes, GL_STATIC_DRAW, meshLimit);
        grown = gl.grow(gl.mSkinSourceBuffer, gl.mSkinSourceCapacity, skinSourceBytes, GL_STATIC_DRAW, meshLimit)
            && grown;
        grown = gl.grow(gl.mSkinGroupBuffer, gl.mSkinGroupCapacity, skinGroupBytes, GL_STATIC_DRAW) && grown;
        grown = gl.grow(gl.mSkinRangeBuffer, gl.mSkinRangeCapacity, skinRangeBytes, GL_STATIC_DRAW) && grown;
        grown
            = gl.grow(gl.mSkinInfluenceBuffer, gl.mSkinInfluenceCapacity, skinInfluenceBytes, GL_STATIC_DRAW) && grown;
        grown = gl.grow(gl.mIndexBuffer, gl.mIndexCapacity, indexBytes, GL_STATIC_DRAW) && grown;
        grown = gl.grow(gl.mMaterialBuffer, gl.mMaterialCapacity, materialBytes, GL_DYNAMIC_COPY) && grown;
        grown = gl.grow(gl.mInstanceBuffer, gl.mInstanceCapacity, instanceBytes, GL_DYNAMIC_COPY) && grown;
        grown = gl.grow(gl.mOcclusionBuffer, gl.mOcclusionCapacity, occlusionBytes, GL_DYNAMIC_COPY) && grown;
        mTextures->mReservedBytes.store(gl.mVertexCapacity + gl.mSkinSourceCapacity + gl.mSkinGroupCapacity
                + gl.mSkinRangeCapacity + gl.mSkinInfluenceCapacity + gl.mIndexCapacity + gl.mMaterialCapacity
                + gl.mInstanceCapacity + gl.mOcclusionCapacity,
            std::memory_order_relaxed);
        if (!grown)
            failUploads(gl, work);
        for (const PendingMesh& mesh : work.mMeshes)
        {
            gl.upload(
                gl.mVertexBuffer, mesh.mVertexOffset * sizeof(GpuVertex), mesh.mVertices.data(), mesh.mVertices.size());
            if (mesh.mSkinSource)
                gl.upload(gl.mSkinSourceBuffer, mesh.mVertexOffset * sizeof(GpuVertex), mesh.mVertices.data(),
                    mesh.mVertices.size());
            if (!mesh.mIndices.empty())
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
        if (work.mBatchesRebuilt)
        {
            const std::vector<GpuBatch>& batches = work.mBatches;
            const std::size_t batchBytes = std::max<std::size_t>(batches.size(), 1) * sizeof(GpuBatch);
            if (!gl.grow(gl.mBatchBuffer, gl.mBatchCapacity, batchBytes, GL_DYNAMIC_COPY))
            {
                Log(Debug::Warning) << "GPU batch buffer allocation failed.";
                mSupported = false;
                mDemoted.store(true, std::memory_order_relaxed);
                return;
            }
            if (!batches.empty())
                gl.upload(gl.mBatchBuffer, 0, batches.data(), batches.size() * sizeof(GpuBatch));
            gl.mBatchCount = static_cast<std::uint32_t>(batches.size());
            gl.mSlotCount = work.mSlotCount;
            gl.mCommandCount = work.mCommandCount;
            gl.mViewCapacity = 0;
        }
    }

    bool GpuScene::checkLimits()
    {
        if (mGl == nullptr)
            return false;
        if (!mGl->mLimitsChecked)
        {
            mGl->mLimitsChecked = true;
            const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            mGl->mFlushUploads = vendor != nullptr && std::string_view(vendor) == "ATI Technologies Inc.";
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
                mDemoted.store(true, std::memory_order_relaxed);
            }
            // Windows resolves these only with a current context.
            osg::setGLExtensionFuncPtr(mGl->mClearBufferData, "glClearBufferData");
            osg::setGLExtensionFuncPtr(mGl->mCopyBufferSubData, "glCopyBufferSubData");
            if (mGl->mCopyBufferSubData == nullptr || mGl->mClearBufferData == nullptr)
            {
                Log(Debug::Warning) << "GPU paging unavailable: buffer functions missing";
                mSupported = false;
                mDemoted.store(true, std::memory_order_relaxed);
            }
        }
        return mSupported.load(std::memory_order_relaxed);
    }

    void GpuScene::compile(osg::State& state)
    {
        if (!checkLimits())
            return;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTextures->resolve(state);
            const unsigned int frame = state.getFrameStamp() != nullptr ? state.getFrameStamp()->getFrameNumber() : 0;
            mTextures->prepareTiers();
            mTextures->manage(*this, frame);
        }
        PendingWork work;
        std::vector<osg::ref_ptr<osg::Program>> programs;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            takePendingWork(work);
            programs.swap(mProgramsToLink);
        }
        upload(state, work);
        for (const osg::ref_ptr<osg::Program>& program : programs)
            program->compileGLObjects(state);
    }

    void GpuScene::releaseGLObjects(osg::State* state)
    {
        if (state == nullptr || mGl == nullptr)
            return;
        Gl& gl = *mGl;
        for (Gl::DrawStats& pending : gl.mDrawStats)
        {
            gl.mExt->glDeleteQueries(static_cast<GLsizei>(pending.mQueries.size()), pending.mQueries.data());
            pending = {};
        }
        gl.mStatsFrame = ~0u;
        gl.mCollectStats = false;
        gl.cleanup(*this, *state);
        mTextureHandles.store(0, std::memory_order_relaxed);
        GLuint buffers[] = { gl.mVertexBuffer, gl.mIndexBuffer, gl.mInstanceBuffer, gl.mBatchBuffer, gl.mChunkBuffer,
            gl.mCountBuffer, gl.mVisibleBuffer, gl.mCommandBuffer, gl.mViewBuffer, gl.mOcclusionBuffer,
            gl.mMaterialBuffer, gl.mDynamicInstanceBuffer, gl.mDynamicVisibleBuffer, gl.mDynamicCommandBuffer,
            gl.mSkinSourceBuffer, gl.mSkinGroupBuffer, gl.mSkinRangeBuffer, gl.mSkinInfluenceBuffer, gl.mSkinTaskBuffer,
            gl.mSkinMatrixBuffer, gl.mRayBuffer, gl.mRayTaskBuffer };
        gl.mExt->glDeleteBuffers(static_cast<GLsizei>(std::size(buffers)), buffers);
        for (const auto& buffer : gl.mReceiverBuffers)
            gl.mExt->glDeleteBuffers(1, &buffer.mBuffer);
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
        gl.deleteVaos();
        if (gl.mPyramid != 0)
            glDeleteTextures(1, &gl.mPyramid);
        gl.mPyramid = 0;
        // Zero capacity lets the next resize allocate.
        gl.mViewCapacity = 0;
        std::lock_guard<std::mutex> lock(mMutex);
        mBatchesDirty = true;
        mTextures->releaseGLObjects(state);
        state->unbindDrawIndirectBufferObject();
        state->unbindVertexArrayObject();
        state->unbindVertexBufferObject();
        state->unbindElementBufferObject();
        state->dirtyAllVertexArrays();
        state->setLastAppliedProgramObject(nullptr);
        state->dirtyAllAttributes();
    }
}
