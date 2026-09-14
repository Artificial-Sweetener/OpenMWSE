#include "diagnosticgameplayroute.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

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
        if (mClearanceRequestId || mTraversalRequestId)
            throw std::logic_error("cannot replace a gameplay route during diagnostic capture");
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

    void DiagnosticGameplayRoute::startClearanceCapture(std::uint64_t requestId, std::size_t frameCount)
    {
        if (mClearanceRequestId)
            throw std::logic_error("gameplay route clearance capture is already active");
        if (frameCount == 0 || frameCount > remainingReplayFrames())
            throw std::invalid_argument("gameplay route clearance frame count exceeds the remaining replay");
        mClearanceRequestId = requestId;
        mClearanceFrameCount = frameCount;
        mClearanceFrames.clear();
        mClearanceFrames.reserve(frameCount);
    }

    bool DiagnosticGameplayRoute::capturingClearance() const noexcept
    {
        return mClearanceRequestId.has_value();
    }

    std::size_t DiagnosticGameplayRoute::replayFrameIndex() const noexcept
    {
        return mReplayIndex;
    }

    void DiagnosticGameplayRoute::recordClearance(DiagnosticRouteClearanceFrame frame)
    {
        if (!mClearanceRequestId || mClearanceFrames.size() >= mClearanceFrameCount)
            throw std::logic_error("gameplay route clearance frame is outside an active capture");
        if (frame.mRouteFrame != mReplayIndex)
            throw std::logic_error("gameplay route clearance frame index does not match replay progress");
        mClearanceFrames.push_back(std::move(frame));
    }

    std::optional<std::vector<DiagnosticRouteClearanceFrame>> DiagnosticGameplayRoute::finishClearanceCapture(
        std::uint64_t requestId)
    {
        if (!mClearanceRequestId)
            return std::nullopt;
        if (*mClearanceRequestId != requestId)
            throw std::logic_error("gameplay route clearance request does not match the completed segment");
        if (mClearanceFrames.size() != mClearanceFrameCount)
            throw std::logic_error("gameplay route clearance capture did not observe every requested frame");
        mClearanceRequestId.reset();
        mClearanceFrameCount = 0;
        return std::exchange(mClearanceFrames, {});
    }

    void DiagnosticGameplayRoute::discardClearanceCapture(const std::uint64_t requestId) noexcept
    {
        if (mClearanceRequestId != requestId)
            return;
        mClearanceRequestId.reset();
        mClearanceFrameCount = 0;
        mClearanceFrames.clear();
    }

    void DiagnosticGameplayRoute::startTraversalCapture(std::uint64_t requestId, std::size_t frameCount)
    {
        if (mTraversalRequestId)
            throw std::logic_error("gameplay route traversal capture is already active");
        if (frameCount == 0 || frameCount > remainingReplayFrames())
            throw std::invalid_argument("gameplay route traversal frame count exceeds the remaining replay");
        mTraversalRequestId = requestId;
        mTraversalFrameCount = frameCount;
        mTraversalFrames.clear();
        mTraversalFrames.reserve(frameCount);
    }

    bool DiagnosticGameplayRoute::capturingTraversal() const noexcept
    {
        return mTraversalRequestId.has_value();
    }

    void DiagnosticGameplayRoute::recordTraversal(DiagnosticRouteTraversalFrame frame)
    {
        if (!mTraversalRequestId || mTraversalFrames.size() >= mTraversalFrameCount)
            throw std::logic_error("gameplay route traversal frame is outside an active capture");
        if (frame.mRouteFrame != mReplayIndex)
            throw std::logic_error("gameplay route traversal frame index does not match replay progress");
        mTraversalFrames.push_back(std::move(frame));
    }

    std::optional<std::vector<DiagnosticRouteTraversalFrame>> DiagnosticGameplayRoute::finishTraversalCapture(
        std::uint64_t requestId)
    {
        if (!mTraversalRequestId)
            return std::nullopt;
        if (*mTraversalRequestId != requestId)
            throw std::logic_error("gameplay route traversal request does not match the completed segment");
        if (mTraversalFrames.size() != mTraversalFrameCount)
            throw std::logic_error("gameplay route traversal capture did not observe every requested frame");
        mTraversalRequestId.reset();
        mTraversalFrameCount = 0;
        return std::exchange(mTraversalFrames, {});
    }

    void DiagnosticGameplayRoute::discardTraversalCapture(const std::uint64_t requestId) noexcept
    {
        if (mTraversalRequestId != requestId)
            return;
        mTraversalRequestId.reset();
        mTraversalFrameCount = 0;
        mTraversalFrames.clear();
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
