#include "diagnosticserialization.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

std::vector<std::byte> OMW::diagnosticBytes(std::string_view text)
{
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return { begin, begin + text.size() };
}

std::string OMW::diagnosticText(std::span<const std::byte> payload)
{
    std::string result(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (result.find('\0') != std::string::npos)
        throw std::invalid_argument("diagnostic text payload contains an embedded null byte");
    return result;
}

std::string OMW::quoteDiagnosticYaml(std::string_view value)
{
    std::string result = "\"";
    for (const char character : value)
    {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        if (character == '\n')
            result += "\\n";
        else if (character != '\r')
            result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string OMW::indentDiagnosticYaml(const std::string& document)
{
    std::istringstream input(document);
    std::ostringstream output;
    for (std::string line; std::getline(input, line);)
        output << "  " << line << '\n';
    return output.str();
}

std::string OMW::formatDiagnosticHash(const std::array<std::uint64_t, 2>& hash)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash[0] << std::setw(16) << hash[1];
    return output.str();
}
