#include <apps/openmw/diagnosticgameplayroute.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>

namespace
{
    /// Owns an isolated artifact root for one gameplay-route lifecycle test.
    class DiagnosticGameplayRouteTest : public ::testing::Test
    {
    protected:
        /// Create the writable artifact root before each test.
        void SetUp() override
        {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            mDirectory = std::filesystem::temp_directory_path() / ("openmw-gameplay-route-" + std::to_string(suffix));
            std::filesystem::create_directories(mDirectory);
        }

        /// Remove every artifact created by the test.
        void TearDown() override
        {
            std::error_code error;
            std::filesystem::remove_all(mDirectory, error);
        }

        std::filesystem::path mDirectory;
    };

    /// Return a valid route frame with every replayed field populated.
    Diagnostics::GameplayRouteFrame makeFrame(double duration, float offset)
    {
        return { duration, "sys::default", { 1.f + offset, 2.f, 3.f }, { 0.1f, 0.2f, 0.3f }, { 0.4f, 0.5f, 0.6f },
            true };
    }

    TEST_F(DiagnosticGameplayRouteTest, RecordsCapsuleAndReplaysFramesInOrder)
    {
        Diagnostics::Capsule initialState;
        initialState.add({ "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest, { std::byte{ 1 } } });
        OMW::DiagnosticGameplayRoute route(mDirectory);

        route.startRecording(std::move(initialState));
        EXPECT_TRUE(route.recording());
        EXPECT_FALSE(route.recordFrame(makeFrame(0.016, 0.f)).has_value());
        EXPECT_FALSE(route.recordFrame(makeFrame(0.020, 1.f)).has_value());
        const std::filesystem::path path = route.stopRecording();

        EXPECT_FALSE(route.recording());
        ASSERT_TRUE(std::filesystem::exists(path));
        const Diagnostics::Capsule capsule = Diagnostics::readCapsule(path);
        ASSERT_NE(capsule.find("manifest.yaml"), nullptr);
        const Diagnostics::CapsuleEntry* payload = capsule.find("state/gameplay-route.yaml");
        ASSERT_NE(payload, nullptr);

        route.loadReplay(payload->mData);
        ASSERT_EQ(route.remainingReplayFrames(), 2u);
        EXPECT_EQ(route.replayFrame(), makeFrame(0.016, 0.f));
        route.completeReplayFrame();
        EXPECT_EQ(route.replayFrame(), makeFrame(0.020, 1.f));
        route.completeReplayFrame();
        EXPECT_FALSE(route.replaying());
    }

    TEST_F(DiagnosticGameplayRouteTest, RejectsEmptyAndOverlappingRecordingLifecycle)
    {
        OMW::DiagnosticGameplayRoute route(mDirectory);
        route.startRecording(Diagnostics::Capsule{});
        EXPECT_THROW(route.startRecording(Diagnostics::Capsule{}), std::logic_error);
        EXPECT_THROW(static_cast<void>(route.stopRecording()), std::runtime_error);
        EXPECT_FALSE(route.recording());
        EXPECT_THROW(static_cast<void>(route.stopRecording()), std::logic_error);
    }

    TEST_F(DiagnosticGameplayRouteTest, DiscardsARecordingThatCannotBeEncoded)
    {
        OMW::DiagnosticGameplayRoute route(mDirectory);
        route.startRecording(Diagnostics::Capsule{});
        EXPECT_FALSE(route.recordFrame(makeFrame(0.201, 0.f)).has_value());

        EXPECT_THROW(static_cast<void>(route.stopRecording()), std::invalid_argument);
        EXPECT_FALSE(route.recording());
        EXPECT_THROW(static_cast<void>(route.stopRecording()), std::logic_error);
    }

    /// Bind exact clearance observations to one replay segment and reject incomplete or mismatched captures.
    TEST_F(DiagnosticGameplayRouteTest, OwnsRequestScopedReplayClearance)
    {
        OMW::DiagnosticGameplayRoute route(mDirectory);
        const std::array frames{ makeFrame(0.016, 0.f), makeFrame(0.020, 1.f) };
        const auto payload = Diagnostics::encodeGameplayRoute(frames);
        route.loadReplay(payload);
        route.startClearanceCapture(42, 2);
        EXPECT_TRUE(route.capturingClearance());
        EXPECT_EQ(route.replayFrameIndex(), 0u);
        EXPECT_THROW(route.loadReplay(payload), std::logic_error);

        OMW::DiagnosticRouteClearanceFrame first;
        first.mRouteFrame = 0;
        first.mCell = "sys::default";
        first.mShape = "cylinder";
        route.recordClearance(first);
        route.completeReplayFrame();
        EXPECT_THROW(static_cast<void>(route.finishClearanceCapture(7)), std::logic_error);
        EXPECT_THROW(static_cast<void>(route.finishClearanceCapture(42)), std::logic_error);

        OMW::DiagnosticRouteClearanceFrame second;
        second.mRouteFrame = 1;
        second.mCell = "sys::default";
        second.mShape = "cylinder";
        route.recordClearance(second);
        route.completeReplayFrame();
        const auto captured = route.finishClearanceCapture(42);
        ASSERT_TRUE(captured.has_value());
        ASSERT_EQ(captured->size(), 2u);
        EXPECT_EQ(captured->front().mRouteFrame, 0u);
        EXPECT_EQ(captured->back().mRouteFrame, 1u);
        EXPECT_FALSE(route.capturingClearance());
        EXPECT_FALSE(route.finishClearanceCapture(42).has_value());
    }

    /// Release failed request state so a later route segment can capture clearance normally.
    TEST_F(DiagnosticGameplayRouteTest, DiscardsOnlyTheMatchingClearanceCapture)
    {
        OMW::DiagnosticGameplayRoute route(mDirectory);
        const std::array frames{ makeFrame(0.016, 0.f), makeFrame(0.020, 1.f) };
        route.loadReplay(Diagnostics::encodeGameplayRoute(frames));
        route.startClearanceCapture(41, 1);

        route.discardClearanceCapture(40);
        EXPECT_TRUE(route.capturingClearance());

        route.discardClearanceCapture(41);
        EXPECT_FALSE(route.capturingClearance());
        EXPECT_NO_THROW(route.startClearanceCapture(42, 1));
    }

    /// Bind movement outcomes to exact route targets and keep request ownership across segmented replay.
    TEST_F(DiagnosticGameplayRouteTest, OwnsRequestScopedTraversal)
    {
        OMW::DiagnosticGameplayRoute route(mDirectory);
        const std::array frames{ makeFrame(0.016, 0.f), makeFrame(0.020, 1.f) };
        const auto payload = Diagnostics::encodeGameplayRoute(frames);
        route.loadReplay(payload);
        route.startTraversalCapture(42, 2);
        EXPECT_TRUE(route.capturingTraversal());
        EXPECT_THROW(route.loadReplay(payload), std::logic_error);

        OMW::DiagnosticRouteTraversalFrame first;
        first.mRouteFrame = 0;
        first.mTargetCell = "sys::default";
        first.mActualCell = "sys::default";
        first.mInitialized = true;
        first.mTargetReached = true;
        route.recordTraversal(first);
        route.completeReplayFrame();
        EXPECT_THROW(static_cast<void>(route.finishTraversalCapture(7)), std::logic_error);
        EXPECT_THROW(static_cast<void>(route.finishTraversalCapture(42)), std::logic_error);

        OMW::DiagnosticRouteTraversalFrame second;
        second.mRouteFrame = 1;
        second.mTargetCell = "sys::default";
        second.mActualCell = "sys::default";
        second.mTargetReached = true;
        route.recordTraversal(second);
        route.completeReplayFrame();
        const auto captured = route.finishTraversalCapture(42);
        ASSERT_TRUE(captured.has_value());
        ASSERT_EQ(captured->size(), 2u);
        EXPECT_TRUE(captured->front().mInitialized);
        EXPECT_FALSE(captured->back().mInitialized);
        EXPECT_FALSE(route.capturingTraversal());
        EXPECT_FALSE(route.finishTraversalCapture(42).has_value());
    }

    /// Release only the failed traversal request so later segments can continue the loaded route.
    TEST_F(DiagnosticGameplayRouteTest, DiscardsOnlyTheMatchingTraversalCapture)
    {
        OMW::DiagnosticGameplayRoute route(mDirectory);
        const std::array frames{ makeFrame(0.016, 0.f), makeFrame(0.020, 1.f) };
        route.loadReplay(Diagnostics::encodeGameplayRoute(frames));
        route.startTraversalCapture(41, 1);

        route.discardTraversalCapture(40);
        EXPECT_TRUE(route.capturingTraversal());

        route.discardTraversalCapture(41);
        EXPECT_FALSE(route.capturingTraversal());
        EXPECT_NO_THROW(route.startTraversalCapture(42, 1));
    }
}
