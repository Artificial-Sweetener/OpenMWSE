#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICCTL_ISOLATEDSETTINGS_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICCTL_ISOLATEDSETTINGS_H

#include <filesystem>
#include <string>
#include <string_view>

namespace DiagnosticCtl
{
    /// Identifies one validated setting overlay for an isolated diagnostic profile.
    struct IsolatedSetting
    {
        std::string mSection;
        std::string mName;
        std::string mValue;
    };

    /// Parse a `Section:name=value` overlay without permitting settings-file syntax injection.
    IsolatedSetting parseIsolatedSetting(std::string_view value);

    /// Append one setting overlay to the writable settings file of an isolated profile.
    void applyIsolatedSetting(const std::filesystem::path& configDirectory, const IsolatedSetting& setting);
}

#endif
