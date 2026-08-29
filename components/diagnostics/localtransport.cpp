#include "localtransport.hpp"

#include "protocol.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <components/misc/windows.hpp>

#include <sddl.h>
#else
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Diagnostics
{
    namespace
    {
        constexpr std::size_t sFrameHeaderSize = 20;

        /// Decode the payload size from a complete protocol header.
        std::uint32_t readPayloadSize(std::span<const std::byte> header)
        {
            std::uint32_t value = 0;
            for (std::size_t byte = 0; byte < sizeof(value); ++byte)
                value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(header[16 + byte])) << (byte * 8);
            if (value > maximumPayloadSize)
                throw ProtocolError(ErrorCode::PayloadTooLarge, "local diagnostic frame exceeds the protocol limit");
            return value;
        }

#ifdef _WIN32
        /// Format the most recent Win32 failure with its owning operation.
        std::runtime_error windowsFailure(std::string_view operation)
        {
            return std::runtime_error(
                std::string(operation) + " failed with Win32 error " + std::to_string(GetLastError()));
        }

        /// Return the current process user's SID for endpoint naming and access control.
        std::wstring currentUserSid()
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                throw windowsFailure("OpenProcessToken");

            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<std::byte> buffer(size);
            if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
            {
                CloseHandle(token);
                throw windowsFailure("GetTokenInformation");
            }
            CloseHandle(token);

            LPWSTR text = nullptr;
            const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            if (!ConvertSidToStringSidW(user->User.Sid, &text))
                throw windowsFailure("ConvertSidToStringSidW");
            std::wstring result(text);
            LocalFree(text);
            return result;
        }

        /// Hash a SID into an endpoint-safe stable suffix without disclosing account names.
        std::uint64_t hashSid(std::wstring_view sid)
        {
            std::uint64_t result = 1469598103934665603ull;
            for (wchar_t character : sid)
            {
                result ^= static_cast<std::uint16_t>(character);
                result *= 1099511628211ull;
            }
            return result;
        }

        /// Own a Win32 handle and transfer it only to a connection object.
        class WindowsConnection final : public LocalConnection
        {
        public:
            /// Adopt one connected named-pipe handle.
            explicit WindowsConnection(HANDLE handle)
                : mHandle(handle)
            {
            }

            /// Close the adopted named-pipe handle.
            ~WindowsConnection() override
            {
                if (mHandle != INVALID_HANDLE_VALUE)
                    CloseHandle(mHandle);
            }

            /// Receive exactly one bounded frame from the named pipe.
            std::vector<std::byte> receive() override
            {
                std::vector<std::byte> frame(sFrameHeaderSize);
                readExactly(frame);
                const std::uint32_t payloadSize = readPayloadSize(frame);
                const std::size_t headerSize = frame.size();
                frame.resize(headerSize + payloadSize);
                readExactly(std::span(frame).subspan(headerSize));
                return frame;
            }

            /// Send exactly one frame to the named pipe.
            void send(std::span<const std::byte> frame) override
            {
                std::size_t offset = 0;
                while (offset < frame.size())
                {
                    const DWORD chunk = static_cast<DWORD>(
                        std::min<std::size_t>(frame.size() - offset, std::numeric_limits<DWORD>::max()));
                    DWORD written = 0;
                    if (!WriteFile(mHandle, frame.data() + offset, chunk, &written, nullptr) || written == 0)
                        throw windowsFailure("WriteFile");
                    offset += written;
                }
                FlushFileBuffers(mHandle);
            }

        private:
            /// Fill a destination span or report a disconnected or truncated stream.
            void readExactly(std::span<std::byte> destination)
            {
                std::size_t offset = 0;
                while (offset < destination.size())
                {
                    const DWORD chunk = static_cast<DWORD>(
                        std::min<std::size_t>(destination.size() - offset, std::numeric_limits<DWORD>::max()));
                    DWORD read = 0;
                    if (!ReadFile(mHandle, destination.data() + offset, chunk, &read, nullptr) || read == 0)
                        throw windowsFailure("ReadFile");
                    offset += read;
                }
            }

            HANDLE mHandle;
        };

        /// Own a named-pipe endpoint protected by an explicit current-user DACL.
        class WindowsListener final : public LocalListener
        {
        public:
            /// Prepare the protected pipe name and security descriptor.
            explicit WindowsListener(std::string endpoint)
                : mEndpoint(std::move(endpoint))
            {
                const std::wstring descriptor = L"D:P(A;;GA;;;" + currentUserSid() + L")";
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                        descriptor.c_str(), SDDL_REVISION_1, &mSecurityDescriptor, nullptr))
                    throw windowsFailure("ConvertStringSecurityDescriptorToSecurityDescriptorW");
            }

            /// Close the listener and release its security descriptor.
            ~WindowsListener() override
            {
                close();
                LocalFree(mSecurityDescriptor);
            }

            /// Create and accept one protected named-pipe instance.
            std::unique_ptr<LocalConnection> accept() override
            {
                if (mClosed.load())
                    return {};
                SECURITY_ATTRIBUTES attributes{ sizeof(attributes), mSecurityDescriptor, FALSE };
                const std::wstring endpoint(mEndpoint.begin(), mEndpoint.end());
                HANDLE pipe = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 8,
                    static_cast<DWORD>(maximumPayloadSize + sFrameHeaderSize),
                    static_cast<DWORD>(maximumPayloadSize + sFrameHeaderSize), 0, &attributes);
                if (pipe == INVALID_HANDLE_VALUE)
                    throw windowsFailure("CreateNamedPipeW");
                mPending.store(pipe);
                if (mClosed.load())
                {
                    mPending.store(INVALID_HANDLE_VALUE);
                    CloseHandle(pipe);
                    return {};
                }

                OVERLAPPED operation{};
                operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (operation.hEvent == nullptr)
                {
                    mPending.store(INVALID_HANDLE_VALUE);
                    CloseHandle(pipe);
                    throw windowsFailure("CreateEventW");
                }

                bool connected = ConnectNamedPipe(pipe, &operation) != FALSE;
                DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
                if (!connected && connectError == ERROR_PIPE_CONNECTED)
                    connected = true;
                else if (!connected && connectError == ERROR_IO_PENDING)
                {
                    while (!mClosed.load())
                    {
                        if (WaitForSingleObject(operation.hEvent, 10) == WAIT_OBJECT_0)
                        {
                            DWORD transferred = 0;
                            connected = GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
                            break;
                        }
                    }
                    if (mClosed.load())
                    {
                        CancelIoEx(pipe, &operation);
                        WaitForSingleObject(operation.hEvent, INFINITE);
                    }
                }
                CloseHandle(operation.hEvent);
                mPending.store(INVALID_HANDLE_VALUE);
                if (!connected || mClosed.load())
                {
                    CloseHandle(pipe);
                    if (mClosed.load())
                        return {};
                    throw windowsFailure("ConnectNamedPipe");
                }
                return std::make_unique<WindowsConnection>(pipe);
            }

            /// Interrupt the currently pending pipe connection.
            void close() noexcept override
            {
                if (mClosed.exchange(true))
                    return;
                const HANDLE pending = mPending.exchange(INVALID_HANDLE_VALUE);
                if (pending != INVALID_HANDLE_VALUE)
                    CancelIoEx(pending, nullptr);
            }

        private:
            std::string mEndpoint;
            PSECURITY_DESCRIPTOR mSecurityDescriptor = nullptr;
            std::atomic<HANDLE> mPending{ INVALID_HANDLE_VALUE };
            std::atomic_bool mClosed = false;
        };
