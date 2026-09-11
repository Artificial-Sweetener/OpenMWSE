#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace Diagnostics
{
    namespace
    {
        constexpr std::array<std::byte, 4> sCommandMagic{ std::byte{ 'O' }, std::byte{ 'M' }, std::byte{ 'W' },
            std::byte{ 'D' } };
        constexpr std::array<std::byte, 4> sResponseMagic{ std::byte{ 'O' }, std::byte{ 'M' }, std::byte{ 'W' },
            std::byte{ 'R' } };
        constexpr std::size_t sHeaderSize = 20;

        /// Append an unsigned integer using the protocol's fixed little-endian representation.
        template <class T>
        void appendInteger(std::vector<std::byte>& output, T value)
        {
            static_assert(std::is_unsigned_v<T>);
            for (std::size_t byte = 0; byte < sizeof(T); ++byte)
                output.push_back(static_cast<std::byte>((value >> (byte * 8)) & 0xffu));
        }

        /// Read an unsigned integer from a validated offset using the protocol byte order.
        template <class T>
        T readInteger(std::span<const std::byte> input, std::size_t& offset)
        {
            static_assert(std::is_unsigned_v<T>);
            if (input.size() - offset < sizeof(T))
                throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic frame ended inside an integer");

            T value = 0;
            for (std::size_t byte = 0; byte < sizeof(T); ++byte)
                value |= static_cast<T>(std::to_integer<unsigned char>(input[offset++])) << (byte * 8);
            return value;
        }
    }

    ProtocolError::ProtocolError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message))
        , mCode(code)
    {
    }

    ErrorCode ProtocolError::code() const noexcept
    {
        return mCode;
    }

    std::vector<std::byte> encodeCommand(const Command& command)
    {
        if (!isSupportedCommand(static_cast<std::uint16_t>(command.mType)))
            throw ProtocolError(ErrorCode::UnsupportedCommand, "diagnostic command type is not supported");
        if (command.mPayload.size() > maximumPayloadSize)
            throw ProtocolError(ErrorCode::PayloadTooLarge, "diagnostic command payload exceeds the protocol limit");

        std::vector<std::byte> output;
        output.reserve(sHeaderSize + command.mPayload.size());
        output.insert(output.end(), sCommandMagic.begin(), sCommandMagic.end());
        appendInteger(output, protocolVersion);
        appendInteger(output, static_cast<std::uint16_t>(command.mType));
        appendInteger(output, command.mRequestId);
        appendInteger(output, static_cast<std::uint32_t>(command.mPayload.size()));
        output.insert(output.end(), command.mPayload.begin(), command.mPayload.end());
        return output;
    }

    Command decodeCommand(std::span<const std::byte> frame)
    {
        if (frame.size() < sHeaderSize)
            throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic frame is shorter than its header");
        if (!std::equal(sCommandMagic.begin(), sCommandMagic.end(), frame.begin()))
            throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic frame has an invalid signature");

        std::size_t offset = sCommandMagic.size();
        const std::uint16_t version = readInteger<std::uint16_t>(frame, offset);
        if (version != protocolVersion)
            throw ProtocolError(ErrorCode::UnsupportedVersion, "diagnostic protocol version is not supported");

        const std::uint16_t type = readInteger<std::uint16_t>(frame, offset);
        if (!isSupportedCommand(type))
            throw ProtocolError(ErrorCode::UnsupportedCommand, "diagnostic command type is not supported");

        Command result;
        result.mType = static_cast<CommandType>(type);
        result.mRequestId = readInteger<std::uint64_t>(frame, offset);
        const std::uint32_t payloadSize = readInteger<std::uint32_t>(frame, offset);
        if (payloadSize > maximumPayloadSize)
            throw ProtocolError(ErrorCode::PayloadTooLarge, "diagnostic command payload exceeds the protocol limit");
        if (frame.size() - offset != payloadSize)
            throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic frame length does not match its payload length");

        result.mPayload.assign(frame.begin() + static_cast<std::ptrdiff_t>(offset), frame.end());
        return result;
    }

    std::vector<std::byte> encodeResponse(const Response& response)
    {
        if (response.mPayload.size() > maximumPayloadSize)
            throw ProtocolError(ErrorCode::PayloadTooLarge, "diagnostic response payload exceeds the protocol limit");

        std::vector<std::byte> output;
        output.reserve(sHeaderSize + response.mPayload.size());
        output.insert(output.end(), sResponseMagic.begin(), sResponseMagic.end());
        appendInteger(output, protocolVersion);
        appendInteger(output, static_cast<std::uint16_t>(response.mError));
        appendInteger(output, response.mRequestId);
        appendInteger(output, static_cast<std::uint32_t>(response.mPayload.size()));
        output.insert(output.end(), response.mPayload.begin(), response.mPayload.end());
        return output;
    }

    Response decodeResponse(std::span<const std::byte> frame)
    {
        if (frame.size() < sHeaderSize)
            throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic response is shorter than its header");
        if (!std::equal(sResponseMagic.begin(), sResponseMagic.end(), frame.begin()))
            throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic response has an invalid signature");

        std::size_t offset = sResponseMagic.size();
        if (readInteger<std::uint16_t>(frame, offset) != protocolVersion)
            throw ProtocolError(ErrorCode::UnsupportedVersion, "diagnostic protocol version is not supported");

        Response result;
        result.mError = static_cast<ErrorCode>(readInteger<std::uint16_t>(frame, offset));
        result.mRequestId = readInteger<std::uint64_t>(frame, offset);
        const std::uint32_t payloadSize = readInteger<std::uint32_t>(frame, offset);
        if (payloadSize > maximumPayloadSize)
            throw ProtocolError(ErrorCode::PayloadTooLarge, "diagnostic response payload exceeds the protocol limit");
        if (frame.size() - offset != payloadSize)
            throw ProtocolError(
                ErrorCode::InvalidFrame, "diagnostic response length does not match its payload length");
        result.mPayload.assign(frame.begin() + static_cast<std::ptrdiff_t>(offset), frame.end());
        return result;
    }

    bool isSupportedCommand(std::uint16_t value) noexcept
    {
        return value >= static_cast<std::uint16_t>(CommandType::Hello)
            && value <= static_cast<std::uint16_t>(CommandType::SetDistanceSettings);
    }

    std::string_view getErrorName(ErrorCode code) noexcept
    {
        switch (code)
        {
            case ErrorCode::None:
                return "none";
            case ErrorCode::UnsupportedVersion:
                return "unsupported-version";
            case ErrorCode::InvalidFrame:
                return "invalid-frame";
            case ErrorCode::PayloadTooLarge:
                return "payload-too-large";
            case ErrorCode::UnsupportedCommand:
                return "unsupported-command";
            case ErrorCode::QueueFull:
                return "queue-full";
            case ErrorCode::Cancelled:
                return "cancelled";
            case ErrorCode::InvalidState:
                return "invalid-state";
            case ErrorCode::PermissionDenied:
                return "permission-denied";
            case ErrorCode::ContentMismatch:
                return "content-mismatch";
            case ErrorCode::InternalFailure:
                return "internal-failure";
        }
        return "unknown";
    }
}
