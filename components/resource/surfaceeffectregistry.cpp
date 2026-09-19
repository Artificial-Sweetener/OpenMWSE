#include "surfaceeffectregistry.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

#include <components/debug/debuglog.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

namespace Resource
{
    namespace
    {
        constexpr VFS::Path::NormalizedView sRuleDirectory("material-effects");

        /// Compare one normalized value against a case-insensitive glob containing only '*' and '?'.
        bool matchesGlob(std::string_view value, std::string_view pattern)
        {
            std::size_t valueIndex = 0;
            std::size_t patternIndex = 0;
            std::size_t starIndex = std::string_view::npos;
            std::size_t starValueIndex = 0;
            const auto equal = [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left))
                    == std::tolower(static_cast<unsigned char>(right));
            };

            while (valueIndex < value.size())
            {
                if (patternIndex < pattern.size()
                    && (pattern[patternIndex] == '?' || equal(value[valueIndex], pattern[patternIndex])))
                {
                    ++valueIndex;
                    ++patternIndex;
                }
                else if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
                {
                    starIndex = patternIndex++;
                    starValueIndex = valueIndex;
                }
                else if (starIndex != std::string_view::npos)
                {
                    patternIndex = starIndex + 1;
                    valueIndex = ++starValueIndex;
                }
                else
                    return false;
            }
            while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
                ++patternIndex;
            return patternIndex == pattern.size();
        }

        /// Parse the only production effect type currently backed by a renderer implementation.
        SceneUtil::SurfaceEffectType parseEffectType(std::string_view value)
        {
            if (value == "water")
                return SceneUtil::SurfaceEffectType::Water;
            throw std::domain_error("unsupported surface effect type '" + std::string(value) + "'");
        }

        /// Read one optional scalar glob while rejecting ambiguous non-scalar selectors.
        std::string optionalPattern(const YAML::Node& node, std::string_view field)
        {
            const YAML::Node value = node[std::string(field)];
            if (!value)
                return {};
            if (!value.IsScalar())
                throw std::domain_error("surface-effect field '" + std::string(field) + "' must be a scalar");
            return value.as<std::string>();
        }
    }

    SurfaceEffectRegistry::SurfaceEffectRegistry(const VFS::Manager& vfs)
    {
        std::vector<std::pair<VFS::Path::Normalized, YAML::Node>> documents;
        for (const VFS::Path::Normalized& path : vfs.getRecursiveDirectoryIterator(sRuleDirectory))
        {
            if (!path.value().ends_with(".yaml"))
                continue;
            try
            {
                const Files::IStreamPtr stream = vfs.get(path);
                documents.emplace_back(path, YAML::Load(std::string(std::istreambuf_iterator<char>(*stream), {})));
            }
            catch (const std::exception& error)
            {
                Log(Debug::Error) << "Failed to load surface-effect rules '" << path << "': " << error.what();
            }
        }
        std::ranges::sort(documents, {}, [](const auto& document) { return document.first.value(); });

        for (const auto& [path, document] : documents)
        {
            try
            {
                if (document["version"].as<int>(0) != 1)
                    throw std::domain_error("version must be 1");
                const YAML::Node profiles = document["profiles"];
                if (profiles && !profiles.IsMap())
                    throw std::domain_error("profiles must be a map");
                for (const auto& entry : profiles)
                {
                    SceneUtil::SurfaceEffectProfile profile;
                    profile.mId = entry.first.as<std::string>();
                    profile.mType = parseEffectType(entry.second["effect"].as<std::string>());
                    if (const YAML::Node waveHeight = entry.second["geometric-wave-height"])
                    {
                        profile.mGeometricWaveHeight = waveHeight.as<float>();
                        if (*profile.mGeometricWaveHeight < 0.f)
                            throw std::domain_error("geometric-wave-height cannot be negative");
                    }
                    const std::string profileId = profile.mId;
                    mProfiles.insert_or_assign(profileId, std::move(profile));
                }
            }
            catch (const std::exception& error)
            {
                Log(Debug::Error) << "Invalid surface-effect profiles in '" << path << "': " << error.what();
            }
        }

        for (const auto& [path, document] : documents)
        {
            try
            {
                if (document["version"].as<int>(0) != 1)
                    throw std::domain_error("version must be 1");
                const YAML::Node rules = document["rules"];
                if (rules && !rules.IsSequence())
                    throw std::domain_error("rules must be a sequence");
                for (const YAML::Node& node : rules)
                {
                    Rule rule;
                    rule.mProfile = node["profile"].as<std::string>();
                    rule.mMeshPattern = optionalPattern(node, "mesh");
                    rule.mMaterialPattern = optionalPattern(node, "material");
                    const YAML::Node textures = node["textures"];
                    if (textures)
                    {
                        if (!textures.IsSequence())
                            throw std::domain_error("rule textures must be a sequence");
                        for (const YAML::Node& texture : textures)
                            rule.mTexturePatterns.push_back(texture.as<std::string>());
                    }
                    if (rule.mMeshPattern.empty() && rule.mMaterialPattern.empty() && rule.mTexturePatterns.empty())
                        throw std::domain_error("a rule must select a mesh, material, or texture");
                    if (!mProfiles.contains(rule.mProfile))
                        throw std::domain_error("rule references unknown profile '" + rule.mProfile + "'");
                    mRules.push_back(std::move(rule));
                }
            }
            catch (const std::exception& error)
            {
                Log(Debug::Error) << "Invalid surface-effect rules in '" << path << "': " << error.what();
            }
        }
        Log(Debug::Info) << "Loaded " << mProfiles.size() << " surface-effect profiles and " << mRules.size()
                         << " material classification rules";
    }

    std::optional<SceneUtil::SurfaceEffectProfile> SurfaceEffectRegistry::match(VFS::Path::NormalizedView mesh,
        std::string_view material, const std::vector<VFS::Path::Normalized>& textures) const
    {
        const SceneUtil::SurfaceEffectProfile* result = nullptr;
        for (const Rule& rule : mRules)
        {
            if (!rule.mMeshPattern.empty() && !matchesGlob(mesh.value(), rule.mMeshPattern))
                continue;
            if (!rule.mMaterialPattern.empty() && !matchesGlob(material, rule.mMaterialPattern))
                continue;
            if (!rule.mTexturePatterns.empty()
                && std::none_of(textures.begin(), textures.end(), [&](const VFS::Path::Normalized& texture) {
                       return std::any_of(rule.mTexturePatterns.begin(), rule.mTexturePatterns.end(),
                           [&](const std::string& pattern) { return matchesGlob(texture.value(), pattern); });
                   }))
                continue;
            result = &mProfiles.at(rule.mProfile);
        }
        if (result == nullptr)
            return std::nullopt;
        return *result;
    }
}
