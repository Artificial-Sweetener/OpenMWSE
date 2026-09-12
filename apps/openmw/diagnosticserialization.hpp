#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICSERIALIZATION_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICSERIALIZATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OMW
{
    /// Convert UTF-8 text into the protocol's byte payload without an implicit terminator.
    std::vector<std::byte> diagnosticBytes(std::string_view text);
    /// Convert a protocol payload to text after rejecting embedded null bytes.
    std::string diagnosticText(std::span<const std::byte> payload);
    /// Quote a scalar for the small YAML manifests emitted by core diagnostics.
    std::string quoteDiagnosticYaml(std::string_view value);
    /// Nest a complete YAML document without changing scalar precision or collection styles.
    std::string indentDiagnosticYaml(const std::string& document);
    /// Format the repository's 128-bit content identity without ambiguous variable-width fields.
    std::string formatDiagnosticHash(const std::array<std::uint64_t, 2>& hash);
}

#endif