#else
        /// Own one Unix-domain stream socket.
        class UnixConnection final : public LocalConnection
        {
        public:
            /// Adopt one connected Unix-domain socket descriptor.
            explicit UnixConnection(int descriptor)
                : mDescriptor(descriptor)
            {
            }

            /// Close the adopted socket descriptor.
            ~UnixConnection() override { ::close(mDescriptor); }

            /// Receive exactly one bounded frame from the socket.
            std::vector<std::byte> receive() override
            {
                std::vector<std::byte> frame(sFrameHeaderSize);
                readExactly(frame);
                const std::uint32_t payloadSize = readPayloadSize(frame);
                const std::size_t headerSize = frame.size();
                frame.resize(headerSize + payloadSize);
                readExactly(std::span(frame).subspan(headerSize));
                return frame;
            }

            /// Send exactly one frame to the socket.
            void send(std::span<const std::byte> frame) override
            {
                std::size_t offset = 0;
                while (offset < frame.size())
                {
                    const ssize_t written = ::send(mDescriptor, frame.data() + offset, frame.size() - offset, 0);
                    if (written <= 0)
                        throw std::runtime_error("failed writing local diagnostic socket");
                    offset += static_cast<std::size_t>(written);
                }
            }

        private:
            /// Fill a destination span or report a disconnected or truncated stream.
            void readExactly(std::span<std::byte> destination)
            {
                std::size_t offset = 0;
                while (offset < destination.size())
                {
                    const ssize_t read
                        = ::recv(mDescriptor, destination.data() + offset, destination.size() - offset, 0);
                    if (read <= 0)
                        throw std::runtime_error("failed reading local diagnostic socket");
                    offset += static_cast<std::size_t>(read);
                }
            }

            int mDescriptor;
        };

        /// Own a permission-restricted Unix-domain listener socket.
        class UnixListener final : public LocalListener
        {
        public:
            /// Bind a new user-only listener at the supplied filesystem path.
            explicit UnixListener(std::string endpoint)
                : mEndpoint(std::move(endpoint))
            {
                mDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (mDescriptor < 0)
                    throw std::runtime_error("failed creating local diagnostic socket");
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                if (mEndpoint.size() >= sizeof(address.sun_path))
                    throw std::invalid_argument("local diagnostic endpoint path is too long");
                std::memcpy(address.sun_path, mEndpoint.c_str(), mEndpoint.size() + 1);
                ::unlink(address.sun_path);
                if (::bind(mDescriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
                    || ::chmod(address.sun_path, S_IRUSR | S_IWUSR) != 0 || ::listen(mDescriptor, 8) != 0)
                    throw std::runtime_error("failed binding local diagnostic socket");
            }

            /// Close and unlink the owned listener.
            ~UnixListener() override
            {
                close();
                ::unlink(mEndpoint.c_str());
            }

            /// Accept one authenticated local socket connection.
            std::unique_ptr<LocalConnection> accept() override
            {
                const int connection = ::accept(mDescriptor, nullptr, nullptr);
                if (connection < 0)
                {
                    if (mClosed.load())
                        return {};
                    throw std::runtime_error("failed accepting local diagnostic socket");
                }
                return std::make_unique<UnixConnection>(connection);
            }

            /// Interrupt pending accepts and prevent new connections.
            void close() noexcept override
            {
                if (!mClosed.exchange(true) && mDescriptor >= 0)
                {
                    ::shutdown(mDescriptor, SHUT_RDWR);
                    ::close(mDescriptor);
                    mDescriptor = -1;
                }
            }

        private:
            std::string mEndpoint;
            int mDescriptor = -1;
            std::atomic_bool mClosed = false;
        };
#endif
    }

    std::string defaultLocalEndpoint()
    {
#ifdef _WIN32
        return R"(\\.\pipe\OpenMWSE-Diagnostics-)" + std::to_string(hashSid(currentUserSid()));
#else
        const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
        const std::filesystem::path root
            = runtimeDirectory != nullptr ? runtimeDirectory : std::filesystem::temp_directory_path();
        return (root / ("openmwse-diagnostics-" + std::to_string(::getuid()) + ".sock")).string();
#endif
    }

    std::unique_ptr<LocalListener> listenLocally(const std::string& endpoint)
    {
        if (endpoint.empty())
            throw std::invalid_argument("local diagnostic endpoint is empty");
#ifdef _WIN32
        if (!endpoint.starts_with(R"(\\.\pipe\OpenMWSE-Diagnostics-)"))
            throw std::invalid_argument("local diagnostic pipe is outside the OpenMWSE namespace");
        return std::make_unique<WindowsListener>(endpoint);
#else
        return std::make_unique<UnixListener>(endpoint);
#endif
    }

    std::unique_ptr<LocalConnection> connectLocally(const std::string& endpoint, std::chrono::milliseconds timeout)
    {
        if (endpoint.empty() || timeout <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("local diagnostic connection parameters are invalid");
#ifdef _WIN32
        if (!endpoint.starts_with(R"(\\.\pipe\OpenMWSE-Diagnostics-)"))
            throw std::invalid_argument("local diagnostic pipe is outside the OpenMWSE namespace");
        const std::wstring pipeName(endpoint.begin(), endpoint.end());
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining
                = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const auto timeoutCount = std::min<std::int64_t>(std::max<std::int64_t>(remaining.count(), 1), 100);
            if (WaitNamedPipeW(pipeName.c_str(), static_cast<DWORD>(timeoutCount)))
            {
                HANDLE pipe = CreateFileW(
                    pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE)
                    return std::make_unique<WindowsConnection>(pipe);
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY && error != ERROR_SEM_TIMEOUT)
                throw windowsFailure("WaitNamedPipeW");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("timed out connecting to local diagnostic pipe");
#else
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (descriptor < 0)
                throw std::runtime_error("failed creating local diagnostic socket");
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if (endpoint.size() >= sizeof(address.sun_path))
            {
                ::close(descriptor);
                throw std::invalid_argument("local diagnostic endpoint path is too long");
            }
            std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
            if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                return std::make_unique<UnixConnection>(descriptor);
            ::close(descriptor);
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("timed out connecting to local diagnostic socket");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#endif
    }
}
