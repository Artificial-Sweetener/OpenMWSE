#include "isolatedsettings.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

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

    /// Return one settings-file field without surrounding ASCII whitespace.
    std::string_view trim(std::string_view value)
    {
        constexpr std::string_view whitespace = " \t";
        const std::size_t begin = value.find_first_not_of(whitespace);
        if (begin == std::string_view::npos)
            return {};
        return value.substr(begin, value.find_last_not_of(whitespace) - begin + 1);
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
        const std::filesystem::path path = configDirectory / "settings.cfg";
        std::vector<std::string> lines;
        if (std::ifstream input(path); input)
        {
            for (std::string line; std::getline(input, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(std::move(line));
            }
            if (!input.eof())
                throw std::runtime_error("failed reading isolated settings file");
        }

        bool inTargetSection = false;
        bool foundSection = false;
        std::size_t sectionEnd = lines.size();
        bool replaced = false;
        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            const std::string_view line = trim(lines[index]);
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
            {
                if (inTargetSection && sectionEnd == lines.size())
                    sectionEnd = index;
                inTargetSection = line.substr(1, line.size() - 2) == setting.mSection;
                foundSection = foundSection || inTargetSection;
                continue;
            }
            if (!inTargetSection)
                continue;

            const std::size_t assignment = line.find('=');
            if (assignment != std::string_view::npos && trim(line.substr(0, assignment)) == setting.mName)
            {
                lines[index] = setting.mName + " = " + setting.mValue;
                replaced = true;
                break;
            }
        }

        if (!replaced && foundSection)
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), setting.mName + " = " + setting.mValue);
        else if (!replaced)
        {
            if (!lines.empty() && !lines.back().empty())
                lines.emplace_back();
            lines.push_back("[" + setting.mSection + "]");
            lines.push_back(setting.mName + " = " + setting.mValue);
        }

        std::ofstream output(path, std::ios::trunc);
        for (const std::string& line : lines)
            output << line << '\n';
        if (!output)
            throw std::runtime_error("failed applying isolated setting overlay");
    }
}
