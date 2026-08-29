#include "capsule.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace Diagnostics
{
    namespace
    {
        constexpr std::array<char, 8> sMagic{ 'O', 'M', 'W', 'D', 'I', 'A', 'G', '\0' };
        constexpr std::size_t sMaximumNameSize = 240;

        /// Return whether a numeric attachment type belongs to the current capsule format.
        bool isSupportedEntryType(CapsuleEntryType type) noexcept
        {
            return type >= CapsuleEntryType::Manifest && type <= CapsuleEntryType::Log;
        }

        /// Validate a portable attachment name that cannot escape a capsule namespace.
        void validateEntryName(std::string_view name)
        {
            if (name.empty() || name.size() > sMaximumNameSize)
                throw std::invalid_argument("diagnostic capsule entry name has an invalid length");
            if (name == "." || name == ".." || name.front() == '/' || name.front() == '\\'
                || name.find("..") != std::string_view::npos || name.find('\\') != std::string_view::npos
                || name.find(':') != std::string_view::npos || name.find('\0') != std::string_view::npos)
                throw std::invalid_argument("diagnostic capsule entry name is not portable");
        }

        /// Write an unsigned integer using the capsule's fixed little-endian representation.
        template <class T>
        void writeInteger(std::ostream& stream, T value)
        {
            static_assert(std::is_unsigned_v<T>);
            for (std::size_t byte = 0; byte < sizeof(T); ++byte)
                stream.put(static_cast<char>((value >> (byte * 8)) & 0xffu));
            if (!stream)
                throw std::runtime_error("failed writing diagnostic capsule integer");
        }

        /// Read an unsigned integer using the capsule's fixed little-endian representation.
        template <class T>
        T readInteger(std::istream& stream)
        {
            static_assert(std::is_unsigned_v<T>);
            T value = 0;
            for (std::size_t byte = 0; byte < sizeof(T); ++byte)
            {
                const int next = stream.get();
                if (next == std::char_traits<char>::eof())
                    throw std::runtime_error("diagnostic capsule ended inside an integer");
                value |= static_cast<T>(static_cast<unsigned char>(next)) << (byte * 8);
            }
            return value;
        }

        /// Calculate the stored integrity checksum for one capsule payload.
        std::uint32_t checksum(std::span<const std::byte> data)
        {
            uLong value = crc32(0L, Z_NULL, 0);
            std::size_t offset = 0;
            while (offset < data.size())
            {
                const std::size_t remaining = data.size() - offset;
                const auto chunk
                    = static_cast<uInt>(std::min<std::size_t>(remaining, std::numeric_limits<uInt>::max()));
                value = crc32(value, reinterpret_cast<const Bytef*>(data.data() + offset), chunk);
                offset += chunk;
            }
            return static_cast<std::uint32_t>(value);
        }

        /// Create a collision-resistant temporary sibling used for one atomic write attempt.
        std::filesystem::path makeTemporaryPath(const std::filesystem::path& destination)
        {
            std::random_device source;
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            return destination.parent_path()
                / (destination.filename().string() + ".tmp-" + std::to_string(now) + "-" + std::to_string(source()));
        }

        /// Remove an incomplete temporary artifact without masking the original failure.
        void removeTemporaryFile(const std::filesystem::path& path) noexcept
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }

    void Capsule::add(CapsuleEntry entry)
    {
        if (!isSupportedEntryType(entry.mType))
            throw std::invalid_argument("diagnostic capsule entry type is not supported");
        validateEntryName(entry.mName);
        if (mEntries.size() >= maximumCapsuleEntries)
            throw std::length_error("diagnostic capsule contains too many entries");
        if (entry.mData.size() > maximumCapsuleEntrySize)
            throw std::length_error("diagnostic capsule entry exceeds its size limit");
        if (mPayloadSize > maximumCapsuleSize - entry.mData.size())
            throw std::length_error("diagnostic capsule exceeds its total size limit");
        if (find(entry.mName) != nullptr)
            throw std::invalid_argument("diagnostic capsule entry name is duplicated");

        mPayloadSize += entry.mData.size();
        mEntries.emplace_back(std::move(entry));
    }

    const std::vector<CapsuleEntry>& Capsule::entries() const noexcept
    {
        return mEntries;
    }

    const CapsuleEntry* Capsule::find(std::string_view name) const noexcept
    {
        const auto found = std::find_if(
            mEntries.begin(), mEntries.end(), [name](const CapsuleEntry& entry) { return entry.mName == name; });
        return found == mEntries.end() ? nullptr : &*found;
    }

    void writeCapsuleAtomically(const std::filesystem::path& path, const Capsule& capsule)
    {
        if (path.empty())
            throw std::invalid_argument("diagnostic capsule destination is empty");
        if (std::filesystem::exists(path))
            throw std::runtime_error("diagnostic capsule destination already exists");
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());

        const std::filesystem::path temporaryPath = makeTemporaryPath(path);
        try
        {
            std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!stream)
                throw std::runtime_error("failed opening temporary diagnostic capsule");

            stream.write(sMagic.data(), static_cast<std::streamsize>(sMagic.size()));
            writeInteger(stream, capsuleVersion);
            writeInteger(stream, static_cast<std::uint16_t>(0));
            writeInteger(stream, static_cast<std::uint32_t>(capsule.entries().size()));
            for (const CapsuleEntry& entry : capsule.entries())
            {
                writeInteger(stream, static_cast<std::uint16_t>(entry.mType));
                writeInteger(stream, static_cast<std::uint16_t>(entry.mName.size()));
                writeInteger(stream, static_cast<std::uint64_t>(entry.mData.size()));
                writeInteger(stream, checksum(entry.mData));
                stream.write(entry.mName.data(), static_cast<std::streamsize>(entry.mName.size()));
                stream.write(reinterpret_cast<const char*>(entry.mData.data()),
                    static_cast<std::streamsize>(entry.mData.size()));
                if (!stream)
                    throw std::runtime_error("failed writing diagnostic capsule entry");
            }
            stream.flush();
            if (!stream)
                throw std::runtime_error("failed flushing diagnostic capsule");
            stream.close();
            std::filesystem::rename(temporaryPath, path);
        }
        catch (...)
        {
            removeTemporaryFile(temporaryPath);
            throw;
        }
    }

    Capsule readCapsule(const std::filesystem::path& path)
    {
        const std::uintmax_t fileSize = std::filesystem::file_size(path);
        if (fileSize > maximumCapsuleSize + maximumCapsuleEntries * (sMaximumNameSize + 32))
            throw std::length_error("diagnostic capsule file exceeds its size limit");

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("failed opening diagnostic capsule");

        std::array<char, sMagic.size()> magic{};
        stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (magic != sMagic)
            throw std::runtime_error("diagnostic capsule has an invalid signature");
        if (readInteger<std::uint16_t>(stream) != capsuleVersion)
            throw std::runtime_error("diagnostic capsule version is not supported");
        if (readInteger<std::uint16_t>(stream) != 0)
            throw std::runtime_error("diagnostic capsule uses unsupported flags");

        const std::uint32_t entryCount = readInteger<std::uint32_t>(stream);
        if (entryCount > maximumCapsuleEntries)
            throw std::length_error("diagnostic capsule contains too many entries");

        Capsule result;
        for (std::uint32_t index = 0; index < entryCount; ++index)
        {
            CapsuleEntry entry;
            entry.mType = static_cast<CapsuleEntryType>(readInteger<std::uint16_t>(stream));
            const std::uint16_t nameSize = readInteger<std::uint16_t>(stream);
            const std::uint64_t dataSize = readInteger<std::uint64_t>(stream);
            const std::uint32_t expectedChecksum = readInteger<std::uint32_t>(stream);
            if (nameSize == 0 || nameSize > sMaximumNameSize || dataSize > maximumCapsuleEntrySize)
                throw std::length_error("diagnostic capsule entry exceeds its bounds");

            entry.mName.resize(nameSize);
            entry.mData.resize(static_cast<std::size_t>(dataSize));
            stream.read(entry.mName.data(), nameSize);
            stream.read(reinterpret_cast<char*>(entry.mData.data()), static_cast<std::streamsize>(entry.mData.size()));
            if (!stream)
                throw std::runtime_error("diagnostic capsule ended inside an entry");
            if (checksum(entry.mData) != expectedChecksum)
                throw std::runtime_error("diagnostic capsule entry failed its integrity check");
            result.add(std::move(entry));
        }

        if (stream.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("diagnostic capsule contains trailing data");
        return result;
    }
}
