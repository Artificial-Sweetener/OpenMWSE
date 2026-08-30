#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICGAMEPLAYROUTE_HPP
#define OPENMW_APPS_OPENMW_DIAGNOSTICGAMEPLAYROUTE_HPP

#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/gameplayroute.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace OMW
{
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
    };
}

#endif
