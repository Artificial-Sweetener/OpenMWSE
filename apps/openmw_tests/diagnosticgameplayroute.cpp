#include <apps/openmw/diagnosticgameplayroute.hpp>

#include <gtest/gtest.h>

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
        return { duration, "sys::default", { 1.f + offset, 2.f, 3.f }, { 0.1f, 0.2f, 0.3f },
            { 0.4f, 0.5f, 0.6f }, true };
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
}
