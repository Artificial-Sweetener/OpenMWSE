#include <components/diagnostics/capsule.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
    /// Own a unique temporary directory for one capsule test.
    class CapsuleTest : public ::testing::Test
    {
    protected:
        /// Create the isolated test directory before each test.
        void SetUp() override
        {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            mDirectory = std::filesystem::temp_directory_path() / ("openmw-diagnostics-" + std::to_string(suffix));
            std::filesystem::create_directories(mDirectory);
        }

        /// Remove every isolated test artifact after each test.
        void TearDown() override
        {
            std::error_code error;
            std::filesystem::remove_all(mDirectory, error);
        }

        std::filesystem::path mDirectory;
    };

    TEST_F(CapsuleTest, RoundTripsEntriesInStoredOrder)
    {
        Diagnostics::Capsule capsule;
        capsule.add(
            { "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest, { std::byte{ 'a' }, std::byte{ 'b' } } });
        capsule.add(
            { "graphics/final.rgba", Diagnostics::CapsuleEntryType::Graphics, { std::byte{ 1 }, std::byte{ 2 } } });

        const std::filesystem::path path = mDirectory / "capture.omwdiag";
        Diagnostics::writeCapsuleAtomically(path, capsule);
        EXPECT_EQ(Diagnostics::readCapsule(path).entries(), capsule.entries());
    }

    TEST_F(CapsuleTest, RejectsTraversalAndDuplicateNames)
    {
        Diagnostics::Capsule capsule;
        EXPECT_THROW(capsule.add({ "../save.omwsave", Diagnostics::CapsuleEntryType::WorldSnapshot, {} }),
            std::invalid_argument);
        capsule.add({ "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest, {} });
        EXPECT_THROW(
            capsule.add({ "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest, {} }), std::invalid_argument);
        EXPECT_THROW(
            capsule.add({ "invalid.bin", static_cast<Diagnostics::CapsuleEntryType>(999), {} }), std::invalid_argument);
    }

    TEST_F(CapsuleTest, DetectsPayloadCorruptionAndPreservesExistingDestination)
    {
        Diagnostics::Capsule capsule;
        capsule.add({ "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest,
            { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } } });
        const std::filesystem::path path = mDirectory / "capture.omwdiag";
        Diagnostics::writeCapsuleAtomically(path, capsule);
        EXPECT_THROW(Diagnostics::writeCapsuleAtomically(path, capsule), std::runtime_error);

        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(-1, std::ios::end);
        stream.put(static_cast<char>(0xff));
        stream.close();
        EXPECT_THROW(static_cast<void>(Diagnostics::readCapsule(path)), std::runtime_error);
    }
}
