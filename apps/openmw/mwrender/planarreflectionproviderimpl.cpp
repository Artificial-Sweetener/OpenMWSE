#include "planarreflectionproviderimpl.hpp"

#include <algorithm>
#include <optional>

#include <osg/Group>
#include <osg/Texture>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/multiview.hpp>

#include "planarreflectioncapture.hpp"
#include "planarreflectionreconstruction.hpp"

namespace MWRender
{
    namespace
    {
        constexpr float unionSourceProjectionScale = 0.8f;
    }

    PlanarReflectionReceiverImpl::PlanarReflectionReceiverImpl(PlanarReflectionProviderImpl& provider,
        const SceneUtil::WaterSurfacePlane& plane, unsigned int resolution, bool visibilityDriven, bool primary)
        : mProvider(provider)
        , mPlane(plane)
        , mResolution(resolution)
        , mVisibilityDriven(visibilityDriven)
        , mPrimary(primary)
    {
    }

    PlanarReflectionProviderImpl::PlanarReflectionProviderImpl(osg::Group& parent, osg::Group& sceneRoot,
        Resource::ResourceSystem& resourceSystem, unsigned int sourceResolution, bool interior,
        PlanarReflectionMode initialMode)
        : mParent(parent)
        , mSceneRoot(sceneRoot)
        , mResourceSystem(resourceSystem)
        , mInterior(interior)
    {
        const auto defines = resourceSystem.getSceneManager()->getShaderManager().getGlobalDefines();
        const auto gpuShader4 = defines.find("useGPUShader4");
        mSupportsShared = gpuShader4 != defines.end() && gpuShader4->second == "1" && !Stereo::getMultiview();
        if (initialMode == PlanarReflectionMode::SharedCapture && mSupportsShared)
            mPublishedMode = mRequestedMode = PlanarReflectionMode::SharedCapture;
        else if (initialMode == PlanarReflectionMode::SharedCapture)
            Log(Debug::Warning)
                << "Shared planar reflections require GPU shader4 and non-multiview rendering; starting in "
                   "Direct Reference mode";
        for (unsigned int lane = 0; lane < mSources.size(); ++lane)
        {
            for (unsigned int layer = 0; layer < mSources[lane].size(); ++layer)
            {
                mSources[lane][layer] = new ExactPlanarReflectionCapture(sourceResolution, sourceResolution, interior,
                    false, lane * sSourceLayerCount + layer, true,
                    layer == sUnionSourceLayer ? unionSourceProjectionScale : 1.f);
                mSources[lane][layer]->setScene(&sceneRoot);
                mSources[lane][layer]->setEnabled(false);
                mParent.addChild(mSources[lane][layer]);
            }
        }
    }

    PlanarReflectionProviderImpl::~PlanarReflectionProviderImpl()
    {
        for (PlanarReflectionReceiver* receiver : mReceivers)
            detach(*receiver->mImpl);
        mReceivers.clear();
        for (const auto& lane : mSources)
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                mParent.removeChild(source);
    }

    void PlanarReflectionProviderImpl::attach(PlanarReflectionReceiver& receiver)
    {
        auto& impl = *receiver.mImpl;
        impl.mId = mNextReceiverId++;
        impl.mDirect = new ExactPlanarReflectionCapture(
            impl.mResolution, impl.mResolution, mInterior, impl.mVisibilityDriven, 0, false, 1.f);
        impl.mDirect->setPlane(impl.mPlane);
        impl.mDirect->setScene(&mSceneRoot);
        impl.mDirect->showWorld(mShowWorld);
        for (unsigned int lane = 0; lane < impl.mShared.size(); ++lane)
        {
            impl.mShared[lane] = new SharedPlanarReflectionTarget(impl.mResolution, *mSources[lane][sExactSourceLayer],
                *mSources[lane][sUnionSourceLayer], mSceneRoot, mResourceSystem.getSceneManager()->getShaderManager(),
                impl.mVisibilityDriven);
            impl.mShared[lane]->setPlane(impl.mPlane);
            if (mCullCallback)
                impl.mShared[lane]->addCullCallback(mCullCallback);
            mParent.addChild(impl.mShared[lane]);
        }
        if (mCullCallback)
            impl.mDirect->addCullCallback(mCullCallback);
        mParent.addChild(impl.mDirect);
        mReceivers.push_back(&receiver);
        updateSourceClipPlane();
        if (mReceivers.size() == 1 || impl.mPrimary)
        {
            mAnchorId = impl.mId;
            mLaneAnchorIds[mActiveLane] = impl.mId;
            configureSource(mActiveLane, impl);
        }
        for (const auto& lane : mSources)
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                source->setSourceDiagnostics(mReceivers.size(), mAnchorId, mAnchorChanges, mAnchorImpact, mComparisonId,
                    mComparisonImpact, static_cast<unsigned int>(mAnchorHandoff), mPreviousTextureWeight);
        applyWorkState();
    }

