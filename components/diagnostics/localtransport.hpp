#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_LOCALTRANSPORT_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_LOCALTRANSPORT_HPP

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Diagnostics
{
    /// Owns one authenticated local byte-stream connection.
    class LocalConnection
    {
    public:
        virtual ~LocalConnection() = default;

        /// Receive one complete bounded protocol frame.
        virtual std::vector<std::byte> receive() = 0;

        /// Send one complete protocol frame or fail without truncating silently.
        virtual void send(std::span<const std::byte> frame) = 0;
    };

    /// Owns a current-user-only local endpoint and accepts authenticated connections.
    class LocalListener
    {
    public:
        virtual ~LocalListener() = default;

        /// Block until one client connects or the listener is closed.
        virtual std::unique_ptr<LocalConnection> accept() = 0;

        /// Interrupt pending accepts and prevent new connections.
        virtual void close() noexcept = 0;
    };

    /// Return the stable per-user endpoint used by explicitly enabled live sessions.
    std::string defaultLocalEndpoint();

    /// Create a current-user-only listener for a validated platform endpoint.
    std::unique_ptr<LocalListener> listenLocally(const std::string& endpoint);

    /// Connect to a local endpoint within a finite timeout.
    std::unique_ptr<LocalConnection> connectLocally(
        const std::string& endpoint, std::chrono::milliseconds timeout = std::chrono::seconds(10));
}

#endif
