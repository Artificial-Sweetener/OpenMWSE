#include "gpusceneinternal.hpp"

namespace MWRender
{
    /// One record per cull traversal.
    GpuScene::ViewRecord& GpuScene::cullRecord(osgUtil::CullVisitor& cv, unsigned int frame)
    {
        ViewRecord& view = mViews[ViewKey(frame, cv.getCurrentCamera())];
        const osgUtil::RenderStage* stage = cv.getCurrentRenderStage();
        if (view.mStage != stage)
        {
            view.mStage = stage;
            view.mStaticRangeCount = 0;
            view.mChunks.clear();
            view.mReceivers.clear();
            view.mLightReceivers = nullptr;
            view.mDynamic.clear();
            view.mSkins.clear();
        }
        return view;
    }

    void GpuScene::recordView(osgUtil::CullVisitor& cv)
    {
        if (!isEnabled())
            return;
        // Runs beside dynamic refreshes.
        refreshLiveMaterials(cv);
        const osg::CullingSet& cullingSet = cv.getCurrentCullingSet();
        const osg::Polytope& frustum = cullingSet.getFrustum();
        const osg::Polytope::ClippingMask mask = frustum.getCurrentMask();
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();

        sCullPath.mScene = this;
        sCullPath.mFrame = frame;
        sCullPath.mCamera = cv.getCurrentCamera();
        sCullPath.mStates.clear();
        for (osgUtil::StateGraph* graph = cv.getCurrentStateGraph(); graph != nullptr; graph = graph->_parent)
            if (graph->getStateSet() != nullptr)
                sCullPath.mStates.push_back(graph->getStateSet());

        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = mViews.begin(); it != mViews.end();)
        {
            if (it->first.first + 2 < frame)
                it = mViews.erase(it);
            else
                ++it;
        }
        mDispatchingCameras.insert(cv.getCurrentCamera());
        ViewRecord& view = cullRecord(cv, frame);
        view.mShadow = cv.getCurrentCamera()->getName() == "ShadowCamera";
        view.mPlanes.clear();
        const osg::Polytope::PlaneList& planes = frustum.getPlaneList();
        for (unsigned int i = 0; i < planes.size() && view.mPlanes.size() < sMaxPlanes; ++i)
            if (mask & (1u << i))
                view.mPlanes.push_back(planes[i].asVec4());
        view.mPixelSizeVector = cullingSet.getPixelSizeVector();
        view.mSmallFeaturePixelSize = (cullingSet.getCullingMask() & osg::CullingSet::SMALL_FEATURE_CULLING)
            ? cullingSet.getSmallFeatureCullingPixelSize()
            : 0.f;
        view.mViewPoint = cv.getViewPointLocal();
        view.mLodScale = cv.getLODScale();
        view.mOcclusion = cv.getCurrentCamera() == mPrepassCamera && cv.getProjectionMatrix() != nullptr
            && cv.getModelViewMatrix() != nullptr;
        if (view.mOcclusion)
        {
            view.mViewProj = osg::Matrixf(*cv.getModelViewMatrix() * *cv.getProjectionMatrix());
            if (mLightOcclusion && mLightManager && mLightManager->getClassicLighting())
            {
                view.mLightProjection = osg::Matrixf(*cv.getProjectionMatrix());
                view.mLightReceivers = mLightManager->receiverBuffer(cv);
            }
        }
        view.mHasParams = true;
        const bool nearFar = cv.getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR;
        const osg::Matrix& modelView = *cv.getModelViewMatrix();
        if (cv.getTraversalMask() & (Mask_Static | Mask_Object | Mask_Terrain))
        {
            for (const Resident& resident : mResidents)
            {
                if (cv.isCulled(resident.mBound))
                    continue;
                if (resident.mSlot >= mChunks.size() || mChunks[resident.mSlot].mGeneration != resident.mGeneration)
                    continue;
                const Chunk& chunk = mChunks[resident.mSlot];
                view.mChunks.emplace_back(resident.mSlot, resident.mGeneration);
                if (resident.mLit && mLightManager && mLightManager->getClassicLighting())
                {
                    osg::BoundingSphere bound = resident.mBound;
                    SceneUtil::transformBoundingSphere(modelView, bound);
                    view.mReceivers.push_back({ chunk.mReceiverSlot, mLightManager->addReceiver(cv, bound) });
                }
                for (const Id material : chunk.mMaterials)
                    markMaterialLocked(material, frame);
                if (nearFar && resident.mBound.valid())
                {
                    osg::BoundingBox box;
                    box.expandBy(resident.mBound);
                    cv.updateCalculatedNearFar(modelView, box);
                }
            }
        }
        recordRegisteredRigs(cv, view, nearFar, modelView);
    }

    void GpuScene::recordRegisteredRigs(
        osgUtil::CullVisitor& cv, ViewRecord& view, bool nearFar, const osg::Matrix& modelView)
    {
        if (!isEnabled() || !mSkinSupported.load(std::memory_order_relaxed))
            return;
        if (!(cv.getTraversalMask() & (Mask_Actor | Mask_Player)))
            return;
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        std::size_t first = 0;
        while (first < mRigRegistry.size())
        {
            SceneUtil::PositionAttitudeTransform& actor = *mRigRegistry[first].mActor;
            std::size_t last = first + 1;
            while (last < mRigRegistry.size() && mRigRegistry[last].mActor == &actor)
                ++last;
            if (!(actor.getNodeMask() & cv.getTraversalMask()) || cv.isCulled(actor.getBound()))
            {
                first = last;
                continue;
            }

            osg::Matrix matrix;
            actor.computeLocalToWorldMatrix(matrix, nullptr);
            const osg::Matrixf world(matrix);
            std::uint32_t receiver = 0;
            if (mLightManager && mLightManager->getClassicLighting())
            {
                const RigEntry& entry = mRigRegistry[first];
                osg::BoundingSphere bound = entry.mLightNode->getBound();
                SceneUtil::transformBoundingSphere(world * modelView, bound);
                receiver = mLightManager->addReceiver(
                    cv, bound, entry.mLightList ? &entry.mLightList->getIgnoredLightSources() : nullptr);
            }
            const auto batches = mActorBatches.find(&actor);
            if (batches != mActorBatches.end())
            {
                for (const auto& batch : batches->second)
                    recordActorBatch(cv, view, batch, world, receiver);
                if (nearFar)
                {
                    osg::BoundingBox bound;
                    bound.expandBy(actor.getBound());
                    cv.updateCalculatedNearFar(modelView, bound);
                }
            }
            for (std::size_t i = first; i < last; ++i)
            {
                const RigEntry& entry = mRigRegistry[i];
                if (entry.mBatched)
                    continue;
                SceneUtil::RigGeometry& rig = *entry.mRig;
                if (rig.getNodeMask() != Mask_GpuProxy)
                    continue;
                unsigned int captureFrame = 0;
                osg::BoundingBox local;
                if (!rig.getLatestBound(captureFrame, local))
                    continue;
                osg::BoundingBox bound = worldBound(local, world);
                if (cv.isCulled(bound))
                    continue;

                SkinUpdate skin;
                if (!getSkinPose(frame, rig, skin.mPose, captureFrame))
                    continue;
                if (skin.mPose.mSnapshot != captureFrame)
                {
                    if (!rig.getBound(skin.mPose.mSnapshot, local))
                        continue;
                    bound = worldBound(local, world);
                    if (cv.isCulled(bound))
                        continue;
                }

                DynamicInstance instance;
                instance.mWorld = world;
                instance.mMesh = entry.mMesh;
                instance.mBucket = entry.mSurface.mBucket;
                instance.mMaterial = entry.mSurface.mMaterial;
                instance.mReceiver = receiver;
                view.mDynamic.push_back(std::move(instance));
                markMaterialLocked(entry.mSurface.mMaterial, frame);
                skin.mMesh = entry.mMesh;
                view.mSkins.push_back(skin);
                if (nearFar && bound.valid())
                    cv.updateCalculatedNearFar(modelView, bound);
            }
            first = last;
        }
    }

    void GpuScene::recordActorBatch(osgUtil::CullVisitor& cv, ViewRecord& view,
        const std::shared_ptr<ActorBatch>& batch, const osg::Matrixf& world, std::uint32_t receiver)
    {
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        SkinUpdate skin;
        skin.mMesh = batch->mMesh;
        skin.mBatch = batch;
        if (!getBatchPose(frame, *batch, skin.mPose))
            return;
        for (const ActorPart& part : batch->mParts)
        {
            if (part.mMorph != nullptr)
            {
                SkinUpdate morph;
                morph.mMesh = batch->mMesh;
                morph.mVertex = part.mVertex;
                morph.mMaterial = part.mMaterial;
                morph.mPalette = part.mPalette;
                morph.mMorph = &part.mMorph->morph(cv.getTraversalNumber());
                view.mSkins.push_back(std::move(morph));
            }
            markMaterialLocked(part.mMaterial, frame);
        }
        view.mSkins.push_back(skin);
        DynamicInstance instance;
        instance.mBatch = batch;
        instance.mWorld = world;
        instance.mMesh = batch->mMesh;
        instance.mBucket = batch->mBucket;
        instance.mReceiver = receiver;
        view.mDynamic.push_back(std::move(instance));
    }

    void GpuScene::addResident(osg::Node& node)
    {
        auto* chunk = dynamic_cast<GpuChunkNode*>(&node);
        if (chunk == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        chunk->mResident = mResidents.size();
        mResidents.push_back(Resident{ chunk->getSlot(), chunk->getGeneration(), chunk->getBound(), chunk->isLit() });
        mResidentNodes.push_back(chunk);
    }

    void GpuScene::removeResident(osg::Node& node)
    {
        auto* chunk = dynamic_cast<GpuChunkNode*>(&node);
        if (chunk == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        removeResidentLocked(*chunk);
    }

    void GpuScene::removeResidentLocked(GpuChunkNode& chunk)
    {
        if (chunk.mResident == GpuChunkNode::sNone)
            return;
        const std::size_t index = chunk.mResident;
        mResidents[index] = mResidents.back();
        mResidentNodes[index] = mResidentNodes.back();
        mResidentNodes[index]->mResident = index;
        mResidents.pop_back();
        mResidentNodes.pop_back();
        chunk.mResident = GpuChunkNode::sNone;
    }

    bool GpuScene::recordRig(osgUtil::CullVisitor& cv, SceneUtil::RigGeometry& rig, const GpuCaptureCallback& callback)
    {
        if (!mSkinSupported.load(std::memory_order_relaxed))
            return false;
        osg::Geometry* geometry = rig.getGpuGeometry();
        if (geometry == nullptr)
            return false;
        SkinUpdate skin;
        if (!getSkinPose(cv.getTraversalNumber(), rig, skin.mPose))
            return false;
        return record(cv, *geometry, false, callback, &skin);
    }

    bool GpuScene::record(osgUtil::CullVisitor& cv, osg::Geometry& geometry, bool updateMesh,
        const GpuCaptureCallback& callback, SkinUpdate* skin)
    {
        static const std::string sLightCluster = "lightCluster";
        thread_local std::vector<const osg::StateSet*> statePath;
        statePath.clear();
        const bool hasBase = sCullPath.mScene == this && sCullPath.mFrame == cv.getFrameStamp()->getFrameNumber()
            && sCullPath.mCamera == cv.getCurrentCamera();
        for (osgUtil::StateGraph* graph = cv.getCurrentStateGraph(); graph != nullptr; graph = graph->_parent)
        {
            const osg::StateSet* stateSet = graph->getStateSet();
            if (stateSet == nullptr
                || (hasBase
                    && std::find(sCullPath.mStates.begin(), sCullPath.mStates.end(), stateSet)
                        != sCullPath.mStates.end())
                || stateSet->getUniform(sLightCluster) != nullptr)
                continue;
            statePath.push_back(stateSet);
        }
        std::reverse(statePath.begin(), statePath.end());
        if (orderDependentPath(statePath))
            return false;
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        const bool liveMaterial = callback.hasLiveMaterial(cv);

        const osg::RefMatrix* viewMatrix = cv.getCurrentRenderStage()->getInitialViewMatrix();
        osg::Matrixf world(*cv.getModelViewMatrix());
        if (viewMatrix != nullptr)
            world *= osg::Matrixf::inverse(osg::Matrixf(*viewMatrix));
        DynamicInstance instance;
        if (updateMesh)
            instance.mGeometry = &geometry;
        instance.mWorld = world;
        instance.mUpdateMesh = updateMesh;
        if (mLightManager && mLightManager->getClassicLighting())
            instance.mReceiver = mLightManager->currentReceiver();

        std::lock_guard<std::mutex> lock(mMutex);
        // Only dispatching cameras consume captures.
        if (mDispatchingCameras.find(cv.getCurrentCamera()) == mDispatchingCameras.end())
            return false;
        const auto [mesh, surface] = callback.resolveLocked(geometry, statePath, frame, liveMaterial);
        if (mesh == sInvalid || surface.mBucket == sInvalid)
            return false;
        instance.mMesh = mesh;
        instance.mBucket = surface.mBucket;
        instance.mMaterial = surface.mMaterial;
        markMaterialLocked(surface.mMaterial, frame);
        ViewRecord& view = cullRecord(cv, frame);
        view.mDynamic.push_back(std::move(instance));
        if (skin != nullptr && skin->mPose.mCount != 0)
        {
            skin->mMesh = mesh;
            view.mSkins.push_back(*skin);
        }
        return true;
    }

    void GpuScene::markChunk(osgUtil::CullVisitor& cv, Id slot, Id generation, bool lit)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const unsigned int frame = cv.getFrameStamp()->getFrameNumber();
        ViewRecord& view = cullRecord(cv, frame);
        view.mChunks.emplace_back(slot, generation);
        if (slot >= mChunks.size() || mChunks[slot].mGeneration != generation)
            return;
        const Chunk& chunk = mChunks[slot];
        if (lit && mLightManager && mLightManager->getClassicLighting())
            view.mReceivers.push_back({ chunk.mReceiverSlot, mLightManager->currentReceiver() });
        for (const Id material : chunk.mMaterials)
            markMaterialLocked(material, frame);
    }
}
