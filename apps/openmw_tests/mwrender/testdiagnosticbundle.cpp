#include <apps/openmw/mwrender/renderdiagnosticbundle.hpp>
#include <apps/openmw/mwrender/renderframediagnostics.hpp>

#include <gtest/gtest.h>

#include <osg/FrameStamp>
#include <osg/RenderInfo>
#include <osg/State>

#include <array>
#include <chrono>
#include <fstream>

namespace
{
    /// Own private output paths for graphics bundle publication and cancellation tests.
    class RenderDiagnosticBundleTest : public ::testing::Test
    {
    protected:
        /// Create a separate artifact directory for each test.
        void SetUp() override
        {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            mDirectory = std::filesystem::temp_directory_path() / ("openmw-render-bundle-" + std::to_string(suffix));
            std::filesystem::create_directories(mDirectory);
        }

        /// Remove this test's completed and partial artifacts.
        void TearDown() override
        {
            std::error_code error;
            std::filesystem::remove_all(mDirectory, error);
        }

        std::filesystem::path mDirectory;
    };

    /// Preserve the public bundle bytes when frames arrive in separate append operations.
    TEST_F(RenderDiagnosticBundleTest, StreamingMatchesWholeBundleAndPublishesOnlyWhenComplete)
    {
        const std::array<MWRender::RenderDiagnosticImage, 2> images{ MWRender::RenderDiagnosticImage{
                                                                         "first", 1, 1, 4, { 0.1f, 0.2f, 0.3f, 1.f } },
            MWRender::RenderDiagnosticImage{ "second", 2, 1, 1, { 0.4f, 0.5f } } };
        const auto path = mDirectory / "stream.omwdiag";
        MWRender::RenderDiagnosticBundleWriter writer(path);
        writer.append(std::span(images).first(1));
        EXPECT_FALSE(std::filesystem::exists(path));
        writer.append(std::span(images).subspan(1));
        writer.finish(1234);
        EXPECT_TRUE(std::filesystem::exists(path));
        MWRender::writeRenderDiagnosticBundle(mDirectory / "whole.omwdiag", 1234, images);
        std::ifstream streamed(path, std::ios::binary);
        std::ifstream whole(mDirectory / "whole.omwdiag", std::ios::binary);
        const std::string actual(std::istreambuf_iterator<char>{ streamed }, {});
        const std::string expected(std::istreambuf_iterator<char>{ whole }, {});
        EXPECT_EQ(actual, expected);
        ASSERT_GE(actual.size(), 24u);
        EXPECT_EQ(actual.substr(0, 8), "OMWATMD1");
        EXPECT_EQ(static_cast<unsigned char>(actual[20]), 2u);
        EXPECT_THROW(writer.finish(1234), std::logic_error);
        EXPECT_THROW(writer.append(images), std::logic_error);
    }

    /// Leave a scheduled capture untouched by older draw-thread work, then handle the eligible frame normally.
    TEST_F(RenderDiagnosticBundleTest, ScheduledCaptureIgnoresPreviouslyQueuedDrawFrames)
    {
        MWRender::RenderFrameDiagnostics capture;
        const auto path = mDirectory / "scheduled.omwdiag";
        capture.requestCapture(path, 1, true, 100);
        osg::ref_ptr<osg::State> state = new osg::State;
        state->setFrameStamp(new osg::FrameStamp);
        osg::RenderInfo renderInfo(state.get(), nullptr);
        state->getFrameStamp()->setFrameNumber(98);
        capture.captureFrame(renderInfo, 0, 0, 0);
        ASSERT_TRUE(capture.captureInProgress());
        state->getFrameStamp()->setFrameNumber(99);
        capture.captureFrame(renderInfo, 0, 0, 0);
        ASSERT_TRUE(capture.captureInProgress());
        state->getFrameStamp()->setFrameNumber(100);
        // Invalid native dimensions cancel an eligible request before any OpenGL access.
        capture.captureFrame(renderInfo, 0, 0, 0);
        EXPECT_FALSE(capture.captureInProgress());
        EXPECT_FALSE(std::filesystem::exists(path));
        EXPECT_FALSE(std::filesystem::exists(mDirectory / "scheduled.omwdiag.partial"));
    }

    /// Discard incomplete output and reject attachments whose dimensions contradict their payload.
    TEST_F(RenderDiagnosticBundleTest, CancellationRemovesPartialOutput)
    {
        const auto path = mDirectory / "cancel.omwdiag";
        {
            MWRender::RenderDiagnosticBundleWriter writer(path);
            const std::array images{ MWRender::RenderDiagnosticImage{ "bad", 2, 2, 4, { 0.f } } };
            EXPECT_THROW(writer.append(images), std::invalid_argument);
        }
        EXPECT_FALSE(std::filesystem::exists(path));
        EXPECT_FALSE(std::filesystem::exists(mDirectory / "cancel.omwdiag.partial"));
    }
}
