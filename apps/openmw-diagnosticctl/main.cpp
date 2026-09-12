#include <components/debug/debugging.hpp>
#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/gameplayroute.hpp>
#include <components/diagnostics/localtransport.hpp>
#include <components/diagnostics/session.hpp>
#include <components/files/hash.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "gameplayrouteexecution.hpp"
#include "controllerprotocol.hpp"
#include "isolatedsettings.hpp"
#include "isolatedrun.hpp"
#include "isolatedworkspace.hpp"
#include "replaystate.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace DiagnosticCtl;

    /// Execute a single command against an already enabled live session.
    int runAttachCommand(std::string_view operation, std::span<char*> arguments)
    {
        const std::string endpoint = findOption(arguments, "--endpoint").value_or(Diagnostics::defaultLocalEndpoint());
        Diagnostics::Command command;
        command.mRequestId = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        if (operation == "status")
            command.mType = Diagnostics::CommandType::InspectSession;
        else if (operation == "graphics")
            command.mType = Diagnostics::CommandType::CaptureGraphics;
        else if (operation == "gameplay")
            command.mType = Diagnostics::CommandType::TraceGameplay;
        else if (operation == "performance")
        {
            command.mType = Diagnostics::CommandType::TracePerformance;
            const std::uint32_t sampleCount
                = static_cast<std::uint32_t>(std::stoul(findOption(arguments, "--performance-samples").value_or("1")));
            if (sampleCount == 0 || sampleCount > 600)
                throw std::invalid_argument("performance samples must be within [1, 600]");
            for (std::size_t byte = 0; byte < sizeof(sampleCount); ++byte)
                command.mPayload.push_back(static_cast<std::byte>((sampleCount >> (byte * 8)) & 0xffu));
        }
        else if (operation == "reload-shaders")
            command.mType = Diagnostics::CommandType::ReloadShaders;
        else if (operation == "shutdown")
            command.mType = Diagnostics::CommandType::ShutdownChild;
        else if (operation == "capsule")
        {
            command.mType = Diagnostics::CommandType::CaptureCapsule;
            command.mPayload = textPayload(findOption(arguments, "--output").value_or("capture.omwdiag"));
        }
        else if (operation == "step")
        {
            command.mType = Diagnostics::CommandType::StepFrames;
            const std::uint32_t count
                = static_cast<std::uint32_t>(std::stoul(findOption(arguments, "--frames").value_or("1")));
            for (std::size_t byte = 0; byte < sizeof(count); ++byte)
                command.mPayload.push_back(static_cast<std::byte>((count >> (byte * 8)) & 0xffu));
        }
        else
            throw std::invalid_argument("unknown diagnostic operation");
        const std::chrono::milliseconds timeout
            = operation == "performance" ? std::chrono::seconds(60) : std::chrono::seconds(10);
        return printResponse(Diagnostics::transact(endpoint, command, timeout));
    }

    /// Inspect a capsule without extracting or trusting its attachment names as paths.
    int inspectCapsule(std::span<char*> arguments)
    {
        const std::filesystem::path path = findOption(arguments, "--capsule").value_or(std::string());
        if (path.empty())
            throw std::invalid_argument("inspect-capsule requires --capsule");
        const Diagnostics::Capsule capsule = Diagnostics::readCapsule(path);
        if (const std::optional<std::string> requestedEntry = findOption(arguments, "--entry"))
        {
            const Diagnostics::CapsuleEntry* entry = capsule.find(*requestedEntry);
            if (entry == nullptr)
                throw std::invalid_argument("requested capsule entry does not exist");
            if (entry->mType == Diagnostics::CapsuleEntryType::WorldSnapshot)
                throw std::invalid_argument("binary world snapshots cannot be printed");
            std::cout.write(
                reinterpret_cast<const char*>(entry->mData.data()), static_cast<std::streamsize>(entry->mData.size()));
            if (!std::cout)
                throw std::runtime_error("failed writing capsule entry");
            return 0;
        }
        std::cout << "capsule-version: " << Diagnostics::capsuleVersion << "\nentries:\n";
        for (const Diagnostics::CapsuleEntry& entry : capsule.entries())
            std::cout << "  - " << entry.mName << " (" << entry.mData.size() << " bytes)\n";
        return 0;
    }

    /// Attach a bounded, validated route to a new capsule without launching an engine or changing the source.
    int addRoute(std::span<char*> arguments)
    {
        const std::filesystem::path source = findOption(arguments, "--capsule").value_or(std::string());
        const std::filesystem::path route = findOption(arguments, "--route").value_or(std::string());
        const std::filesystem::path output = findOption(arguments, "--output").value_or(std::string());
        if (source.empty() || route.empty() || output.empty())
            throw std::invalid_argument("add-route requires --capsule, --route, and --output");
        std::ifstream input(route, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error("failed opening gameplay route");
        const std::streamoff size = input.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) > Diagnostics::maximumCapsuleEntrySize)
            throw std::length_error("gameplay route file size is outside capsule entry bounds");
        std::vector<std::byte> payload(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input)
            throw std::runtime_error("failed reading gameplay route");
        Diagnostics::writeCapsuleAtomically(
            output, Diagnostics::withGameplayRoute(Diagnostics::readCapsule(source), payload));
        std::cout << "route-capsule: " << output << '\n';
        return 0;
    }

    /// Parse and execute the bounded diagnostic controller command surface.
    int runController(int argc, char** argv)
    {
        if (argc < 2)
        {
            printUsage(std::cerr);
            return 2;
        }
        const std::string_view operation = argv[1];
        if (operation == "help" || operation == "--help" || operation == "-h")
        {
            printUsage(std::cout);
            return 0;
        }
        const std::span arguments(argv + 2, static_cast<std::size_t>(argc - 2));
        if (operation == "run" || operation == "replay")
            return runIsolated(arguments);
        if (operation == "inspect-capsule")
            return inspectCapsule(arguments);
        if (operation == "add-route")
            return addRoute(arguments);
        return runAttachCommand(operation, arguments);
    }
}

/// Run the diagnostic controller with standard OpenMW exception reporting.
int main(int argc, char** argv)
{
    try
    {
        return runController(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::cerr << "openmw-diagnosticctl: " << error.what() << '\n';
        return 1;
    }
}
