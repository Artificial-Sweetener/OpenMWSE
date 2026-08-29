#include <components/diagnostics/session.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    /// Return a reserved, collision-free endpoint for one parallel-safe session test.
    std::string uniqueTestEndpoint()
    {
        static std::atomic_uint64_t counter = 0;
        return Diagnostics::defaultLocalEndpoint() + "-test-" + std::to_string(++counter);
    }

    TEST(DiagnosticSession, ExecutesOnlyAfterSafePointDrain)
    {
        const std::string endpoint = uniqueTestEndpoint();
        Diagnostics::Session session(endpoint);
        Diagnostics::Response clientResponse;
        std::jthread client([&] {
            clientResponse
                = Diagnostics::transact(endpoint, { 42, Diagnostics::CommandType::InspectSession, { std::byte{ 9 } } });
        });

        std::vector<Diagnostics::Command> commands;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (commands.empty() && std::chrono::steady_clock::now() < deadline)
        {
            commands = session.drain();
            std::this_thread::yield();
        }
        ASSERT_EQ(commands.size(), 1);
        EXPECT_EQ(commands.front().mPayload, std::vector<std::byte>{ std::byte{ 9 } });
        session.respond({ 42, Diagnostics::ErrorCode::None, { std::byte{ 7 } } });
        client.join();
        EXPECT_EQ(clientResponse.mPayload, std::vector<std::byte>{ std::byte{ 7 } });
    }

    TEST(DiagnosticSession, StopCancelsAClientWaitingForSafePointWork)
    {
        const std::string endpoint = uniqueTestEndpoint();
        Diagnostics::Session session(endpoint);
        Diagnostics::Response clientResponse;
        std::jthread client([&] {
            clientResponse = Diagnostics::transact(endpoint, { 84, Diagnostics::CommandType::InspectSession, {} });
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (session.drain().empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        session.stop();
        client.join();
        EXPECT_EQ(clientResponse.mError, Diagnostics::ErrorCode::Cancelled);
    }

    TEST(DiagnosticSession, ExecutingCancellationOverridesACompletedResponse)
    {
        const std::string endpoint = uniqueTestEndpoint();
        Diagnostics::Session session(endpoint);
        Diagnostics::Response originalResponse;
        std::jthread originalClient([&] {
            originalResponse = Diagnostics::transact(endpoint, { 91, Diagnostics::CommandType::InspectSession, {} });
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (session.drain().empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();

        Diagnostics::Command cancel{ 92, Diagnostics::CommandType::Cancel, {} };
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte)
            cancel.mPayload.push_back(static_cast<std::byte>((91ull >> (byte * 8)) & 0xffu));
        EXPECT_EQ(Diagnostics::transact(endpoint, cancel).mError, Diagnostics::ErrorCode::None);
        session.respond({ 91, Diagnostics::ErrorCode::None, { std::byte{ 1 } } });
        originalClient.join();
        EXPECT_EQ(originalResponse.mError, Diagnostics::ErrorCode::Cancelled);
        EXPECT_TRUE(originalResponse.mPayload.empty());
    }

    TEST(DiagnosticSession, RejectsEndpointsOutsideTheReservedLocalNamespace)
    {
        EXPECT_THROW(static_cast<void>(Diagnostics::listenLocally("not-an-openmwse-endpoint")), std::invalid_argument);
    }
}
