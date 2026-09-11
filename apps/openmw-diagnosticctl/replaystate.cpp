#include "replaystate.hpp"

#include <stdexcept>
#include <string>
#include <yaml-cpp/yaml.h>

namespace DiagnosticCtl
{
    void validateAtmosphereRuntimeParity(std::string_view expected, std::string_view actual)
    {
        const YAML::Node recorded = YAML::Load(std::string(expected));
        YAML::Node restored = YAML::Load(std::string(actual));
        const auto recordedPalette = recorded["palette"];
        auto restoredPalette = restored["palette"];
        if (recordedPalette && restoredPalette && recordedPalette["runtime-version"].as<unsigned>() == 1
            && restoredPalette["runtime-version"].as<unsigned>() == 2)
        {
            if (!restoredPalette["regional-colors"].IsMap())
                throw std::runtime_error("replay regional color history is missing");
            restoredPalette.remove("regional-colors");
            restoredPalette["runtime-version"] = 1;
        }
        if (YAML::Dump(recorded) != YAML::Dump(restored))
            throw std::runtime_error("replay regional atmosphere history does not match its capsule");
    }
}