    void PlanarReflectionProviderImpl::detach(PlanarReflectionReceiverImpl& impl)
    {
        if (impl.mReleased)
            return;
        impl.mReleased = true;
        if (mCullCallback)
        {
            impl.mDirect->removeCullCallback(mCullCallback);
            for (const osg::ref_ptr<SharedPlanarReflectionTarget>& shared : impl.mShared)
                shared->removeCullCallback(mCullCallback);
        }
        mParent.removeChild(impl.mDirect);
        for (const osg::ref_ptr<SharedPlanarReflectionTarget>& shared : impl.mShared)
            mParent.removeChild(shared);
        impl.mDirect->setScene(nullptr);
        impl.mDirect = nullptr;
        impl.mShared = {};
        updateSourceClipPlane();
        for (const auto& lane : mSources)
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                source->setSourceDiagnostics(mReceivers.size() - 1, mAnchorId, mAnchorChanges, mAnchorImpact,
                    mComparisonId, mComparisonImpact, static_cast<unsigned int>(mAnchorHandoff),
                    mPreviousTextureWeight);
    }

    bool PlanarReflectionProviderImpl::recentlyVisible(
        const PlanarReflectionReceiverImpl& receiver, unsigned int latestFrame) const
    {
        return !receiver.mVisibilityDriven || latestFrame - receiver.mVisibleFrame <= 1;
    }

