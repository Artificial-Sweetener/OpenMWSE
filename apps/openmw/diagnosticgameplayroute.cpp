#include "diagnosticgameplayroute.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace OMW
{
    DiagnosticGameplayRoute::DiagnosticGameplayRoute(std::filesystem::path artifactRoot)
        : mArtifactRoot(std::move(artifactRoot))
    {
    }

    void DiagnosticGameplayRoute::startRecording(Diagnostics::Capsule initialState)
    {
        if (recording())
            throw std::logic_error("gameplay route recording is already active");
        mRecordingCapsule = std::move(initialState);
        mRecordedFrames.clear();
        mRecordedFrames.reserve(1800);
    }

    std::optional<std::filesystem::path> DiagnosticGameplayRoute::recordFrame(Diagnostics::GameplayRouteFrame frame)
    {
        if (!recording())
            return std::nullopt;
        mRecordedFrames.push_back(std::move(frame));
        if (mRecordedFrames.size() < Diagnostics::maximumGameplayRouteFrames)
            return std::nullopt;
        try
        {
            return finishRecording();
        }
        catch (...)
        {
            discardRecording();
            throw;
        }
    }

    std::filesystem::path DiagnosticGameplayRoute::stopRecording()
    {
        if (!recording())
            throw std::logic_error("gameplay route recording is not active");
        if (mRecordedFrames.empty())
        {
            discardRecording();
            throw std::runtime_error("gameplay route recording contains no frames");
        }
        try
        {
            return finishRecording();
        }
        catch (...)
        {
            discardRecording();
            throw;
        }
    }

    bool DiagnosticGameplayRoute::recording() const noexcept
    {
        return mRecordingCapsule.has_value();
    }

    void DiagnosticGameplayRoute::loadReplay(std::span<const std::byte> payload)
    {
        mReplayFrames = Diagnostics::decodeGameplayRoute(payload);
        mReplayIndex = 0;
    }

    const Diagnostics::GameplayRouteFrame& DiagnosticGameplayRoute::replayFrame() const
    {
        if (!replaying())
            throw std::logic_error("gameplay route replay has no remaining frame");
        return mReplayFrames[mReplayIndex];
    }

    void DiagnosticGameplayRoute::completeReplayFrame()
    {
        if (!replaying())
            throw std::logic_error("gameplay route replay has no frame to complete");
        ++mReplayIndex;
    }

    bool DiagnosticGameplayRoute::replaying() const noexcept
    {
        return mReplayIndex < mReplayFrames.size();
    }

    std::size_t DiagnosticGameplayRoute::remainingReplayFrames() const noexcept
    {
        return mReplayFrames.size() - mReplayIndex;
    }

    std::filesystem::path DiagnosticGameplayRoute::finishRecording()
    {
        Diagnostics::Capsule completed = *mRecordingCapsule;
        completed.add({ "state/gameplay-route.yaml", Diagnostics::CapsuleEntryType::GameplayTrace,
            Diagnostics::encodeGameplayRoute(mRecordedFrames) });

        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm localTime{};
#ifdef _WIN32
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif
        std::ostringstream name;
        name << "gameplay-route-" << std::put_time(&localTime, "%Y%m%d-%H%M%S");
        std::filesystem::path destination = mArtifactRoot / (name.str() + ".omwdiag");
        for (unsigned suffix = 1; std::filesystem::exists(destination); ++suffix)
            destination = mArtifactRoot / (name.str() + "-" + std::to_string(suffix) + ".omwdiag");
        Diagnostics::writeCapsuleAtomically(destination, completed);
        discardRecording();
        return destination;
    }

    void DiagnosticGameplayRoute::discardRecording() noexcept
    {
        mRecordingCapsule.reset();
        mRecordedFrames.clear();
    }
}
