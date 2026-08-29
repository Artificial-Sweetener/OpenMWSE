#include "isolatedsettings.hpp"

#include <fstream>
#include <stdexcept>

namespace
{
    /// Reject characters that could escape the intended settings section or assignment.
    void requireSafeField(std::string_view field, std::string_view label, std::string_view forbidden)
    {
        if (field.empty() || field.find_first_of("\r\n") != std::string_view::npos
            || field.find_first_of(forbidden) != std::string_view::npos)
        {
            throw std::invalid_argument("invalid isolated setting " + std::string(label));
        }
    }
}

namespace DiagnosticCtl
{
    IsolatedSetting parseIsolatedSetting(std::string_view value)
    {
        const std::size_t sectionEnd = value.find(':');
        const std::size_t nameEnd = value.find('=', sectionEnd == std::string_view::npos ? 0 : sectionEnd + 1);
        if (sectionEnd == std::string_view::npos || nameEnd == std::string_view::npos)
            throw std::invalid_argument("isolated setting must use Section:name=value");

        IsolatedSetting result{ std::string(value.substr(0, sectionEnd)),
            std::string(value.substr(sectionEnd + 1, nameEnd - sectionEnd - 1)), std::string(value.substr(nameEnd + 1)) };
        requireSafeField(result.mSection, "section", "[]:=");
        requireSafeField(result.mName, "name", "[]:=");
        requireSafeField(result.mValue, "value", "");
        return result;
    }

    void applyIsolatedSetting(const std::filesystem::path& configDirectory, const IsolatedSetting& setting)
    {
        std::ofstream stream(configDirectory / "settings.cfg", std::ios::app);
        stream << "\n[" << setting.mSection << "]\n" << setting.mName << " = " << setting.mValue << '\n';
        if (!stream)
            throw std::runtime_error("failed applying isolated setting overlay");
    }
}
