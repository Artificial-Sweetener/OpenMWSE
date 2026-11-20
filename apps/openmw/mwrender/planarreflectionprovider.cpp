#include "planarreflectionprovider.hpp"

#include <algorithm>

#include <osg/Group>
#include <osg/Texture>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>

#include "planarreflectioncapture.hpp"
#include "planarreflectionproviderimpl.hpp"
#include "planarreflectionreconstruction.hpp"

namespace MWRender
{
    PlanarReflectionReceiver::PlanarReflectionReceiver(PlanarReflectionProvider& provider,
        const SceneUtil::WaterSurfacePlane& plane, unsigned int resolution, bool visibilityDriven, bool primary)
        : mImpl(std::make_unique<PlanarReflectionReceiverImpl>(
            *provider.mImpl, plane, resolution, visibilityDriven, primary))
    {
    }

    PlanarReflectionReceiver::~PlanarReflectionReceiver()
    {
        if (!mImpl->mReleased)
            mImpl->mProvider.detach(*mImpl);
    }

    osg::Texture* PlanarReflectionReceiver::texture(osgUtil::CullVisitor& visitor)
    {
        if (mImpl->mProvider.mPublishedMode != PlanarReflectionMode::SharedCapture)
            return mImpl->mDirect->getColorTexture(&visitor);
        const unsigned int lane = mImpl->mProvider.mAnchorHandoff == PlanarReflectionAnchorHandoff::Blending
            ? mImpl->mProvider.mStagingLane
            : mImpl->mProvider.mActiveLane;
        return mImpl->mProvider.laneTexture(*mImpl, lane, visitor);
    }

    osg::Texture* PlanarReflectionReceiver::previousTexture(osgUtil::CullVisitor& visitor)
    {
        if (mImpl->mProvider.mPublishedMode != PlanarReflectionMode::SharedCapture
            || mImpl->mProvider.mAnchorHandoff != PlanarReflectionAnchorHandoff::Blending)
            return texture(visitor);
        return mImpl->mProvider.laneTexture(*mImpl, mImpl->mProvider.mActiveLane, visitor);
    }

    float PlanarReflectionReceiver::previousTextureWeight() const
    {
        return mImpl->mProvider.mPublishedMode == PlanarReflectionMode::SharedCapture
                && mImpl->mProvider.mAnchorHandoff == PlanarReflectionAnchorHandoff::Blending
            ? mImpl->mProvider.mPreviousTextureWeight
            : 0.f;
    }

    void PlanarReflectionReceiver::setPlane(const SceneUtil::WaterSurfacePlane& plane)
    {
        mImpl->mPlane = plane;
        mImpl->mDirect->setPlane(plane);
        for (const osg::ref_ptr<SharedPlanarReflectionTarget>& shared : mImpl->mShared)
            shared->setPlane(plane);
        mImpl->mProvider.updateSourceClipPlane();
        if (mImpl->mId == mImpl->mProvider.mAnchorId)
            mImpl->mProvider.updateAnchorPlane(*mImpl);
    }

