#include <components/sceneutil/offscreencontext.hpp>

#include <gtest/gtest.h>

#include <osg/GL>

#ifdef _WIN32
#include <components/misc/windows.hpp>

#include <set>
#endif

namespace
{
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
