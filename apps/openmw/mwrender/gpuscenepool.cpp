#include "gpusceneinternal.hpp"

namespace MWRender
{
    GpuScene::Id GpuScene::getMesh(const osg::Geometry& geometry)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const Id existing = findMeshLocked(geometry);
            if (existing != sInvalid)
                return existing;
        }
        // Packing runs outside the scene lock.
        PendingMesh pending;
        if (!packGeometry(geometry, pending.mVertices, pending.mIndices))
            return sInvalid;
        std::lock_guard<std::mutex> lock(mMutex);
        return adoptPackedMeshLocked(geometry, std::move(pending));
    }

    GpuScene::Id GpuScene::findMeshLocked(const osg::Geometry& geometry)
    {
        const auto found = mMeshIds.find(&geometry);
        if (found != mMeshIds.end())
        {
            if (mMeshes[found->second].mGeometry.get() == &geometry)
            {
                mMeshes[found->second].mLast = mLifetimeFrame;
                return found->second;
            }
            // A dead geometry left its key behind.
            mMeshIds.erase(found);
        }
        return sInvalid;
    }

    GpuScene::Id GpuScene::getMeshLocked(const osg::Geometry& geometry, const osg::Geometry* source)
    {
        const Id existing = findMeshLocked(geometry);
        if (existing != sInvalid)
            return existing;

        PendingMesh pending;
        pending.mSkinSource = source != nullptr;
        const osg::Geometry& data = source != nullptr ? *source : geometry;
        if (!packGeometry(data, pending.mVertices, pending.mIndices))
            return sInvalid;
        return insertMeshLocked(geometry, std::move(pending));
    }

    GpuScene::Id GpuScene::insertMeshLocked(const osg::Geometry& geometry, PendingMesh&& pending)
    {
        Id id = sInvalid;
        {
            const std::uint32_t vertexCount = static_cast<std::uint32_t>(pending.mVertices.size() / sizeof(GpuVertex));
            const std::uint32_t indexCount = static_cast<std::uint32_t>(pending.mIndices.size());
            std::size_t budget = mMeshBudgetBytes;
            const std::size_t cap = mMeshCapBytes.load(std::memory_order_relaxed);
            if (cap != 0 && (budget == 0 || cap < budget))
                budget = cap;
            const std::uint32_t limit = budget == 0
                ? sInvalid
                : static_cast<std::uint32_t>(std::min<std::size_t>(budget / sizeof(GpuVertex), sInvalid));
            pending.mVertexOffset = mVertices.allocate(vertexCount, limit);
            if (pending.mVertexOffset == sInvalid
                && evictMeshesLocked(mLifetimeFrame, vertexCount * sizeof(GpuVertex)) != 0)
                pending.mVertexOffset = mVertices.allocate(vertexCount, limit);
            if (pending.mVertexOffset == sInvalid)
            {
                if (!mMeshBudgetWarned)
                    Log(Debug::Warning) << "GPU mesh budget exhausted; new meshes stay legacy.";
                mMeshBudgetWarned = true;
                return sInvalid;
            }
            pending.mIndexOffset = mIndices.allocate(indexCount);
            Mesh mesh{ &geometry, &geometry, pending.mIndexOffset, indexCount,
                static_cast<std::int32_t>(pending.mVertexOffset), vertexCount, 0, mLifetimeFrame };
            if (!mFreeMeshes.empty())
            {
                id = mFreeMeshes.back();
                mFreeMeshes.pop_back();
                mMeshes[id] = std::move(mesh);
            }
            else
            {
                id = static_cast<Id>(mMeshes.size());
                mMeshes.push_back(std::move(mesh));
            }
            pending.mMesh = id;
            mPendingMeshes.push_back(std::move(pending));
            mMeshIds.emplace(&geometry, id);
        }
        return id;
    }

    GpuScene::Id GpuScene::adoptPackedMeshLocked(const osg::Geometry& geometry, PendingMesh&& pending)
    {
        // Another thread may have packed it meanwhile.
        const Id existing = findMeshLocked(geometry);
        if (existing != sInvalid)
            return existing;
        return insertMeshLocked(geometry, std::move(pending));
    }

    void GpuScene::releaseMeshLocked(Id meshId)
    {
        Mesh& mesh = mMeshes[meshId];
        // A zero count is a stale release.
        if (mesh.mUsers == 0 || --mesh.mUsers > 0)
            return;
        dropMeshLocked(meshId);
    }

    void GpuScene::queueMeshRelease(Id mesh)
    {
        std::lock_guard<std::mutex> lock(mReleaseMutex);
        mPendingReleases.push_back(mesh);
    }

    void GpuScene::drainMeshReleasesLocked()
    {
        std::vector<Id> releases;
        {
            std::lock_guard<std::mutex> lock(mReleaseMutex);
            releases.swap(mPendingReleases);
        }
        for (const Id mesh : releases)
            releaseMeshLocked(mesh);
    }

    void GpuScene::dropMeshLocked(Id meshId)
    {
        Mesh& mesh = mMeshes[meshId];
        const std::uint32_t vertex = static_cast<std::uint32_t>(mesh.mBaseVertex);
        std::erase_if(mPendingMeshes, [&](const PendingMesh& pending) { return pending.mMesh == meshId; });
        std::erase_if(mPendingSkins, [&](const PendingSkin& pending) { return pending.mMesh == meshId; });
        mVertices.free(vertex, mesh.mVertexCount);
        mIndices.free(mesh.mFirstIndex, mesh.mIndexCount);
        dropSkinLocked(meshId);
        mMorphVersions.erase(
            mMorphVersions.lower_bound({ meshId, 0 }), mMorphVersions.upper_bound({ meshId, sInvalid }));
        const auto found = mMeshIds.find(mesh.mKey);
        // A newer mesh may hold the key.
        if (found != mMeshIds.end() && found->second == meshId)
        {
            mUpdatedMeshes.erase(mesh.mKey);
            mMeshVersions.erase(mesh.mKey);
            mMeshIds.erase(found);
        }
        mesh.mGeometry = nullptr;
        mesh.mKey = nullptr;
        mesh.mFailed = false;
        mFreeMeshes.push_back(meshId);
    }

    void GpuScene::dropSkinLocked(Id meshId)
    {
        const auto skin = mSkinMeshes.find(meshId);
        if (skin == mSkinMeshes.end())
            return;
        mSkinGroups.free(skin->second.mGroupFirst, skin->second.mGroupCount);
        mSkinInfluences.free(skin->second.mInfluenceFirst, skin->second.mInfluenceCount);
        mSkinMeshes.erase(skin);
    }

    /// Stops draws for a chunk without data.
    void GpuScene::dropChunkDrawsLocked(Id slot, Id generation)
    {
        if (slot >= mChunks.size())
            return;
        Chunk& chunk = mChunks[slot];
        if (chunk.mGeneration != generation)
            return;
        for (const auto& [batch, count] : chunk.mBatchCounts)
            mBatches[batch].mResident -= count;
        chunk.mBatchCounts.clear();
        mBatchesDirty = true;
    }

    /// Frees unreferenced meshes, oldest first.
    std::size_t GpuScene::evictMeshesLocked(unsigned int frame, std::size_t wanted)
    {
        // One full sweep per frame.
        if (mEvictSweepFrame == frame)
            return 0;
        mEvictSweepFrame = frame;
        // In-flight draws may still read them.
        constexpr unsigned int grace = 4;
        std::vector<Id> candidates;
        for (Id id = 0; id < mMeshes.size(); ++id)
        {
            const Mesh& mesh = mMeshes[id];
            // Skinned meshes keep rig data resident.
            if (mesh.mKey != nullptr && mesh.mUsers == 0 && frame > mesh.mLast + grace
                && mSkinMeshes.find(id) == mSkinMeshes.end())
                candidates.push_back(id);
        }
        std::sort(candidates.begin(), candidates.end(),
            [&](Id left, Id right) { return mMeshes[left].mLast < mMeshes[right].mLast; });
        std::size_t freed = 0;
        for (const Id id : candidates)
        {
            if (freed >= wanted)
                break;
            freed += mMeshes[id].mVertexCount * sizeof(GpuVertex);
            dropMeshLocked(id);
            ++mMeshEvictions;
        }
        return freed;
    }

    std::uint32_t GpuScene::RangeAllocator::allocate(std::uint32_t count, std::uint32_t limit)
    {
        for (auto it = mFree.begin(); it != mFree.end(); ++it)
        {
            if (it->second < count)
                continue;
            const std::uint32_t first = it->first;
            if (first > limit || count > limit - first)
                continue;
            const std::uint32_t remaining = it->second - count;
            mFree.erase(it);
            if (remaining > 0)
                mFree.emplace(first + count, remaining);
            mUsed += count;
            return first;
        }
        if (mExtent > limit || count > limit - mExtent)
            return sInvalid;
        const std::uint32_t first = mExtent;
        mExtent += count;
        mUsed += count;
        return first;
    }

    void GpuScene::RangeAllocator::free(std::uint32_t first, std::uint32_t count)
    {
        mUsed -= count;
        auto inserted = mFree.emplace(first, count).first;
        auto next = std::next(inserted);
        if (next != mFree.end() && inserted->first + inserted->second == next->first)
        {
            inserted->second += next->second;
            mFree.erase(next);
        }
        if (inserted != mFree.begin())
        {
            auto prev = std::prev(inserted);
            if (prev->first + prev->second == inserted->first)
            {
                prev->second += inserted->second;
                mFree.erase(inserted);
            }
        }
    }
}