    void PlanarReflectionProviderImpl::configureSource(unsigned int lane, PlanarReflectionReceiverImpl& anchor)
    {
        for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : mSources[lane])
        {
            source->setVisibilityDriven(anchor.mVisibilityDriven);
            source->setPlane(anchor.mPlane);
            source->markVisible(anchor.mVisibleFrame);
        }
        mSources[lane][sExactSourceLayer]->setSharedClipPlane(anchor.mPlane);
        const auto comparison = std::find_if(mReceivers.begin(), mReceivers.end(),
            [&](const auto* receiver) { return receiver->mImpl->mId == mComparisonId; });
        mSources[lane][sUnionSourceLayer]->setSharedClipPlane(
            comparison != mReceivers.end() ? std::optional((*comparison)->mImpl->mPlane) : mUnionClipPlane);
    }

    void PlanarReflectionProviderImpl::updateAnchorPlane(PlanarReflectionReceiverImpl& anchor)
    {
        for (unsigned int lane = 0; lane < mLaneAnchorIds.size(); ++lane)
        {
            const bool frozenPriorLane
                = mAnchorHandoff == PlanarReflectionAnchorHandoff::Blending && lane == mActiveLane;
            if (mLaneAnchorIds[lane] == anchor.mId && !frozenPriorLane)
                configureSource(lane, anchor);
        }
    }

    void PlanarReflectionProviderImpl::updateSourceClipPlane()
    {
        std::optional<SceneUtil::WaterSurfacePlane> unionPlane;
        constexpr float parallelTolerance = 0.00001f;
        for (const PlanarReflectionReceiver* receiver : mReceivers)
        {
            const auto& impl = *receiver->mImpl;
            if (impl.mReleased)
                continue;
            if (!unionPlane)
            {
                unionPlane = impl.mPlane;
                continue;
            }
            if (unionPlane->mNormal * impl.mPlane.mNormal < 1.f - parallelTolerance)
            {
                unionPlane.reset();
                break;
            }
            if (impl.mPlane.mDistance < unionPlane->mDistance)
                unionPlane = impl.mPlane;
        }
        mUnionClipPlane = unionPlane;
        for (auto& lane : mSources)
            lane[sUnionSourceLayer]->setSharedClipPlane(mUnionClipPlane);
    }

    bool PlanarReflectionProviderImpl::unionLayerRequired(unsigned int lane) const
    {
        return !mSources[lane][sUnionSourceLayer]->capturesExactPlane(mSources[lane][sExactSourceLayer]->plane());
    }

    bool PlanarReflectionProviderImpl::anchorHandoffReady(unsigned int latestFrame) const
    {
        if (!mSources[mStagingLane][sExactSourceLayer]->completed()
            || (unionLayerRequired(mStagingLane) && !mSources[mStagingLane][sUnionSourceLayer]->completed()))
            return false;
        for (const PlanarReflectionReceiver* receiver : mReceivers)
        {
            const auto& impl = *receiver->mImpl;
            if (!recentlyVisible(impl, latestFrame))
                continue;
            if (!impl.mShared[mStagingLane]->completed())
                return false;
        }
        return true;
    }

    osg::Texture* PlanarReflectionProviderImpl::laneTexture(
        PlanarReflectionReceiverImpl& receiver, unsigned int lane, osgUtil::CullVisitor& visitor) const
    {
        return receiver.mShared[lane]->getColorTexture(&visitor);
    }

    void PlanarReflectionProviderImpl::applyWorkState()
    {
        const bool transitioningMode = mRequestedMode != mPublishedMode;
        const bool direct = mEnabled
            && (mPublishedMode == PlanarReflectionMode::DirectReference
                || (transitioningMode && mRequestedMode == PlanarReflectionMode::DirectReference));
        const bool shared = mEnabled && mSupportsShared
            && (mPublishedMode == PlanarReflectionMode::SharedCapture
                || (transitioningMode && mRequestedMode == PlanarReflectionMode::SharedCapture));
        const bool handoff = mAnchorHandoff != PlanarReflectionAnchorHandoff::Idle;
        const unsigned int sampledLane
            = mAnchorHandoff == PlanarReflectionAnchorHandoff::Blending ? mStagingLane : mActiveLane;
        for (unsigned int lane = 0; lane < mSources.size(); ++lane)
        {
            const bool currentLane = lane == mActiveLane;
            const bool nextLane = lane == mStagingLane;
            const bool renderSource = shared
                && ((currentLane && mAnchorHandoff != PlanarReflectionAnchorHandoff::Blending)
                    || (nextLane && handoff));
            mSources[lane][sExactSourceLayer]->setEnabled(renderSource);
            mSources[lane][sUnionSourceLayer]->setEnabled(renderSource && unionLayerRequired(lane));
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : mSources[lane])
                source->setDiagnosticDominant(false);
        }
        for (PlanarReflectionReceiver* receiver : mReceivers)
        {
            auto& impl = *receiver->mImpl;
            impl.mDirect->setEnabled(direct);
            impl.mDirect->setDiagnosticDominant(
                mEnabled && mPublishedMode == PlanarReflectionMode::DirectReference && impl.mId == mAnchorId);
            impl.mDirect->setDiagnosticComparison(
                mEnabled && mPublishedMode == PlanarReflectionMode::DirectReference && impl.mId == mComparisonId);
            for (unsigned int lane = 0; lane < impl.mShared.size(); ++lane)
            {
                const bool currentLane = lane == mActiveLane;
                const bool nextLane = lane == mStagingLane;
                const bool renderTarget = shared && (currentLane || (nextLane && handoff));
                const bool sourceUpdates = shared
                    && ((currentLane && mAnchorHandoff != PlanarReflectionAnchorHandoff::Blending)
                        || (nextLane && handoff));
                impl.mShared[lane]->setEnabled(renderTarget);
                impl.mShared[lane]->setSourceUpdates(sourceUpdates);
                impl.mShared[lane]->setDiagnosticDominant(mEnabled
                    && mPublishedMode == PlanarReflectionMode::SharedCapture && lane == sampledLane
                    && impl.mId == mAnchorId);
                impl.mShared[lane]->setDiagnosticComparison(mEnabled
                    && mPublishedMode == PlanarReflectionMode::SharedCapture && lane == sampledLane
                    && impl.mId == mComparisonId);
            }
        }
    }

    void PlanarReflectionProviderImpl::updateSourcePlane(float dt)
    {
        if (mReceivers.empty())
            return;
        unsigned int latestFrame = 0;
        for (const PlanarReflectionReceiver* receiver : mReceivers)
            latestFrame = std::max(latestFrame, receiver->mImpl->mVisibleFrame);
        std::vector<SceneUtil::WaterReflectionAnchorCandidate> candidates;
        for (const PlanarReflectionReceiver* receiver : mReceivers)
        {
            const auto& impl = *receiver->mImpl;
            if (recentlyVisible(impl, latestFrame) && impl.mImpact > 0.0)
                candidates.push_back({ impl.mId, impl.mImpact });
        }
        if (candidates.empty())
            for (const PlanarReflectionReceiver* receiver : mReceivers)
                if (receiver->mImpl->mPrimary)
                    candidates.push_back({ receiver->mImpl->mId, 1.0 });
        if (candidates.empty())
            candidates.push_back({ mReceivers.front()->mImpl->mId, 1.0 });

        const std::uint64_t previousComparison = mComparisonId;
        const std::uint64_t previousAnchor = mAnchorId;
        const bool selectedAnchorVisible = std::any_of(
            candidates.begin(), candidates.end(), [&](const auto& candidate) { return candidate.mId == mAnchorId; });
        if (mAnchorHandoff == PlanarReflectionAnchorHandoff::Idle || !selectedAnchorVisible)
            mAnchorId = mAnchorPolicy.select(candidates, latestFrame);
        mAnchorImpact = 0.0;
        mComparisonId = 0;
        mComparisonImpact = 0.0;
        for (const SceneUtil::WaterReflectionAnchorCandidate& candidate : candidates)
        {
            if (candidate.mId == mAnchorId)
                mAnchorImpact = candidate.mImpact;
            else if (candidate.mImpact > mComparisonImpact)
            {
                mComparisonId = candidate.mId;
                mComparisonImpact = candidate.mImpact;
            }
        }
        if (mComparisonId != previousComparison)
            applyWorkState();

        const auto anchor = std::find_if(mReceivers.begin(), mReceivers.end(),
            [&](const auto* receiver) { return receiver->mImpl->mId == mAnchorId; });
        if (anchor == mReceivers.end())
            return;

        const bool sharedActive = mPublishedMode == PlanarReflectionMode::SharedCapture
            || mRequestedMode == PlanarReflectionMode::SharedCapture;
        if (mAnchorId != previousAnchor)
        {
            ++mAnchorChanges;
            auto& anchorImpl = *(*anchor)->mImpl;
            if (sharedActive)
            {
                mStagingLane = 1u - mActiveLane;
                mLaneAnchorIds[mStagingLane] = mAnchorId;
                configureSource(mStagingLane, anchorImpl);
                for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : mSources[mStagingLane])
                    source->beginStage();
                for (PlanarReflectionReceiver* receiver : mReceivers)
                    receiver->mImpl->mShared[mStagingLane]->beginStage();
                mAnchorHandoff = PlanarReflectionAnchorHandoff::Staging;
                mAnchorHandoffElapsed = 0.f;
                mPreviousTextureWeight = 0.f;
            }
            else
            {
                mLaneAnchorIds[mActiveLane] = mAnchorId;
                configureSource(mActiveLane, anchorImpl);
            }
            const auto selectedCandidate = std::find_if(candidates.begin(), candidates.end(),
                [&](const auto& candidate) { return candidate.mId == mAnchorId; });
            const auto previousCandidate = std::find_if(candidates.begin(), candidates.end(),
                [&](const auto& candidate) { return candidate.mId == previousAnchor; });
            Log(Debug::Info) << "Selected shared planar-reflection anchor " << mAnchorId << " of " << mReceivers.size()
                             << " receivers on plane normal " << anchorImpl.mPlane.mNormal.x() << ' '
                             << anchorImpl.mPlane.mNormal.y() << ' ' << anchorImpl.mPlane.mNormal.z() << " distance "
                             << anchorImpl.mPlane.mDistance << " at impact "
                             << (selectedCandidate != candidates.end() ? selectedCandidate->mImpact : 0.0)
                             << " versus prior "
                             << (previousCandidate != candidates.end() ? previousCandidate->mImpact : 0.0);
            applyWorkState();
        }

        if (mAnchorHandoff == PlanarReflectionAnchorHandoff::Idle)
            configureSource(mActiveLane, *(*anchor)->mImpl);
        else if (mAnchorHandoff == PlanarReflectionAnchorHandoff::Staging)
        {
            configureSource(mStagingLane, *(*anchor)->mImpl);
            if (anchorHandoffReady(latestFrame))
            {
                mAnchorHandoff = PlanarReflectionAnchorHandoff::Blending;
                mAnchorHandoffElapsed = 0.f;
                mPreviousTextureWeight = 1.f;
                applyWorkState();
            }
        }
        else
        {
            constexpr float transitionDuration = 0.3f;
            configureSource(mStagingLane, *(*anchor)->mImpl);
            mAnchorHandoffElapsed += std::max(0.f, dt);
            const float linearProgress = std::clamp(mAnchorHandoffElapsed / transitionDuration, 0.f, 1.f);
            const float smoothProgress = linearProgress * linearProgress * (3.f - 2.f * linearProgress);
            mPreviousTextureWeight = 1.f - smoothProgress;
            if (linearProgress >= 1.f)
            {
                mActiveLane = mStagingLane;
                mStagingLane = 1u - mActiveLane;
                mAnchorHandoff = PlanarReflectionAnchorHandoff::Idle;
                mPreviousTextureWeight = 0.f;
                applyWorkState();
            }
        }

        for (const auto& lane : mSources)
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                source->setSourceDiagnostics(mReceivers.size(), mAnchorId, mAnchorChanges, mAnchorImpact, mComparisonId,
                    mComparisonImpact, static_cast<unsigned int>(mAnchorHandoff), mPreviousTextureWeight);
    }

    void PlanarReflectionProviderImpl::publishReadyMode()
    {
        if (mRequestedMode == mPublishedMode)
            return;
        unsigned int latestFrame = 0;
        for (const PlanarReflectionReceiver* receiver : mReceivers)
            latestFrame = std::max(latestFrame, receiver->mImpl->mVisibleFrame);
        bool ready = mRequestedMode != PlanarReflectionMode::SharedCapture
            || (mSources[mActiveLane][sExactSourceLayer]->completed()
                && (!unionLayerRequired(mActiveLane) || mSources[mActiveLane][sUnionSourceLayer]->completed()));
        for (const PlanarReflectionReceiver* receiver : mReceivers)
        {
            const auto& impl = *receiver->mImpl;
            if (!recentlyVisible(impl, latestFrame))
                continue;
            ready = ready
                && (mRequestedMode == PlanarReflectionMode::DirectReference ? impl.mDirect->completed()
                                                                            : impl.mShared[mActiveLane]->completed());
        }
        if (!ready)
            return;
        mPublishedMode = mRequestedMode;
        applyWorkState();
        Log(Debug::Info) << "Published "
                         << (mPublishedMode == PlanarReflectionMode::SharedCapture ? "shared-capture"
                                                                                   : "direct-reference")
                         << " planar reflections after frame-complete target handoff";
    }
}
