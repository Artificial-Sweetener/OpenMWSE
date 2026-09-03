#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/offscreencontext.hpp>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/GL>
#include <osg/GLExtensions>
#include <osg/Group>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <cmath>

#ifdef _WIN32
#include <components/misc/windows.hpp>

#include <set>
#endif

namespace
{
    /// Place a one-shot completion fence after the camera's timestamp pair and before context submission.
    class FrameCompletionFence final : public osg::Camera::DrawCallback
    {
    public:
        /// Insert the test fence without flushing the driver's command queue.
        void operator()(osg::RenderInfo& renderInfo) const override
        {
            if (mFence == nullptr)
                mFence = renderInfo.getState()->get<osg::GLExtensions>()->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }

        /// Wait for submission by the context's frame boundary, not by the wait call itself.
        GLenum wait(osg::State& state) const
        {
            if (mFence == nullptr)
                return GL_WAIT_FAILED;
            return state.get<osg::GLExtensions>()->glClientWaitSync(mFence, 0, 1000000000);
        }

        /// Release the test fence while its graphics context is current.
        void releaseGLObjects(osg::State* state) const override
        {
            if (mFence != nullptr && state != nullptr)
            {
                state->get<osg::GLExtensions>()->glDeleteSync(mFence);
                mFence = nullptr;
            }
            osg::Camera::DrawCallback::releaseGLObjects(state);
        }

    private:
        mutable osg::GLsync mFence = nullptr;
    };

    /// Restore diagnostic collection even when an offscreen assertion aborts a test.
    class OffscreenGpuTimingTest : public testing::Test
    {
        /// Leave subsequent tests outside the bounded timing collection.
        void TearDown() override { SceneUtil::setDiagnosticGpuPassStatisticsCollection(false); }
    };

#ifdef _WIN32
    /// Collect visible top-level desktop windows owned by the current process.
    std::set<HWND> getVisibleProcessWindows()
    {
        struct State
        {
            DWORD mProcessId = GetCurrentProcessId();
            std::set<HWND> mWindows;
        } state;
        EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
                auto& state = *reinterpret_cast<State*>(parameter);
                DWORD owner = 0;
                GetWindowThreadProcessId(window, &owner);
                if (owner == state.mProcessId && IsWindowVisible(window))
                    state.mWindows.insert(window);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&state));
        return state.mWindows;
    }
#endif
}

TEST(OffscreenContextTest, RendersWithoutCreatingDesktopWindow)
{
#ifdef _WIN32
    const std::set<HWND> windowsBefore = getVisibleProcessWindows();
#endif

    osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ .mWidth = 64, .mHeight = 32 });
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->valid());
    EXPECT_TRUE(context->getTraits()->pbuffer);
    EXPECT_FALSE(context->getTraits()->windowDecoration);
    EXPECT_FALSE(context->getTraits()->doubleBuffer);
    EXPECT_EQ(context->getTraits()->width, 64);
    EXPECT_EQ(context->getTraits()->height, 32);

    ASSERT_TRUE(context->realize());
    ASSERT_TRUE(context->makeCurrent());
    ASSERT_NE(glGetString(GL_RENDERER), nullptr);
    glViewport(0, 0, 64, 32);
    glClearColor(0.25f, 0.5f, 0.75f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
    context->releaseContext();
    context->close();

#ifdef _WIN32
    EXPECT_EQ(getVisibleProcessWindows(), windowsBefore);
#endif
}

TEST(OffscreenContextTest, RejectsInvalidDimensions)
{
    EXPECT_THROW(SceneUtil::createOffscreenContext({ .mWidth = 0, .mHeight = 32 }), std::invalid_argument);
    EXPECT_THROW(SceneUtil::createOffscreenContext({ .mWidth = 64, .mHeight = -1 }), std::invalid_argument);
}

TEST_F(OffscreenGpuTimingTest, PublishesCameraTimestampsWithoutPresentation)
{
    osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ .mWidth = 64, .mHeight = 32 });
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->realize());
    ASSERT_TRUE(context->makeCurrent());
    const osg::GLExtensions* extensions = context->getState()->get<osg::GLExtensions>();
    ASSERT_NE(extensions, nullptr);
    if (extensions->glVersion < 3.3f && !extensions->isARBTimerQuerySupported)
        GTEST_SKIP() << "Timestamp queries are not supported by this offscreen context";
    ASSERT_NE(extensions->glFenceSync, nullptr);
    ASSERT_NE(extensions->glClientWaitSync, nullptr);
    ASSERT_NE(extensions->glDeleteSync, nullptr);
    context->releaseContext();
    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    osg::Camera* camera = viewer.getCamera();
    camera->setGraphicsContext(context);
    camera->setViewport(0, 0, 64, 32);
    camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    viewer.setSceneData(new osg::Group);
    SceneUtil::enableDiagnosticGpuPassStatistics(*camera);
    osg::ref_ptr<FrameCompletionFence> completionFence = new FrameCompletionFence;
    camera->addFinalDrawCallback(completionFence);
    SceneUtil::setDiagnosticGpuPassStatisticsCollection(true);
    viewer.frame();
    const unsigned sampleFrame = viewer.getFrameStamp()->getFrameNumber();

    ASSERT_TRUE(context->makeCurrent());
    SCOPED_TRACE(testing::Message() << "GL " << extensions->glVersion << ", EXT timer "
                                    << extensions->isTimerQuerySupported << ", ARB timer "
                                    << extensions->isARBTimerQuerySupported);
    ASSERT_NE(extensions->glQueryCounter, nullptr);
    ASSERT_NE(extensions->glGetQueryObjectui64v, nullptr);
    const GLenum completion = completionFence->wait(*context->getState());
    EXPECT_TRUE(completion == GL_ALREADY_SIGNALED || completion == GL_CONDITION_SATISFIED)
        << "Offscreen frame submission must not require a readback call to flush its commands: " << completion;
    completionFence->releaseGLObjects(context->getState());
    camera->removeFinalDrawCallback(completionFence);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
    context->releaseContext();
    for (unsigned i = 0; i < SceneUtil::diagnosticGpuTimingReadbackLatencyFrames + 1; ++i)
        viewer.frame();

    double begin = 0;
    double end = 0;
    double duration = 0;
    double queryState = 0;
    EXPECT_TRUE(camera->getStats()->getAttribute(
        sampleFrame, std::string(SceneUtil::diagnosticGpuQueryStateStatistic), queryState));
    EXPECT_EQ(queryState, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Complete));
    EXPECT_TRUE(camera->getStats()->getAttribute(
        sampleFrame, std::string(SceneUtil::diagnosticGpuPassBeginTimestampStatistic), begin));
    EXPECT_TRUE(camera->getStats()->getAttribute(
        sampleFrame, std::string(SceneUtil::diagnosticGpuPassEndTimestampStatistic), end));
    EXPECT_TRUE(camera->getStats()->getAttribute(
        sampleFrame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), duration));
    EXPECT_TRUE(std::isfinite(begin));
    EXPECT_TRUE(std::isfinite(end));
    EXPECT_GE(end, begin);
    EXPECT_GE(duration, 0);
    EXPECT_NEAR(end - begin, duration, 1e-6);
}
