#include "settingssequences.hpp"

#include "controllerprotocol.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <string>

namespace DiagnosticCtl
{
    namespace
    {
        /// Load one optional sequence and require exact route-segment coverage.
        template <class Value, class Decode>
        std::vector<Value> loadSequence(std::span<char*> arguments, const SettingsSequenceContext& context,
            std::string_view option, std::string_view evidenceName, std::string_view description, Decode&& decode)
        {
            const auto sequencePath = findOption(arguments, option);
            if (!sequencePath)
                return {};
            const YAML::Node sequence = YAML::LoadFile(*sequencePath);
            const std::size_t segmentCount
                = (context.mFrameCount + context.mSegmentFrameCount - 1) / context.mSegmentFrameCount;
            if (!sequence.IsSequence() || sequence.size() != segmentCount || sequence.size() > 64)
                throw std::invalid_argument(
                    std::string(description) + " requires one entry per route segment, at most 64");
            std::vector<Value> result;
            result.reserve(sequence.size());
            for (const auto& entry : sequence)
                result.push_back(decode(YAML::Dump(entry)));
            std::filesystem::copy_file(*sequencePath, context.mArtifacts / evidenceName);
            return result;
        }
    }

    std::vector<Diagnostics::AtmosphereSettings> loadAtmosphereSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context)
    {
        return loadSequence<Diagnostics::AtmosphereSettings>(arguments, context, "--atmosphere-settings-sequence",
            "atmosphere-settings-sequence.yaml", "atmosphere settings sequence",
            Diagnostics::deserializeAtmosphereSettings);
    }

    std::vector<Diagnostics::DistanceSettings> loadDistanceSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context)
    {
        return loadSequence<Diagnostics::DistanceSettings>(arguments, context, "--distance-settings-sequence",
            "distance-settings-sequence.yaml", "distance settings sequence", Diagnostics::deserializeDistanceSettings);
    }

    std::vector<Diagnostics::OptimizationSettings> loadOptimizationSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context)
    {
        return loadSequence<Diagnostics::OptimizationSettings>(arguments, context, "--optimization-settings-sequence",
            "optimization-settings-sequence.yaml", "optimization settings sequence",
            Diagnostics::deserializeOptimizationSettings);
    }

    std::vector<Diagnostics::WaterSettings> loadWaterSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context)
    {
        return loadSequence<Diagnostics::WaterSettings>(arguments, context, "--water-settings-sequence",
            "water-settings-sequence.yaml", "water settings sequence", Diagnostics::deserializeWaterSettings);
    }
}
