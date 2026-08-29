#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_SESSION_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_SESSION_HPP

#include "commandqueue.hpp"
#include "localtransport.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Diagnostics
{
    /// Bridges local clients to a bounded safe-point command queue without executing game code on IPC threads.
    class Session
    {
    public:
        /// Start a protected local endpoint and its bounded transport workers.
        explicit Session(std::string endpoint, std::size_t workerCount = 2);

        /// Stop transport activity and cancel every pending exchange.
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        /// Drain validated commands for execution at the owning engine safe point.
        std::vector<Command> drain(std::size_t maximumCount = 64);

        /// Publish one completed command result to its waiting local client.
        void respond(Response response);

        /// Return whether an executing request has been cancelled by its client.
        bool consumeCancellation(std::uint64_t requestId);

        /// Stop accepting work and wake all clients with terminal errors.
        void stop() noexcept;

        /// Return the protected endpoint served by this session.
        const std::string& endpoint() const noexcept;

    private:
        /// Accept local connections without parsing or executing their requests.
        void acceptConnections(std::stop_token stopToken);

        /// Validate and relay one request at a time from the bounded connection queue.
        void processConnections(std::stop_token stopToken);

        /// Wait for one safe-point result or session shutdown.
        Response waitForResponse(std::uint64_t requestId, std::stop_token stopToken);

        std::string mEndpoint;
        CommandQueue mCommands;
        std::unique_ptr<LocalListener> mListener;
        std::jthread mAcceptThread;
        std::vector<std::jthread> mWorkers;
        std::mutex mConnectionMutex;
        std::condition_variable_any mConnectionCondition;
        std::deque<std::unique_ptr<LocalConnection>> mConnections;
        std::mutex mResponseMutex;
        std::condition_variable_any mResponseCondition;
        std::unordered_map<std::uint64_t, Response> mResponses;
        bool mStopped = false;
    };

    /// Send one command over a short-lived local connection and validate its matching response.
    Response transact(const std::string& endpoint, const Command& command,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
}

#endif
