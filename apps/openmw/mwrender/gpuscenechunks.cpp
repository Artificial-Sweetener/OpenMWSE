#include "gpusceneinternal.hpp"

namespace MWRender
{
    osg::ref_ptr<osg::Node> GpuScene::createChunk(
        std::vector<Instance>&& instances, const osg::BoundingSphere& bound, bool lit, ChunkKind kind)
    {
        if (instances.empty())
            return nullptr;

        // Geometry packing runs outside the scene lock.
        std::vector<const osg::Geometry*> missing;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const Instance& instance : instances)
                if (findMeshLocked(*instance.mGeometry) == sInvalid
                    && std::find(missing.begin(), missing.end(), instance.mGeometry.get()) == missing.end())
                    missing.push_back(instance.mGeometry.get());
        }
        std::vector<std::pair<const osg::Geometry*, PendingMesh>> packed;
        packed.reserve(missing.size());
        for (const osg::Geometry* geometry : missing)
        {
            PendingMesh pending;
            if (packGeometry(*geometry, pending.mVertices, pending.mIndices))
                packed.emplace_back(geometry, std::move(pending));
        }

        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& [geometry, pending] : packed)
            adoptPackedMeshLocked(*geometry, std::move(pending));
        for (const Instance& instance : instances)
            if (getMeshLocked(*instance.mGeometry) == sInvalid || instance.mMaterial >= mMaterialStates.size()
                || mMaterialStates[instance.mMaterial] == MaterialState::Published)
                return nullptr;
        Chunk chunk;
        chunk.mKind = kind;
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
        pending.mChunk = slot;
        pending.mGeneration = chunk.mGeneration;
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
            if (instance.mGroundcoverLod)
            {
                const std::uint64_t triangles = mMeshes[meshId].mIndexCount / 3;
                if (instance.mLodMin > 0.f)
                {
                    ++chunk.mGroundcoverReducedInstances;
                    chunk.mGroundcoverReducedTriangles += triangles;
                }
                else
                {
                    ++chunk.mGroundcoverDetailedInstances;
                    chunk.mGroundcoverDetailedTriangles += triangles;
                }
            }
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
        if (instance.mGroundcoverLod)
            flags |= sFlagGroundcoverLod;
        if (instance.mTerrainExtra)
            flags |= sFlagTerrainExtra;
        if (instance.mOpaqueShadow)
            flags |= sFlagOpaqueShadow;
        flags |= instance.mModes << sFlagModeShift;
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
        pending.mChunk = slot;
        pending.mGeneration = chunk.mGeneration;
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
        mResidentBoundStale = true;
        mBatchesDirty = true;
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
            drainMeshReleasesLocked();
            retireMaterialsLocked(frame);
            mTextures->prepareTiers();
            mTextures->manage(*this, frame);
            // Twin growth may hold four buffers.
            mMeshBudgetBytes = mTextures->mBudgetBytes / 8;
            const std::size_t used = mVertices.mUsed * sizeof(GpuVertex);
            if (mMeshBudgetBytes != 0 && used > mMeshBudgetBytes)
                evictMeshesLocked(frame, used - mMeshBudgetBytes);
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
            if (mResidentBoundStale)
            {
                mResidentBoundStale = false;
                mResidentBound = osg::BoundingSphere();
                for (const Chunk& chunk : mChunks)
                    if (chunk.mGeneration != 0)
                        mResidentBound.expandBy(chunk.mBounds);
                mBoundDirty = true;
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
}
