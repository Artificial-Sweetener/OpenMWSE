#ifndef OPENMW_MWRENDER_PLANARREFLECTIONPROVIDERIMPL_H
#define OPENMW_MWRENDER_PLANARREFLECTIONPROVIDERIMPL_H

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <osg/ref_ptr>

#include <components/sceneutil/waterutil.hpp>

#include "planarreflectionprovider.hpp"

namespace osg
{
    class Callback;
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class ExactPlanarReflectionCapture;
    class SharedPlanarReflectionTarget;

    /// Own the two render paths and observations associated with one public receiver handle.
    class PlanarReflectionReceiverImpl
    {
    public:
        /// Construct receiver policy state before its provider attaches render targets.
        PlanarReflectionReceiverImpl(PlanarReflectionProviderImpl& provider, const SceneUtil::WaterSurfacePlane& plane,
            unsigned int resolution, bool visibilityDriven, bool primary);

        PlanarReflectionProviderImpl& mProvider;
        SceneUtil::WaterSurfacePlane mPlane;
        unsigned int mResolution;
        bool mVisibilityDriven;
        bool mPrimary;
        std::uint64_t mId = 0;
        unsigned int mVisibleFrame = 0;
        unsigned int mImpactFrame = 0;
        double mImpact = 0.0;
        bool mReleased = false;
        osg::ref_ptr<ExactPlanarReflectionCapture> mDirect;
        std::array<osg::ref_ptr<SharedPlanarReflectionTarget>, 2> mShared;
    };

    /// Describe the frame-complete phases of a source-anchor texture handoff.
    enum class PlanarReflectionAnchorHandoff
    {
        Idle,
        Staging,
        Blending,
    };

    /// Coordinate source selection, target work, and frame-complete live mode handoffs.
    class PlanarReflectionProviderImpl
    {
    public:
        /// Initialize the provider in its startup mode and add the shared source before dependent targets.
        PlanarReflectionProviderImpl(osg::Group& parent, osg::Group& sceneRoot,
            Resource::ResourceSystem& resourceSystem, unsigned int sourceResolution, bool interior,
            PlanarReflectionMode initialMode);

        /// Detach source and receiver cameras while receiver implementations remain valid.
        ~PlanarReflectionProviderImpl();

        /// Add one receiver's direct and reconstructed targets after the shared source.
        void attach(PlanarReflectionReceiver& receiver);

        /// Remove one receiver and its cameras without invalidating other handles.
        void detach(PlanarReflectionReceiverImpl& receiver);

        /// Return whether a receiver was visible in the source-selection observation window.
        bool recentlyVisible(const PlanarReflectionReceiverImpl& receiver, unsigned int latestFrame) const;

        /// Configure one source lane with the baseline capture contract at an exact receiver plane.
        void configureSource(unsigned int lane, PlanarReflectionReceiverImpl& anchor);

        /// Update the exact plane used by every source lane anchored to a moving receiver.
        void updateAnchorPlane(PlanarReflectionReceiverImpl& anchor);

        /// Preserve the union of receiver-required half-spaces when one ordinary clip plane can represent it.
        void updateSourceClipPlane();

        /// Return whether a lane needs its union-clipped source in addition to its exact anchor source.
        bool unionLayerRequired(unsigned int lane) const;

        /// Return whether the staged source and every visible dependent target completed a frame.
        bool anchorHandoffReady(unsigned int latestFrame) const;

        /// Return the texture produced for one receiver from one exact source lane.
        osg::Texture* laneTexture(
            PlanarReflectionReceiverImpl& receiver, unsigned int lane, osgUtil::CullVisitor& visitor) const;

        /// Enable paths needed for the published and staged live-comparison modes.
        void applyWorkState();

        /// Select the highest-impact stable anchor and apply its exact capture plane.
        void updateSourcePlane(float dt);

        /// Publish the requested path after its required targets complete a frame.
        void publishReadyMode();

        osg::Group& mParent;
        osg::Group& mSceneRoot;
        Resource::ResourceSystem& mResourceSystem;
        bool mInterior = false;
        bool mShowWorld = true;
        bool mEnabled = true;
        bool mSupportsShared = false;
        static constexpr unsigned int sExactSourceLayer = 0;
        static constexpr unsigned int sUnionSourceLayer = 1;
        static constexpr unsigned int sSourceLayerCount = 2;
        std::array<std::array<osg::ref_ptr<ExactPlanarReflectionCapture>, sSourceLayerCount>, 2> mSources;
        std::optional<SceneUtil::WaterSurfacePlane> mUnionClipPlane;
        osg::ref_ptr<osg::Callback> mCullCallback;
        std::vector<PlanarReflectionReceiver*> mReceivers;
        std::uint64_t mNextReceiverId = 1;
        std::uint64_t mAnchorId = 0;
        unsigned int mAnchorChanges = 0;
        SceneUtil::WaterReflectionAnchorPolicy mAnchorPolicy;
        std::array<std::uint64_t, 2> mLaneAnchorIds{ 0, 0 };
        unsigned int mActiveLane = 0;
        unsigned int mStagingLane = 1;
        PlanarReflectionAnchorHandoff mAnchorHandoff = PlanarReflectionAnchorHandoff::Idle;
        float mAnchorHandoffElapsed = 0.f;
        float mPreviousTextureWeight = 0.f;
        std::uint64_t mComparisonId = 0;
        double mAnchorImpact = 0.0;
        double mComparisonImpact = 0.0;
        PlanarReflectionMode mPublishedMode = PlanarReflectionMode::DirectReference;
        PlanarReflectionMode mRequestedMode = PlanarReflectionMode::DirectReference;
    };
}

#endif
