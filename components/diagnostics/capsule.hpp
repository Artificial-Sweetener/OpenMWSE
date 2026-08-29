#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_CAPSULE_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_CAPSULE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Diagnostics
{
    inline constexpr std::uint16_t capsuleVersion = 1;
    inline constexpr std::size_t maximumCapsuleEntries = 256;
    inline constexpr std::size_t maximumCapsuleEntrySize = 128 * 1024 * 1024;
    inline constexpr std::size_t maximumCapsuleSize = 512 * 1024 * 1024;

    /// Describes the semantic role of one validated capsule attachment.
    enum class CapsuleEntryType : std::uint16_t
    {
        Manifest = 1,
        WorldSnapshot = 2,
        Configuration = 3,
        Graphics = 4,
        GameplayTrace = 5,
        PerformanceTrace = 6,
        Log = 7,
    };

    /// Stores one named diagnostic artifact with a bounded, integrity-checked payload.
    struct CapsuleEntry
    {
        std::string mName;
        CapsuleEntryType mType = CapsuleEntryType::Manifest;
        std::vector<std::byte> mData;

        /// Compare the portable name, semantic type, and stored bytes.
        bool operator==(const CapsuleEntry&) const = default;
    };

    /// Owns a validated collection of portable diagnostic artifacts.
    class Capsule
    {
    public:
        /// Add an entry after validating its name, uniqueness, and size limits.
        void add(CapsuleEntry entry);

        /// Return all entries in their deterministic stored order.
        const std::vector<CapsuleEntry>& entries() const noexcept;

        /// Find an entry by its canonical name or return null when it is absent.
        const CapsuleEntry* find(std::string_view name) const noexcept;

    private:
        std::vector<CapsuleEntry> mEntries;
        std::size_t mPayloadSize = 0;
    };

    /// Write a complete capsule atomically without replacing an existing artifact.
    void writeCapsuleAtomically(const std::filesystem::path& path, const Capsule& capsule);

    /// Read and validate a capsule before exposing any contained entry.
    Capsule readCapsule(const std::filesystem::path& path);
}

#endif
