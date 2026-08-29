#include "session.hpp"

#include <components/debug/debuglog.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Diagnostics
{
    Session::Session(std::string endpoint, std::size_t workerCount)
        : mEndpoint(std::move(endpoint))
        , mListener(listenLocally(mEndpoint))
    {
        if (workerCount == 0 || workerCount > 8)
            throw std::invalid_argument("diagnostic session worker count is outside its bounds");

        mAcceptThread = std::jthread([this](std::stop_token stopToken) { acceptConnections(stopToken); });
        mWorkers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index)
            mWorkers.emplace_back([this](std::stop_token stopToken) { processConnections(stopToken); });
    }

    Session::~Session()
    {
        stop();
    }

    std::vector<Command> Session::drain(std::size_t maximumCount)
    {
        return mCommands.drain(maximumCount);
    }

    void Session::respond(Response response)
    {
        const std::uint64_t requestId = response.mRequestId;
        if (mCommands.consumeCancellation(requestId))
        {
            response.mError = ErrorCode::Cancelled;
            response.mPayload.clear();
        }
        {
            std::lock_guard lock(mResponseMutex);
            if (mStopped)
                return;
            mResponses.insert_or_assign(requestId, std::move(response));
        }
        mResponseCondition.notify_all();
    }

    bool Session::consumeCancellation(std::uint64_t requestId)
    {
        return mCommands.consumeCancellation(requestId);
    }

    void Session::stop() noexcept
    {
        {
            std::scoped_lock lock(mConnectionMutex, mResponseMutex);
            if (mStopped)
                return;
            mStopped = true;
            mConnections.clear();
        }
        mCommands.close();
        mListener->close();
        mAcceptThread.request_stop();
        for (std::jthread& worker : mWorkers)
            worker.request_stop();
        mConnectionCondition.notify_all();
        mResponseCondition.notify_all();
    }

    const std::string& Session::endpoint() const noexcept
    {
        return mEndpoint;
    }

    void Session::acceptConnections(std::stop_token stopToken)
    {
        try
        {
            while (!stopToken.stop_requested())
            {
                std::unique_ptr<LocalConnection> connection = mListener->accept();
                if (!connection)
                    return;
                {
                    std::lock_guard lock(mConnectionMutex);
                    if (mStopped)
                        return;
                    if (mConnections.size() >= 16)
                        continue;
                    mConnections.emplace_back(std::move(connection));
                }
                mConnectionCondition.notify_one();
            }
        }
        catch (const std::exception& error)
        {
            if (!stopToken.stop_requested())
                Log(Debug::Error) << "Diagnostic bridge listener stopped: " << error.what();
        }
    }

    void Session::processConnections(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            std::unique_ptr<LocalConnection> connection;
            {
                std::unique_lock lock(mConnectionMutex);
                mConnectionCondition.wait(lock, stopToken, [this] { return mStopped || !mConnections.empty(); });
                if (mStopped || stopToken.stop_requested())
                    return;
                connection = std::move(mConnections.front());
                mConnections.pop_front();
            }

            try
            {
                Command command = decodeCommand(connection->receive());
                Response response;
                if (command.mType == CommandType::Cancel)
                {
                    response.mRequestId = command.mRequestId;
                    if (command.mPayload.size() != sizeof(std::uint64_t))
                        response.mError = ErrorCode::InvalidFrame;
                    else
                    {
                        std::uint64_t cancelledId = 0;
                        for (std::size_t byte = 0; byte < sizeof(cancelledId); ++byte)
                            cancelledId
                                |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(command.mPayload[byte]))
                                << (byte * 8);
                        response.mError = mCommands.cancel(cancelledId) ? ErrorCode::None : ErrorCode::InvalidState;
                    }
                }
                else
                {
                    response.mRequestId = command.mRequestId;
                    response.mError = mCommands.enqueue(std::move(command));
                    if (response.mError == ErrorCode::None)
                        response = waitForResponse(response.mRequestId, stopToken);
                }
                connection->send(encodeResponse(response));
            }
            catch (const ProtocolError& error)
            {
                try
                {
                    connection->send(encodeResponse({ 0, error.code(), {} }));
                }
                catch (...)
                {
                }
            }
            catch (const std::exception& error)
            {
                Log(Debug::Warning) << "Diagnostic bridge client disconnected: " << error.what();
            }
        }
    }

    Response Session::waitForResponse(std::uint64_t requestId, std::stop_token stopToken)
    {
        std::unique_lock lock(mResponseMutex);
        mResponseCondition.wait(
            lock, stopToken, [this, requestId] { return mStopped || mResponses.contains(requestId); });
        if (mStopped || stopToken.stop_requested())
            return { requestId, ErrorCode::Cancelled, {} };
        Response result = std::move(mResponses.at(requestId));
        mResponses.erase(requestId);
        return result;
    }

    Response transact(const std::string& endpoint, const Command& command, std::chrono::milliseconds timeout)
    {
        std::unique_ptr<LocalConnection> connection = connectLocally(endpoint, timeout);
        connection->send(encodeCommand(command));
        Response response = decodeResponse(connection->receive());
        if (response.mRequestId != command.mRequestId)
            throw ProtocolError(ErrorCode::InvalidFrame, "diagnostic response request identity does not match");
        return response;
    }
}
