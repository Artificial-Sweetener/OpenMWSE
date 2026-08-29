#include <apps/openmw-diagnosticctl/isolatedsettings.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
    /// Own a unique temporary configuration directory for isolated-setting tests.
    class TemporaryConfigDirectory
    {
    public:
        TemporaryConfigDirectory()
            : mPath(std::filesystem::temp_directory_path()
                  / ("openmw-diagnostic-settings-"
                      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
        {
            std::filesystem::create_directories(mPath);
        }

        /// Remove the isolated configuration after each test.
        ~TemporaryConfigDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(mPath, error);
        }

        /// Return the writable configuration directory.
        const std::filesystem::path& path() const { return mPath; }

    private:
        std::filesystem::path mPath;
    };
}

TEST(DiagnosticIsolatedSettingsTest, ParsesAndAppliesValidatedOverlay)
{
    const DiagnosticCtl::IsolatedSetting setting
        = DiagnosticCtl::parseIsolatedSetting("Post Processing:transparent postpass=false");
    EXPECT_EQ(setting.mSection, "Post Processing");
    EXPECT_EQ(setting.mName, "transparent postpass");
    EXPECT_EQ(setting.mValue, "false");

    TemporaryConfigDirectory config;
    DiagnosticCtl::applyIsolatedSetting(config.path(), setting);

    std::ifstream stream(config.path() / "settings.cfg");
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, "\n[Post Processing]\ntransparent postpass = false\n");
}

TEST(DiagnosticIsolatedSettingsTest, RejectsMalformedAndInjectableOverlays)
{
    EXPECT_THROW(DiagnosticCtl::parseIsolatedSetting("transparent postpass=false"), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::parseIsolatedSetting("Post Processing:transparent postpass=false\n[Video]"),
        std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::parseIsolatedSetting("[Post Processing]:transparent postpass=false"),
        std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::parseIsolatedSetting("Post Processing:transparent:postpass=false"),
        std::invalid_argument);
}
