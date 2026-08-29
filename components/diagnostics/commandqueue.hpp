#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_COMMANDQUEUE_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_COMMANDQUEUE_HPP

#include "protocol.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Diagnostics
{
    /// Transfers validated commands from transport threads to the engine's documented safe point.
    class CommandQueue
    {
    public:
        /// Create a bounded queue that rejects work instead of allowing unbounded memory growth.
        explicit CommandQueue(std::size_t capacity = 256);

        /// Queue an immutable command when capacity and request identity permit it.
        ErrorCode enqueue(Command command);

        /// Mark a request cancelled and remove it if execution has not begun.
        bool cancel(std::uint64_t requestId);

        /// Drain commands for execution by the single safe-point owner.
        std::vector<Command> drain(std::size_t maximumCount = 64);

        /// Return whether a drained request was cancelled before its owner completed it.
        bool consumeCancellation(std::uint64_t requestId);

        /// Reject new commands and discard pending work during transport or engine shutdown.
        void close();

        /// Return the number of commands awaiting safe-point execution.
        std::size_t size() const;

        /// Return whether the queue has entered its terminal closed state.
        bool isClosed() const;

    private:
        const std::size_t mCapacity;
        mutable std::mutex mMutex;
        std::deque<Command> mPending;
        std::unordered_set<std::uint64_t> mActiveRequestIds;
        std::unordered_set<std::uint64_t> mCancelledRequestIds;
        bool mClosed = false;
    };
}

#endif
