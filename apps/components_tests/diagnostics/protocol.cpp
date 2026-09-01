#include <components/diagnostics/protocol.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(DiagnosticProtocol, RoundTripsCanonicalCommand)
    {
        Diagnostics::Command command;
        command.mRequestId = 0x0102030405060708ull;
        command.mType = Diagnostics::CommandType::CaptureGraphics;
        command.mPayload = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };

        EXPECT_EQ(Diagnostics::decodeCommand(Diagnostics::encodeCommand(command)), command);
    }

    TEST(DiagnosticProtocol, SupportsVersionedAtmosphereScenarioCommand)
    {
        Diagnostics::Command command;
        command.mRequestId = 9;
        command.mType = Diagnostics::CommandType::SetAtmosphereScenario;
        command.mPayload = { std::byte{ 'v' }, std::byte{ '2' } };

        EXPECT_EQ(Diagnostics::decodeCommand(Diagnostics::encodeCommand(command)), command);
        EXPECT_TRUE(Diagnostics::isSupportedCommand(
            static_cast<std::uint16_t>(Diagnostics::CommandType::SetAtmosphereScenario)));
    }

    TEST(DiagnosticProtocol, SupportsIsolatedCameraCommand)
    {
        Diagnostics::Command command;
        command.mRequestId = 10;
        command.mType = Diagnostics::CommandType::SetCamera;
        command.mPayload = { std::byte{ '[' }, std::byte{ ']' } };

        EXPECT_EQ(Diagnostics::decodeCommand(Diagnostics::encodeCommand(command)), command);
        EXPECT_TRUE(Diagnostics::isSupportedCommand(static_cast<std::uint16_t>(Diagnostics::CommandType::SetCamera)));
    }

    TEST(DiagnosticProtocol, RejectsDeclaredLengthMismatch)
    {
        Diagnostics::Command command{ 1, Diagnostics::CommandType::Hello, { std::byte{ 1 } } };
        std::vector<std::byte> frame = Diagnostics::encodeCommand(command);
        frame.pop_back();

        try
        {
            static_cast<void>(Diagnostics::decodeCommand(frame));
            FAIL() << "Expected a protocol error";
        }
        catch (const Diagnostics::ProtocolError& error)
        {
            EXPECT_EQ(error.code(), Diagnostics::ErrorCode::InvalidFrame);
        }
    }

    TEST(DiagnosticProtocol, RejectsUnknownCommand)
    {
        Diagnostics::Command command{ 1, static_cast<Diagnostics::CommandType>(999), {} };
        EXPECT_THROW(static_cast<void>(Diagnostics::encodeCommand(command)), Diagnostics::ProtocolError);
    }

    TEST(DiagnosticProtocol, RejectsPayloadBeyondTheWireLimit)
    {
        Diagnostics::Command command{ 1, Diagnostics::CommandType::Hello,
            std::vector<std::byte>(Diagnostics::maximumPayloadSize + 1) };
        try
        {
            static_cast<void>(Diagnostics::encodeCommand(command));
            FAIL() << "Expected a protocol error";
        }
        catch (const Diagnostics::ProtocolError& error)
        {
            EXPECT_EQ(error.code(), Diagnostics::ErrorCode::PayloadTooLarge);
        }
    }

    TEST(DiagnosticProtocol, RoundTripsStructuredResponse)
    {
        Diagnostics::Response response{ 7, Diagnostics::ErrorCode::ContentMismatch,
            { std::byte{ 'm' }, std::byte{ 'i' }, std::byte{ 's' }, std::byte{ 'm' }, std::byte{ 'a' },
                std::byte{ 't' }, std::byte{ 'c' }, std::byte{ 'h' } } };
        EXPECT_EQ(Diagnostics::decodeResponse(Diagnostics::encodeResponse(response)), response);
    }
}
