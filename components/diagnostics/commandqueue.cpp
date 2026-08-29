#include "commandqueue.hpp"

#include <algorithm>
#include <stdexcept>

namespace Diagnostics
{
    CommandQueue::CommandQueue(std::size_t capacity)
        : mCapacity(capacity)
    {
        if (capacity == 0)
            throw std::invalid_argument("diagnostic command queue capacity must be positive");
    }

    ErrorCode CommandQueue::enqueue(Command command)
    {
        std::lock_guard lock(mMutex);
        if (mClosed)
            return ErrorCode::InvalidState;
        if (mPending.size() >= mCapacity)
            return ErrorCode::QueueFull;
        if (!mActiveRequestIds.insert(command.mRequestId).second)
            return ErrorCode::InvalidFrame;
        mPending.emplace_back(std::move(command));
        return ErrorCode::None;
    }

    bool CommandQueue::cancel(std::uint64_t requestId)
    {
        std::lock_guard lock(mMutex);
        if (!mActiveRequestIds.contains(requestId))
            return false;

        const auto pending = std::find_if(mPending.begin(), mPending.end(),
            [requestId](const Command& command) { return command.mRequestId == requestId; });
        if (pending != mPending.end())
        {
            mPending.erase(pending);
            mActiveRequestIds.erase(requestId);
            return true;
        }

        mCancelledRequestIds.insert(requestId);
        return true;
    }

    std::vector<Command> CommandQueue::drain(std::size_t maximumCount)
    {
        std::lock_guard lock(mMutex);
        const std::size_t count = std::min(maximumCount, mPending.size());
        std::vector<Command> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            result.emplace_back(std::move(mPending.front()));
            mPending.pop_front();
        }
        return result;
    }

    bool CommandQueue::consumeCancellation(std::uint64_t requestId)
    {
        std::lock_guard lock(mMutex);
        const bool cancelled = mCancelledRequestIds.erase(requestId) != 0;
        mActiveRequestIds.erase(requestId);
        return cancelled;
    }

    void CommandQueue::close()
    {
        std::lock_guard lock(mMutex);
        mClosed = true;
        mPending.clear();
        mActiveRequestIds.clear();
        mCancelledRequestIds.clear();
    }

    std::size_t CommandQueue::size() const
    {
        std::lock_guard lock(mMutex);
        return mPending.size();
    }

    bool CommandQueue::isClosed() const
    {
        std::lock_guard lock(mMutex);
        return mClosed;
    }
}
