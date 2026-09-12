#ifndef OPENMW_DIAGNOSTICCTL_CONTROLLERPROTOCOL_H
#define OPENMW_DIAGNOSTICCTL_CONTROLLERPROTOCOL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/localtransport.hpp>
#include <components/diagnostics/session.hpp>

namespace DiagnosticCtl
{
    class ChildProcess;

    /// Convert a text argument into a bounded protocol payload.
    std::vector<std::byte> textPayload(std::string_view text);
    /// Convert a protocol payload into printable text without assuming a terminator.
    std::string payloadText(std::span<const std::byte> payload);
    /// Parse one named command-line option without accepting ambiguous duplicates.
    std::optional<std::string> findOption(std::span<char*> arguments, std::string_view name);
    /// Parse every occurrence of a repeatable named command-line option.
    std::vector<std::string> findOptions(std::span<char*> arguments, std::string_view name);
    /// Return whether one non-valued command-line flag appears exactly once.
    bool hasFlag(std::span<char*> arguments, std::string_view name);
    /// Parse one finite floating-point option and reject trailing non-numeric text.
    double parseFiniteOption(std::string_view name, const std::string& text);
    /// Parse one unsigned frame-count option without allowing narrowing or trailing text.
    std::uint32_t parseFrameCountOption(std::string_view name, const std::string& text, std::uint32_t maximum);
    /// Build one exact atmosphere scenario from mutually exclusive weather and environment options.
    std::optional<Diagnostics::AtmosphereScenario> parseAtmosphereScenarioOptions(std::span<char*> arguments);
    /// Generate a unique protected endpoint within the transport's reserved namespace.
    std::string uniqueEndpoint();
    /// Send a command after the child or live session has reached its safe-point loop.
    Diagnostics::Response transactWhenReady(const std::string& endpoint, const Diagnostics::Command& command,
        std::chrono::seconds readinessTimeout, ChildProcess* child = nullptr, bool allowVisibleChild = false);
    /// Require the engine to attest to the isolated graphics and input owners used by the child.
    void validateIsolatedSession(const Diagnostics::Response& response, bool headed);
    /// Print one structured response and convert protocol errors to a failing exit status.
    int printResponse(const Diagnostics::Response& response);
    /// Print one response and stop an automated workflow when the engine rejected the command.
    void requireSuccessfulResponse(const Diagnostics::Response& response);
    /// Print the stable controller command surface and isolation workflow options.
    void printUsage(std::ostream& stream);
    /// Wait for every production render stage to restore normal state before continuing.
    void waitForGraphicsCapture(const std::string& endpoint, std::uint64_t& requestId);
    /// Wait until an exact final-composite sequence has finished serializing.
    void waitForRenderFrameCapture(const std::string& endpoint, std::uint64_t& requestId);
    /// Wait for an exact cumulative fixed-frame count without advancing simulation after completion.
    void waitForScheduledFrames(
        const std::string& endpoint, std::uint64_t& requestId, std::uint64_t expectedCompletedFrames);
    /// Compare stable replay state while allowing an explicit camera command to restore transient placement.
    void validateReplayState(
        std::span<const std::byte> expectedBytes, std::span<const std::byte> actualBytes, bool validateCamera);
}

#endif
