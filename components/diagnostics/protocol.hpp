#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_PROTOCOL_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Diagnostics
{
    inline constexpr std::uint16_t protocolVersion = 2;
    inline constexpr std::size_t maximumPayloadSize = 16 * 1024 * 1024;

    /// Identifies a validated bridge operation without exposing an unrestricted execution surface.
    enum class CommandType : std::uint16_t
    {
        Hello = 1,
        InspectSession = 2,
        CaptureCapsule = 3,
        CaptureGraphics = 4,
        TraceGameplay = 5,
        TracePerformance = 6,
        StepFrames = 7,
        SetCamera = 8,
        SetWorldState = 9,
        ReloadShaders = 10,
        Cancel = 11,
        ShutdownChild = 12,
        LoadGameplayRoute = 13,
        RunGameplayRouteSegment = 14,
        SetAtmosphereScenario = 15,
        SetAtmosphereSettings = 16,
        ShowVideoSettings = 17,
    };

    /// Classifies protocol failures so clients can distinguish invalid requests from runtime failures.
    enum class ErrorCode : std::uint16_t
    {
        None = 0,
        UnsupportedVersion = 1,
        InvalidFrame = 2,
        PayloadTooLarge = 3,
        UnsupportedCommand = 4,
        QueueFull = 5,
        Cancelled = 6,
        InvalidState = 7,
        PermissionDenied = 8,
        ContentMismatch = 9,
        InternalFailure = 10,
    };

    /// Carries one immutable, validated command from an IPC reader to an engine safe point.
    struct Command
    {
        std::uint64_t mRequestId = 0;
        CommandType mType = CommandType::Hello;
        std::vector<std::byte> mPayload;

        /// Compare canonical command identity and payload fields.
        bool operator==(const Command&) const = default;
    };

    /// Carries one structured command result from an engine safe point to a bridge client.
    struct Response
    {
        std::uint64_t mRequestId = 0;
        ErrorCode mError = ErrorCode::None;
        std::vector<std::byte> mPayload;

        /// Compare canonical response identity, status, and payload fields.
        bool operator==(const Response&) const = default;
    };

    /// Reports a rejected protocol frame with a stable machine-readable reason.
    class ProtocolError : public std::runtime_error
    {
    public:
        /// Create a protocol exception carrying the supplied stable error code.
        ProtocolError(ErrorCode code, std::string message);

        /// Return the stable reason associated with this protocol failure.
        ErrorCode code() const noexcept;

    private:
        ErrorCode mCode;
    };

    /// Encode a command into the canonical little-endian bridge frame.
    std::vector<std::byte> encodeCommand(const Command& command);

    /// Decode and fully validate one canonical bridge frame.
    Command decodeCommand(std::span<const std::byte> frame);

    /// Encode a command result into the canonical little-endian bridge frame.
    std::vector<std::byte> encodeResponse(const Response& response);

    /// Decode and fully validate one canonical command-result frame.
    Response decodeResponse(std::span<const std::byte> frame);

    /// Return whether a numeric command identifier belongs to this protocol version.
    bool isSupportedCommand(std::uint16_t value) noexcept;

    /// Convert a stable error code to its protocol spelling.
    std::string_view getErrorName(ErrorCode code) noexcept;
}

#endif
