#include <gtest/gtest.h>

#include <apps/openmw/mwinput/diagnosticinputmanager.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include <osgViewer/Viewer>

namespace
{
    /// Own a unique writable binding path for one diagnostic input test.
    class TemporaryBindingsFile
    {
    public:
        TemporaryBindingsFile()
            : mPath(std::filesystem::temp_directory_path()
                  / ("openmw-diagnostic-input-"
                      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".xml"))
        {
        }

        /// Remove files written by the bindings owner after the input manager is destroyed.
        ~TemporaryBindingsFile()
        {
            std::error_code error;
            std::filesystem::remove(mPath, error);
            std::filesystem::remove(mPath.string() + ".new", error);
        }

        /// Return the isolated binding path.
        const std::filesystem::path& path() const { return mPath; }

    private:
        std::filesystem::path mPath;
    };
}

TEST(DiagnosticInputManagerTest, AdvancesDeterministicStateWithoutDevices)
{
    TemporaryBindingsFile bindings;
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    MWInput::DiagnosticInputManager input(viewer, nullptr, bindings.path(), false);

    EXPECT_TRUE(input.getControlSwitch("playercontrols"));
    EXPECT_FALSE(input.controlsDisabled());
    EXPECT_FALSE(input.isIdle());
    EXPECT_EQ(input.getMouseMoveX(), 0);
    EXPECT_EQ(input.getMouseMoveY(), 0);
    EXPECT_FALSE(input.joystickLastUsed());

    input.update(0.6f, false);
    EXPECT_TRUE(input.isIdle());
    input.resetIdleTime();
    EXPECT_FALSE(input.isIdle());

    input.update(1.f, true);
    EXPECT_TRUE(input.controlsDisabled());
    EXPECT_FALSE(input.isIdle());
}

TEST(DiagnosticInputManagerTest, PreservesControlSwitchState)
{
    TemporaryBindingsFile bindings;
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    MWInput::DiagnosticInputManager input(viewer, nullptr, bindings.path(), false);

    input.toggleControlSwitch("playercontrols", false);
    EXPECT_FALSE(input.getControlSwitch("playercontrols"));
    EXPECT_EQ(input.countSavedGameRecords(), 1u);

    input.clear();
    EXPECT_TRUE(input.getControlSwitch("playercontrols"));
}
