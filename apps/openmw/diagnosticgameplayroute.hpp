#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICGAMEPLAYROUTE_HPP
#define OPENMW_APPS_OPENMW_DIAGNOSTICGAMEPLAYROUTE_HPP

#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/gameplayroute.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <osg/Vec3f>

namespace OMW
{
    /// Accept the small horizontal residue left by collision margins after one commanded route transition.
    inline constexpr float diagnosticRouteHorizontalTolerance = 8.f;

    /// Accept ground-following displacement within the engine's ordinary step and support margin.
    inline constexpr float diagnosticRouteVerticalTolerance = 16.f;

    /// Identifies one exact placed object intersecting a diagnostic standing-actor probe.
    struct DiagnosticRouteClearanceContact
    {
        std::string mObjectId;
        std::string mReference;
        osg::Vec3f mPoint;
        osg::Vec3f mNormal;
        float mPenetrationDepth = 0.f;
    };

    /// Captures the authoritative Bullet standing clearance observed for one consumed route frame.
    struct DiagnosticRouteClearanceFrame
    {
        std::size_t mRouteFrame = 0;
        std::string mCell;
        osg::Vec3f mPosition;
        std::string mShape;
        osg::Vec3f mHalfExtents;
        float mLift = 0.f;
        std::vector<DiagnosticRouteClearanceContact> mContacts;
    };

    /// Captures one target and the position achieved by authoritative player movement during diagnostic traversal.
    struct DiagnosticRouteTraversalFrame
    {
        std::size_t mRouteFrame = 0;
        std::string mTargetCell;
        osg::Vec3f mTargetPosition;
        std::string mActualCell;
        osg::Vec3f mActualPosition;
        float mHorizontalError = 0.f;
        float mVerticalError = 0.f;
        bool mInitialized = false;
        bool mTargetReached = false;
    };

    /// Owns bounded gameplay-route capture and deterministic replay state.
    class DiagnosticGameplayRoute
    {
    public:
        /// Store completed route capsules beneath the supplied diagnostic artifact root.
        explicit DiagnosticGameplayRoute(std::filesystem::path artifactRoot);

        /// Begin recording frames into a capsule containing the route's initial world snapshot.
        void startRecording(Diagnostics::Capsule initialState);

        /// Append one authoritative rendered frame and finish automatically at the route bound.
        std::optional<std::filesystem::path> recordFrame(Diagnostics::GameplayRouteFrame frame);

        /// Finish the active recording and return its immutable capsule path.
        std::filesystem::path stopRecording();

        /// Return whether live gameplay frames are being recorded.
        bool recording() const noexcept;

        /// Validate and load a recorded route for deterministic child replay.
        void loadReplay(std::span<const std::byte> payload);

        /// Return the frame that should drive the next scheduled replay update.
        const Diagnostics::GameplayRouteFrame& replayFrame() const;

        /// Advance past the frame just applied by the engine.
        void completeReplayFrame();

        /// Return whether a loaded replay still has frames to apply.
        bool replaying() const noexcept;

        /// Return the number of loaded route frames not yet applied.
        std::size_t remainingReplayFrames() const noexcept;

        /// Begin retaining one request's bounded standing-actor clearance observations.
        void startClearanceCapture(std::uint64_t requestId, std::size_t frameCount);

        /// Return whether the current replay frame belongs to a clearance capture.
        bool capturingClearance() const noexcept;

        /// Return the absolute index of the route frame currently being applied.
        std::size_t replayFrameIndex() const noexcept;

        /// Retain the authoritative Bullet clearance observed for the current replay frame.
        void recordClearance(DiagnosticRouteClearanceFrame frame);

        /// Finish and remove the matching complete capture, or report that the request did not ask for one.
        std::optional<std::vector<DiagnosticRouteClearanceFrame>> finishClearanceCapture(std::uint64_t requestId);

        /// Discard an unfinished matching capture after its owning diagnostic collection fails.
        void discardClearanceCapture(std::uint64_t requestId) noexcept;

        /// Begin retaining one request's bounded authoritative player-movement observations.
        void startTraversalCapture(std::uint64_t requestId, std::size_t frameCount);

        /// Return whether the current replay frame should be reached through player movement.
        bool capturingTraversal() const noexcept;

        /// Retain the authoritative player position achieved for the current replay target.
        void recordTraversal(DiagnosticRouteTraversalFrame frame);

        /// Finish and remove the matching traversal capture, or report that the request did not ask for one.
        std::optional<std::vector<DiagnosticRouteTraversalFrame>> finishTraversalCapture(std::uint64_t requestId);

        /// Discard an unfinished matching traversal capture after its owning diagnostic collection fails.
        void discardTraversalCapture(std::uint64_t requestId) noexcept;

    private:
        /// Write the current recording to a non-replacing timestamped capsule.
        std::filesystem::path finishRecording();

        /// Clear a recording that cannot be completed without leaving capture active.
        void discardRecording() noexcept;

        std::filesystem::path mArtifactRoot;
        std::optional<Diagnostics::Capsule> mRecordingCapsule;
        std::vector<Diagnostics::GameplayRouteFrame> mRecordedFrames;
        std::vector<Diagnostics::GameplayRouteFrame> mReplayFrames;
        std::size_t mReplayIndex = 0;
        std::optional<std::uint64_t> mClearanceRequestId;
        std::size_t mClearanceFrameCount = 0;
        std::vector<DiagnosticRouteClearanceFrame> mClearanceFrames;
        std::optional<std::uint64_t> mTraversalRequestId;
        std::size_t mTraversalFrameCount = 0;
        std::vector<DiagnosticRouteTraversalFrame> mTraversalFrames;
    };
}

#endif
