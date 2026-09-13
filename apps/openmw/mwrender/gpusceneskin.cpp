#include "gpusceneinternal.hpp"

namespace MWRender
{
    void GpuScene::removeActor(SceneUtil::PositionAttitudeTransform& actor)
    {
        std::vector<RayRequest> failed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            removeActorLocked(actor);
            if (mRigRegistry.empty())
            {
                failed.swap(mPendingRays);
                mRayInvalidate = false;
            }
        }
        if (!failed.empty())
        {
            RayBatch batch;
            batch.mRequests = std::move(failed);
            finishRay(mRayState, batch, nullptr, false);
        }
    }

    void GpuScene::removeActorLocked(SceneUtil::PositionAttitudeTransform& actor, std::vector<Id>* retired)
    {
        mActorRigPopulations.erase(&actor);
        const auto batches = mActorBatches.find(&actor);
        if (batches != mActorBatches.end())
        {
            for (const auto& batch : batches->second)
            {
                releaseMeshLocked(batch->mMesh);
                for (const ActorPart& part : batch->mParts)
                {
                    part.mDrawable->setNodeMask(~0u);
                    if (part.mMesh != sInvalid)
                    {
                        if (retired != nullptr)
                            retired->push_back(part.mMesh);
                        else
                            releaseMeshLocked(part.mMesh);
                    }
                    dropMaterialLocked(part.mMaterial);
                }
            }
            mActorBatches.erase(batches);
        }
        const auto found = std::find_if(
            mRigRegistry.begin(), mRigRegistry.end(), [&](const RigEntry& entry) { return entry.mActor == &actor; });
        if (found == mRigRegistry.end())
            return;
        for (const RigEntry& entry : mRigRegistry)
            if (entry.mActor == &actor)
            {
                entry.mRig->setCpuSkinning(true);
                if (retired != nullptr)
                    retired->push_back(entry.mMesh);
                else
                    releaseMeshLocked(entry.mMesh);
                dropMaterialLocked(entry.mSurface.mMaterial);
            }
        restoreProxy(actor);
        mRigRegistry.erase(std::remove_if(mRigRegistry.begin(), mRigRegistry.end(),
                               [&](const RigEntry& entry) { return entry.mActor == &actor; }),
            mRigRegistry.end());
        if (!mPendingRays.empty())
            mRayInvalidate = true;
        updateRigCullLocked();
    }

    void GpuScene::restoreRigsLocked()
    {
        SceneUtil::PositionAttitudeTransform* actor = nullptr;
        for (const RigEntry& entry : mRigRegistry)
        {
            entry.mRig->setCpuSkinning(true);
            if (entry.mActor == actor)
                continue;
            actor = entry.mActor;
            restoreProxy(*actor);
        }
    }

    void GpuScene::updateRigCullLocked()
    {
        if (mNode != nullptr)
            mNode->setCullingActive(!isEnabled() || mRigRegistry.empty());
    }

    bool GpuScene::registerSkinLocked(Id mesh, const SceneUtil::RigGeometry& rig)
    {
        const std::vector<std::uint32_t>& groups = rig.getSkinGroups();
        if (groups.size() != mMeshes[mesh].mVertexCount)
            return false;
        const auto found = mSkinMeshes.find(mesh);
        if (found != mSkinMeshes.end())
            return true;

        std::vector<SceneUtil::RigGeometry::SkinRange> ranges;
        std::vector<SceneUtil::RigGeometry::SkinWeight> influences;
        const std::size_t boneCount = rig.getSkinData(ranges, influences);
        if (boneCount == 0 || boneCount > std::numeric_limits<std::uint32_t>::max()
            || influences.size() > std::numeric_limits<std::uint32_t>::max())
            return false;
        std::uint32_t count = 0;
        for (const std::uint32_t group : groups)
        {
            if (group == std::numeric_limits<std::uint32_t>::max())
                continue;
            count = std::max(count, group + 1);
        }
        if (count == 0 || count != ranges.size())
            return false;
        for (const SceneUtil::RigGeometry::SkinRange& range : ranges)
            if (static_cast<std::uint64_t>(range.mFirst) + range.mCount > influences.size())
                return false;
        for (const SceneUtil::RigGeometry::SkinWeight& influence : influences)
            if (influence.mBone >= boneCount)
                return false;

        const osg::ref_ptr<osg::Geometry> vertexSource = rig.getSourceGeometry();
        const auto queued = std::find_if(
            mPendingMeshes.begin(), mPendingMeshes.end(), [&](const PendingMesh& item) { return item.mMesh == mesh; });
        if (queued != mPendingMeshes.end())
            queued->mSkinSource = true;
        else if (vertexSource == nullptr || !queueSkinSource(mesh, *vertexSource))
            return false;

        PendingSkin pending;
        pending.mMesh = mesh;
        pending.mFirst = static_cast<std::uint32_t>(mMeshes[mesh].mBaseVertex);
        pending.mGroups = groups;
        pending.mGroupFirst = mSkinGroups.allocate(count);
        pending.mInfluenceFirst = mSkinInfluences.allocate(static_cast<std::uint32_t>(influences.size()));
        for (std::uint32_t& group : pending.mGroups)
            if (group != std::numeric_limits<std::uint32_t>::max())
                group += pending.mGroupFirst;
        pending.mGroupRanges.reserve(ranges.size());
        for (const SceneUtil::RigGeometry::SkinRange& range : ranges)
            pending.mGroupRanges.push_back({ pending.mInfluenceFirst + range.mFirst, range.mCount });
        pending.mInfluences.reserve(influences.size());
        for (const SceneUtil::RigGeometry::SkinWeight& influence : influences)
            pending.mInfluences.push_back({ influence.mBone, influence.mWeight });

        const SkinMesh skin{ pending.mGroupFirst, count, pending.mInfluenceFirst,
            static_cast<std::uint32_t>(pending.mInfluences.size()), static_cast<std::uint32_t>(boneCount) };
        mSkinMeshes.emplace(mesh, skin);
        mPendingSkins.push_back(std::move(pending));
        return true;
    }

    /// Copies bind pose vertices for skinning.
    bool GpuScene::queueSkinSource(Id meshId, const osg::Geometry& source)
    {
        const Mesh& mesh = mMeshes[meshId];
        if (mesh.mBaseVertex < 0)
            return false;
        PendingMesh pending;
        std::vector<std::uint32_t> indices;
        if (!packGeometry(source, pending.mVertices, indices)
            || pending.mVertices.size() != mesh.mVertexCount * sizeof(GpuVertex))
            return false;
        pending.mMesh = meshId;
        pending.mVertexOffset = static_cast<std::uint32_t>(mesh.mBaseVertex);
        pending.mIndexOffset = mesh.mFirstIndex;
        pending.mSkinSource = true;
        // The first dispatch needs this copy.
        mPendingMeshes.push_front(std::move(pending));
        return true;
    }

    bool GpuScene::getSkinPose(unsigned int frame, SceneUtil::RigGeometry& rig, SkinPose& out, unsigned int snapshot)
    {
        osg::Geometry* geometry = rig.getGpuGeometry();
        if (geometry == nullptr)
            return false;
        {
            SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
            std::lock_guard<std::mutex> lock(slot.mMutex);
            if (slot.mValid && slot.mFrame == frame)
            {
                const auto found = slot.mPoses.find(geometry);
                if (found != slot.mPoses.end())
                {
                    out = found->second;
                    return true;
                }
            }
        }

        thread_local std::vector<osg::Matrixf> matrices;
        matrices.clear();
        unsigned int paletteFrame = 0;
        if (snapshot == sInvalid)
        {
            if (!rig.getLatestPalette(paletteFrame, matrices))
                return false;
        }
        else
        {
            paletteFrame = snapshot;
            if (!rig.getPalette(snapshot, matrices))
                return false;
        }
        thread_local std::vector<GpuSkinMatrix> pose;
        pose.resize(matrices.size());
        for (std::size_t i = 0; i < matrices.size(); ++i)
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 4; ++row)
                    pose[i].mCols[col][row] = matrices[i](row, col);

        SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
        std::lock_guard<std::mutex> lock(slot.mMutex);
        if (!slot.mValid || slot.mFrame != frame)
        {
            slot.mFrame = frame;
            slot.mValid = true;
            slot.mPoses.clear();
            slot.mMatrices.clear();
        }
        const auto found = slot.mPoses.find(geometry);
        if (found != slot.mPoses.end())
            out = found->second;
        else
        {
            out = { frame, paletteFrame, static_cast<std::uint32_t>(slot.mMatrices.size()),
                static_cast<std::uint32_t>(pose.size()) };
            slot.mMatrices.insert(slot.mMatrices.end(), pose.begin(), pose.end());
            slot.mPoses.emplace(geometry, out);
        }
        return true;
    }

    bool GpuScene::getBatchPose(unsigned int frame, const ActorBatch& batch, SkinPose& out)
    {
        SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
        std::lock_guard<std::mutex> lock(slot.mMutex);
        if (!slot.mValid || slot.mFrame != frame)
        {
            slot.mFrame = frame;
            slot.mValid = true;
            slot.mPoses.clear();
            slot.mMatrices.clear();
        }
        const auto found = slot.mPoses.find(&batch);
        if (found != slot.mPoses.end())
        {
            out = found->second;
            return true;
        }
        const auto first = static_cast<std::uint32_t>(slot.mMatrices.size());
        const auto append = [&](const osg::Matrixf& matrix) {
            GpuSkinMatrix packed;
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 4; ++row)
                    packed.mCols[col][row] = matrix(row, col);
            slot.mMatrices.push_back(packed);
        };
        thread_local std::vector<osg::Matrixf> palette;
        for (const ActorPart& part : batch.mParts)
        {
            if (part.mRig == nullptr)
                continue;
            unsigned int captured;
            if (!part.mRig->getLatestPalette(captured, palette) || palette.size() != part.mMatrixCount)
            {
                slot.mMatrices.resize(first);
                return false;
            }
            for (const osg::Matrixf& matrix : palette)
                append(matrix);
        }
        thread_local std::vector<osg::Matrix> matrices;
        matrices.resize(batch.mTransforms.size());
        for (std::size_t i = 0; i < batch.mTransforms.size(); ++i)
        {
            const ActorBatch::Transform& transform = batch.mTransforms[i];
            matrices[i] = transform.mParent != sInvalid ? matrices[transform.mParent] : osg::Matrix::identity();
            transform.mNode->computeLocalToWorldMatrix(matrices[i], nullptr);
            append(matrices[i]);
        }
        append(osg::Matrixf::identity());
        out = { frame, frame, first, batch.mMatrixCount };
        slot.mPoses.emplace(&batch, out);
        return true;
    }

    bool GpuScene::runSkin(osg::State& state, const std::vector<GpuSkinTask>& tasks,
        const std::vector<GpuSkinMatrix>& matrices, std::uint32_t maxVertices)
    {
        if (tasks.empty())
            return true;
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;
        if (gl.mSkinProgram == nullptr)
        {
            gl.mSkinProgram = new osg::Program;
            gl.mSkinProgram->addShader(mShaderManager.getShader("gpuscene_skin.comp"));
        }
        state.applyAttribute(gl.mSkinProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mSkinProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;

        gl.stream(gl.mSkinMatrixBuffer, matrices.data(), matrices.size() * sizeof(GpuSkinMatrix));
        gl.stream(gl.mSkinTaskBuffer, tasks.data(), tasks.size() * sizeof(GpuSkinTask));
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, gl.mSkinSourceBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, gl.mVertexBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, gl.mSkinGroupBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, gl.mSkinTaskBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, gl.mSkinMatrixBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, gl.mSkinRangeBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, gl.mSkinInfluenceBuffer);
        ext.glDispatchCompute(
            (maxVertices + sSkinGroupSize - 1) / sSkinGroupSize, static_cast<GLuint>(tasks.size()), 1);
        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        return true;
    }

    bool GpuScene::applySkin(osg::State& state, const std::vector<SkinUpdate>& skins)
    {
        if (skins.empty())
            return true;

        thread_local std::vector<GpuSkinTask> tasks;
        thread_local std::vector<GpuSkinMatrix> matrices;
        thread_local std::vector<SkinPose> poses;
        tasks.clear();
        matrices.clear();
        poses.clear();
        std::uint32_t maxVertices = 0;
        std::size_t matrixCount = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const SkinUpdate& skin : skins)
            {
                if (skin.mMesh >= mMeshes.size())
                    continue;
                const Mesh& mesh = mMeshes[skin.mMesh];
                const auto registered = mSkinMeshes.find(skin.mMesh);
                const std::uint64_t last = static_cast<std::uint64_t>(mesh.mBaseVertex) + mesh.mVertexCount;
                osg::ref_ptr<const osg::Geometry> geometry;
                if (mesh.mBaseVertex < 0 || mesh.mFailed || last > mVertices.mExtent || !mesh.mGeometry.lock(geometry))
                    continue;
                if (skin.mMorph != nullptr)
                {
                    const osg::Array* vertices = skin.mMorph->getVertexArray();
                    const auto value = std::make_pair(vertices, vertices->getModifiedCount());
                    const auto [it, inserted]
                        = mMorphVersions.try_emplace(std::make_pair(skin.mMesh, skin.mVertex), value);
                    if (inserted || it->second != value)
                    {
                        it->second = value;
                        std::vector<std::uint8_t> bytes;
                        std::vector<std::uint32_t> indices;
                        if (!packGeometry(*skin.mMorph, bytes, indices))
                            continue;
                        auto* packed = reinterpret_cast<GpuVertex*>(bytes.data());
                        for (std::size_t i = 0; i < bytes.size() / sizeof(GpuVertex); ++i)
                        {
                            packed[i].mMaterial = skin.mMaterial;
                            packed[i].mPalette = skin.mPalette;
                            packed[i].mGroup = sInvalid;
                        }
                        mGl->upload(mGl->mSkinSourceBuffer, (mesh.mBaseVertex + skin.mVertex) * sizeof(GpuVertex),
                            bytes.data(), bytes.size());
                    }
                    continue;
                }
                if (skin.mPose.mCount == 0
                    || (!skin.mBatch
                        && (registered == mSkinMeshes.end() || registered->second.mBoneCount + 1 != skin.mPose.mCount))
                    || !mUpdatedMeshes.insert(geometry.get()).second)
                    continue;
                unsigned int flags = 0;
                if (geometry->getNormalArray() != nullptr)
                    flags |= 1;
                if (dynamic_cast<const osg::Vec4Array*>(geometry->getTexCoordArray(7)) != nullptr)
                    flags |= 2;
                const GpuSkinTask task{ static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount,
                    static_cast<std::uint32_t>(matrixCount), flags | (skin.mBatch ? 4u : 0u) };
                tasks.push_back(task);
                poses.push_back(skin.mPose);
                matrixCount += skin.mPose.mCount;
                maxVertices = std::max(maxVertices, mesh.mVertexCount);
            }
        }
        if (tasks.empty())
            return true;
        matrices.reserve(matrixCount);
        const unsigned int frame = poses.front().mFrame;
        SkinPoseFrame& slot = mSkinPoseFrames[frame % mSkinPoseFrames.size()];
        {
            std::lock_guard<std::mutex> lock(slot.mMutex);
            // Recycled poses skip deformation.
            if (!slot.mValid || slot.mFrame != frame)
                return true;
            for (const SkinPose& pose : poses)
            {
                const std::uint64_t last = static_cast<std::uint64_t>(pose.mFirst) + pose.mCount;
                if (pose.mFrame != frame || last > slot.mMatrices.size())
                    return true;
                matrices.insert(matrices.end(), slot.mMatrices.begin() + pose.mFirst, slot.mMatrices.begin() + last);
            }
        }

        return runSkin(state, tasks, matrices, maxVertices);
    }
}