    void PlanarReflectionReceiver::markVisible(unsigned int frameNumber, double impact)
    {
        mImpl->mVisibleFrame = frameNumber;
        if (mImpl->mImpactFrame != frameNumber)
        {
            mImpl->mImpactFrame = frameNumber;
            mImpl->mImpact = 0.0;
        }
        mImpl->mImpact += std::max(0.0, impact);
        mImpl->mDirect->markVisible(frameNumber);
        for (const osg::ref_ptr<SharedPlanarReflectionTarget>& shared : mImpl->mShared)
            shared->markVisible(frameNumber);
        for (unsigned int lane = 0; lane < mImpl->mProvider.mSources.size(); ++lane)
            if (mImpl->mId == mImpl->mProvider.mLaneAnchorIds[lane])
                for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : mImpl->mProvider.mSources[lane])
                    source->markVisible(frameNumber);
    }

    bool PlanarReflectionReceiver::hasCompletedRender() const
    {
        if (mImpl->mProvider.mRequestedMode != PlanarReflectionMode::SharedCapture)
            return mImpl->mDirect->completed();
        const unsigned int lane = mImpl->mProvider.mActiveLane;
        return mImpl->mShared[lane]->completed();
    }

    unsigned int PlanarReflectionReceiver::resolution() const
    {
        return mImpl->mResolution;
    }

    const SceneUtil::WaterSurfacePlane& PlanarReflectionReceiver::plane() const
    {
        return mImpl->mPlane;
    }

    PlanarReflectionProvider::PlanarReflectionProvider(osg::Group& parent, osg::Group& sceneRoot,
        Resource::ResourceSystem& resourceSystem, unsigned int sourceResolution, bool interior,
        PlanarReflectionMode initialMode)
        : mImpl(std::make_unique<PlanarReflectionProviderImpl>(
            parent, sceneRoot, resourceSystem, sourceResolution, interior, initialMode))
    {
    }

    PlanarReflectionProvider::~PlanarReflectionProvider() = default;

    osg::ref_ptr<PlanarReflectionReceiver> PlanarReflectionProvider::createReceiver(
        const SceneUtil::WaterSurfacePlane& plane, unsigned int resolution, bool visibilityDriven, bool primary)
    {
        osg::ref_ptr<PlanarReflectionReceiver> receiver
            = new PlanarReflectionReceiver(*this, plane, resolution, visibilityDriven, primary);
        mImpl->attach(*receiver);
        return receiver;
    }

    void PlanarReflectionProvider::releaseReceiver(PlanarReflectionReceiver& receiver)
    {
        const auto found = std::find(mImpl->mReceivers.begin(), mImpl->mReceivers.end(), &receiver);
        if (found == mImpl->mReceivers.end())
            return;
        mImpl->detach(*receiver.mImpl);
        mImpl->mReceivers.erase(found);
        if (receiver.mImpl->mId == mImpl->mAnchorId)
        {
            mImpl->mAnchorId = 0;
            mImpl->mAnchorHandoff = PlanarReflectionAnchorHandoff::Idle;
            mImpl->updateSourcePlane(0.f);
        }
    }

    void PlanarReflectionProvider::setMode(PlanarReflectionMode mode)
    {
        if (mode == PlanarReflectionMode::SharedCapture && !mImpl->mSupportsShared)
        {
            Log(Debug::Warning)
                << "Shared planar reflections require GPU shader4 and non-multiview rendering; retaining "
                   "Direct Reference mode";
            mode = PlanarReflectionMode::DirectReference;
        }
        if (mode == mImpl->mRequestedMode)
            return;
        mImpl->mRequestedMode = mode;
        if (mode == PlanarReflectionMode::SharedCapture)
        {
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : mImpl->mSources[mImpl->mActiveLane])
                source->beginStage();
            for (PlanarReflectionReceiver* receiver : mImpl->mReceivers)
                receiver->mImpl->mShared[mImpl->mActiveLane]->beginStage();
        }
        else
            for (PlanarReflectionReceiver* receiver : mImpl->mReceivers)
                receiver->mImpl->mDirect->beginStage();
        mImpl->applyWorkState();
    }

    void PlanarReflectionProvider::update(float dt)
    {
        mImpl->updateSourcePlane(dt);
        mImpl->publishReadyMode();
    }

    void PlanarReflectionProvider::setInterior(bool interior)
    {
        mImpl->mInterior = interior;
        for (const auto& lane : mImpl->mSources)
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                source->setInterior(interior);
        for (PlanarReflectionReceiver* receiver : mImpl->mReceivers)
            receiver->mImpl->mDirect->setInterior(interior);
    }

    void PlanarReflectionProvider::showWorld(bool show)
    {
        mImpl->mShowWorld = show;
        for (const auto& lane : mImpl->mSources)
            for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                source->showWorld(show);
        for (PlanarReflectionReceiver* receiver : mImpl->mReceivers)
            receiver->mImpl->mDirect->showWorld(show);
    }

    void PlanarReflectionProvider::setEnabled(bool enabled)
    {
        mImpl->mEnabled = enabled;
        mImpl->applyWorkState();
    }

    void PlanarReflectionProvider::setCullCallback(osg::Callback* callback)
    {
        if (mImpl->mCullCallback)
        {
            for (const auto& lane : mImpl->mSources)
                for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                    source->removeCullCallback(mImpl->mCullCallback);
            for (PlanarReflectionReceiver* receiver : mImpl->mReceivers)
            {
                receiver->mImpl->mDirect->removeCullCallback(mImpl->mCullCallback);
                for (const osg::ref_ptr<SharedPlanarReflectionTarget>& shared : receiver->mImpl->mShared)
                    shared->removeCullCallback(mImpl->mCullCallback);
            }
        }
        mImpl->mCullCallback = callback;
        if (mImpl->mCullCallback)
        {
            for (const auto& lane : mImpl->mSources)
                for (const osg::ref_ptr<ExactPlanarReflectionCapture>& source : lane)
                    source->addCullCallback(mImpl->mCullCallback);
            for (PlanarReflectionReceiver* receiver : mImpl->mReceivers)
            {
                receiver->mImpl->mDirect->addCullCallback(mImpl->mCullCallback);
                for (const osg::ref_ptr<SharedPlanarReflectionTarget>& shared : receiver->mImpl->mShared)
                    shared->addCullCallback(mImpl->mCullCallback);
            }
        }
    }

    PlanarReflectionMode PlanarReflectionProvider::publishedMode() const
    {
        return mImpl->mPublishedMode;
    }

    bool PlanarReflectionProvider::supportsSharedCapture() const
    {
        return mImpl->mSupportsShared;
    }
}
