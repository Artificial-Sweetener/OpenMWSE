#include "optimizationsettings.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>

namespace Diagnostics
{
    OptimizationSettings deserializeOptimizationSettings(std::string_view text)
    {
        if (text.size() > 4096)
            throw std::invalid_argument("optimization settings payload exceeds 4096 bytes");
        const YAML::Node node = YAML::Load(std::string(text));
        if (!node.IsMap() || node["settings-version"].as<int>() != 1 || node.size() != 14)
            throw std::invalid_argument("optimization settings require version 1 and every selection");
        return {
            node["enabled"].as<bool>(),
            node["gpu-driven-paging"].as<bool>(),
            node["gpu-skinning"].as<bool>(),
            node["gpu-occlusion-culling"].as<bool>(),
            node["gpu-actors"].as<bool>(),
            node["gpu-texture-arrays"].as<bool>(),
            node["indexed-light-lists"].as<bool>(),
            node["gpu-scene-lighting"].as<bool>(),
            node["gpu-storage-budget"].as<bool>(),
            node["low-end-indirect-batching"].as<bool>(),
            node["gpu-light-occlusion"].as<bool>(),
            node["gpu-distant-paging"].as<bool>(),
            node["skip-dormant-ripples"].as<bool>(),
        };
    }

    std::string serializeOptimizationSettings(const OptimizationSettings& settings)
    {
        YAML::Node node;
        node["settings-version"] = 1;
        node["enabled"] = settings.mEnabled;
        node["gpu-driven-paging"] = settings.mGpuDrivenPaging;
        node["gpu-skinning"] = settings.mGpuSkinning;
        node["gpu-occlusion-culling"] = settings.mGpuOcclusionCulling;
        node["gpu-actors"] = settings.mGpuActors;
        node["gpu-texture-arrays"] = settings.mGpuTextureArrays;
        node["indexed-light-lists"] = settings.mIndexedLightLists;
        node["gpu-scene-lighting"] = settings.mGpuSceneLighting;
        node["gpu-storage-budget"] = settings.mGpuStorageBudget;
        node["low-end-indirect-batching"] = settings.mLowEndIndirectBatching;
        node["gpu-light-occlusion"] = settings.mGpuLightOcclusion;
        node["gpu-distant-paging"] = settings.mGpuDistantPaging;
        node["skip-dormant-ripples"] = settings.mSkipDormantRipples;
        return YAML::Dump(node);
    }
}
