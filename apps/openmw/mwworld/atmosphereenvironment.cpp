#include "atmosphereenvironment.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <istream>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include <components/esm/exteriorcelllocation.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/vfs/manager.hpp>

#include "cellstore.hpp"
#include "class.hpp"
#include "esmstore.hpp"

namespace MWWorld
{
    namespace
    {
        std::string lowerCase(std::string_view value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return result;
        }

        /// Match a normalized token against transient engine names without allocating a lowercase copy.
        bool containsCaseInsensitive(std::string_view value, std::string_view token)
        {
            return std::search(value.begin(), value.end(), token.begin(), token.end(), [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left))
                    == std::tolower(static_cast<unsigned char>(right));
            }) != value.end();
        }

        /// Add one weighted state to an accumulating continuous environment blend.
        void accumulate(AtmosphereEnvironmentState& destination, const AtmosphereEnvironmentState& source, float weight)
        {
            for (const auto& [trait, contribution] : source.mTraits)
                destination.mTraits[trait] += contribution * weight;
        }

        /// Clamp environment evidence after interpolation or normalized sample accumulation.
        void clamp(AtmosphereEnvironmentState& state)
        {
            for (auto& [trait, contribution] : state.mTraits)
                contribution = std::clamp(contribution, 0.f, 1.f);
        }

        /// Add lower-authority evidence only where higher-authority evidence leaves uncertainty.
        void mergeLowerAuthority(AtmosphereEnvironmentState& destination, const AtmosphereEnvironmentState& source)
        {
            for (const auto& [trait, contribution] : source.mTraits)
            {
                float& current = destination.mTraits[trait];
                current += (1.f - current) * std::clamp(contribution, 0.f, 1.f);
            }
        }

        /// Evaluate a cubic transition while accepting either ascending or descending bounds.
        float smoothTransition(float value, float edge, float full)
        {
            if (edge == full)
                return value >= full ? 1.f : 0.f;
            const float factor = std::clamp((value - edge) / (full - edge), 0.f, 1.f);
            return factor * factor * (3.f - 2.f * factor);
        }
    }

    float AtmosphereEnvironmentState::get(std::string_view trait) const
    {
        const auto found = mTraits.find(std::string(trait));
        return found == mTraits.end() ? 0.f : std::clamp(found->second, 0.f, 1.f);
    }

    AtmosphereEnvironmentState makeAtmosphereOrographicState(const osg::Vec2f& sampleCenterWorldUnits,
        float centerHeightWorldUnits, float eastHeightWorldUnits, float westHeightWorldUnits,
        float northHeightWorldUnits, float southHeightWorldUnits, float sampleRadiusWorldUnits)
    {
        AtmosphereEnvironmentState result;
        result.mTerrainHeightWorldUnits = centerHeightWorldUnits;
        result.mTerrainSampleCenterWorldUnits = sampleCenterWorldUnits;
        if (!std::isfinite(sampleRadiusWorldUnits) || sampleRadiusWorldUnits <= 0.f)
            return result;

        const float diameter = sampleRadiusWorldUnits * 2.f;
        result.mTerrainGradient.set((eastHeightWorldUnits - westHeightWorldUnits) / diameter,
            (northHeightWorldUnits - southHeightWorldUnits) / diameter);
        const float surroundingHeight
            = (eastHeightWorldUnits + westHeightWorldUnits + northHeightWorldUnits + southHeightWorldUnits) * 0.25f;
        result.mValleyDepthWorldUnits = std::max(surroundingHeight - centerHeightWorldUnits, 0.f);
        return result;
    }

    float AtmosphereCondensateFormationController::update(float elapsedGameHours, bool paused,
        float precipitationWeight, float gameHour, float sunriseHour, const AtmosphereEnvironmentState& environment)
    {
        precipitationWeight = std::clamp(precipitationWeight, 0.f, 1.f);
        if (!paused && std::isfinite(elapsedGameHours) && elapsedGameHours > 0.f)
        {
            const float timeConstant = precipitationWeight > mRecentPrecipitation ? 0.35f : 4.f;
            const float response = 1.f - std::exp(-elapsedGameHours / timeConstant);
            mRecentPrecipitation = std::lerp(mRecentPrecipitation, precipitationWeight, response);
        }

        const float wrappedDistance = std::abs(gameHour - sunriseHour);
        const float dawnDistance = std::min(wrappedDistance, 24.f - wrappedDistance);
        const float dawnFactor = 1.f - smoothTransition(dawnDistance, 0.35f, 2.6f);
        const float wetlandWeight = environment.get("wetland");
        const float coastalWeight = environment.get("coastal");
        const float humidEnvironment = std::clamp(wetlandWeight + coastalWeight * 0.45f, 0.f, 1.f);
        const float coolAir = std::max(dawnFactor, 0.35f);
        const float precipitationMist = mRecentPrecipitation * coolAir * std::lerp(0.35f, 0.72f, humidEnvironment);
        const float dawnMist = dawnFactor * std::lerp(0.12f, 0.42f, humidEnvironment);
        return 1.f + precipitationMist + dawnMist;
    }

    void AtmosphereCondensateFormationController::reset()
    {
        mRecentPrecipitation = 0.f;
    }

    void AtmosphereCondensateFormationController::setRecentPrecipitation(float weight)
    {
        if (!std::isfinite(weight) || weight < 0.f || weight > 1.f)
            throw std::invalid_argument("recent precipitation requires a finite weight within [0, 1]");
        mRecentPrecipitation = weight;
    }

    /// Own parsed evidence policy and lazily cached immutable cell classifications.
    struct AtmosphereEnvironmentClassifier::Impl
    {
        struct Rule
        {
            std::vector<std::string> mContains;
            AtmosphereEnvironmentState mContributions;
        };

        struct TerrainEvidenceRule
        {
            float mEdge = 0.f;
            float mFull = 0.f;
            AtmosphereEnvironmentState mContributions;
            bool mEnabled = false;
        };

        struct CellContentEvidence
        {
            AtmosphereEnvironmentState mDiagnosticAssets;
            AtmosphereEnvironmentState mFlora;
            AtmosphereEnvironmentState mRegionFallback;
        };

        /// Parse strict, versioned rules for every supported evidence tier.
        explicit Impl(std::istream& stream)
        {
            const YAML::Node root = YAML::Load(stream);
            if (!root.IsMap() || root["version"].as<int>(0) != 1)
                throw std::runtime_error("OpenMWSE atmosphere environment rules require version 1");

            mTextureRules = readRules(root, "textureRules", true);
            mDiagnosticAssetRules = readRules(root, "diagnosticAssetRules", false);
            mFloraRules = readRules(root, "floraRules", false);
            mWeatherRules = readRules(root, "weatherProbabilityRules", false);
            mRegionFallbackRules = readRules(root, "regionFallbackRules", false);

            const YAML::Node terrain = root["terrainEvidence"];
            if (terrain)
            {
                if (!terrain.IsMap())
                    throw std::runtime_error("OpenMWSE atmosphere terrainEvidence must be a map");
                mWaterProximity = readTerrainRule(terrain["waterProximity"], "terrainEvidence.waterProximity");
                mElevation = readTerrainRule(terrain["elevation"], "terrainEvidence.elevation");
            }
        }

        /// Parse a homogeneous token-rule sequence with field-qualified validation errors.
        static std::vector<Rule> readRules(const YAML::Node& root, std::string_view field, bool required)
        {
            const YAML::Node rules = root[std::string(field)];
            if (!rules)
            {
                if (required)
                    throw std::runtime_error("OpenMWSE atmosphere environment rules must define " + std::string(field));
                return {};
            }
            if (!rules.IsSequence() || (required && rules.size() == 0))
                throw std::runtime_error(
                    "OpenMWSE atmosphere environment rules require a sequence: " + std::string(field));

            std::vector<Rule> result;
            result.reserve(rules.size());
            for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex)
            {
                const YAML::Node source = rules[ruleIndex];
                const std::string path
                    = "atmosphere-environments." + std::string(field) + "[" + std::to_string(ruleIndex) + "]";
                if (!source.IsMap() || !source["contains"].IsSequence() || !source["traits"].IsMap())
                    throw std::runtime_error("OpenMWSE atmosphere environment rule is malformed: " + path);

                Rule rule;
                for (const YAML::Node& token : source["contains"])
                {
                    const std::string normalized = lowerCase(token.as<std::string>());
                    if (normalized.empty())
                        throw std::runtime_error("OpenMWSE atmosphere environment token is empty: " + path);
                    rule.mContains.push_back(normalized);
                }
                if (rule.mContains.empty())
                    throw std::runtime_error("OpenMWSE atmosphere environment rule has no tokens: " + path);
                rule.mContributions = readTraits(source["traits"], path);
                result.push_back(std::move(rule));
            }
            return result;
        }

        /// Parse bounded trait contributions shared by token and terrain rules.
        static AtmosphereEnvironmentState readTraits(const YAML::Node& traits, std::string_view path)
        {
            AtmosphereEnvironmentState result;
            for (const auto& trait : traits)
            {
                const std::string name = lowerCase(trait.first.as<std::string>());
                const float contribution = trait.second.as<float>();
                if (name.empty() || !std::isfinite(contribution) || contribution <= 0.f || contribution > 1.f)
                    throw std::runtime_error("OpenMWSE atmosphere environment trait is invalid: " + std::string(path));
                result.mTraits.emplace(name, contribution);
            }
            if (result.mTraits.empty())
                throw std::runtime_error("OpenMWSE atmosphere environment rule has no traits: " + std::string(path));
            return result;
        }

        /// Parse one smooth terrain relation whose edge and full values define direction.
        static TerrainEvidenceRule readTerrainRule(const YAML::Node& source, std::string_view path)
        {
            if (!source)
                return {};
            if (!source.IsMap() || !source["traits"].IsMap())
                throw std::runtime_error("OpenMWSE atmosphere terrain rule is malformed: " + std::string(path));
            TerrainEvidenceRule result;
            result.mEdge = source["edge"].as<float>();
            result.mFull = source["full"].as<float>();
            if (!std::isfinite(result.mEdge) || !std::isfinite(result.mFull) || result.mEdge == result.mFull)
                throw std::runtime_error("OpenMWSE atmosphere terrain rule bounds are invalid: " + std::string(path));
            result.mContributions = readTraits(source["traits"], path);
            result.mEnabled = true;
            return result;
        }

        /// Attach the immutable world store used for lazily sampled terrain evidence.
        Impl(std::istream& stream, const ESMStore& store)
            : Impl(stream)
        {
            mStore = &store;
        }

        /// Match token samples against one evidence tier and normalize either by coverage or matching presence.
        static AtmosphereEnvironmentState classifyRules(std::span<const AtmosphereEnvironmentSample> samples,
            std::span<const Rule> rules, bool normalizeByAllSamples)
        {
            AtmosphereEnvironmentState result;
            float totalWeight = 0.f;
            for (const AtmosphereEnvironmentSample& sample : samples)
            {
                if (!std::isfinite(sample.mWeight) || sample.mWeight <= 0.f)
                    continue;
                const std::string value = lowerCase(sample.mValue);
                AtmosphereEnvironmentState sampleState;
                for (const Rule& rule : rules)
                {
                    const bool matches = std::any_of(rule.mContains.begin(), rule.mContains.end(),
                        [&](const std::string& token) { return value.find(token) != std::string::npos; });
                    if (matches)
                        accumulate(sampleState, rule.mContributions, 1.f);
                }
                clamp(sampleState);
                if (!normalizeByAllSamples && sampleState.mTraits.empty())
                    continue;
                accumulate(result, sampleState, sample.mWeight);
                totalWeight += sample.mWeight;
            }
            if (totalWeight > 0.f)
            {
                for (auto& [trait, contribution] : result.mTraits)
                    contribution /= totalWeight;
            }
            clamp(result);
            return result;
        }

        AtmosphereEnvironmentState classify(std::span<const AtmosphereEnvironmentSample> samples) const
        {
            return classifyRules(samples, mTextureRules, true);
        }

        /// Classify and cache terrain materials for one immutable exterior cell.
        AtmosphereEnvironmentState classifyTerrainCell(const ESM::ExteriorCellLocation& location) const
        {
            const auto cached = mTerrainCellCache.find(location);
            if (cached != mTerrainCellCache.end())
                return cached->second;

            AtmosphereEnvironmentState result;
            if (mStore != nullptr && !ESM::isEsm4Ext(location.mWorldspace))
            {
                const ESM::Land* land = mStore->get<ESM::Land>().search(location.mX, location.mY);
                const ESM::Land::LandData* data = land ? land->getLandData(ESM::Land::DATA_VTEX) : nullptr;
                if (data != nullptr)
                {
                    std::vector<AtmosphereEnvironmentSample> samples;
                    samples.reserve(data->mTextures.size());
                    for (const std::uint16_t texture : data->mTextures)
                    {
                        if (texture == 0)
                            continue;
                        const ESM::Path* path = mStore->get<ESM::LandTexture>().search(texture - 1, land->getPlugin());
                        if (path != nullptr)
                            samples.push_back({ path->getNormalized().value(), 1.f });
                    }
                    result = classify(samples);
                }
            }
            mTerrainCellCache.emplace(location, result);
            return result;
        }

        /// Bilinearly blend cached terrain evidence around a world position.
        AtmosphereEnvironmentState classifyTerrain(const osg::Vec3f& worldPosition, ESM::RefId worldspace) const
        {
            if (mStore == nullptr)
                return {};
            const float cellSize = static_cast<float>(ESM::getCellSize(worldspace));
            const float centerX = worldPosition.x() / cellSize - 0.5f;
            const float centerY = worldPosition.y() / cellSize - 0.5f;
            const int lowerX = static_cast<int>(std::floor(centerX));
            const int lowerY = static_cast<int>(std::floor(centerY));
            const float blendX = centerX - static_cast<float>(lowerX);
            const float blendY = centerY - static_cast<float>(lowerY);

            AtmosphereEnvironmentState result;
            accumulate(result, classifyTerrainCell({ lowerX, lowerY, worldspace }), (1.f - blendX) * (1.f - blendY));
            accumulate(result, classifyTerrainCell({ lowerX + 1, lowerY, worldspace }), blendX * (1.f - blendY));
            accumulate(result, classifyTerrainCell({ lowerX, lowerY + 1, worldspace }), (1.f - blendX) * blendY);
            accumulate(result, classifyTerrainCell({ lowerX + 1, lowerY + 1, worldspace }), blendX * blendY);
            clamp(result);
            return result;
        }

        /// Inspect loaded object IDs and models once per cell to derive diagnostic-asset and flora evidence.
        CellContentEvidence classifyCellContent(const CellStore& cell) const
        {
            const ESM::RefId cellId = cell.getCell()->getId();
            const auto cached = mCellContentCache.find(cellId);
            if (cached != mCellContentCache.end())
                return cached->second;

            constexpr std::size_t maximumEvidenceDescriptors = 4096;
            std::set<std::string> descriptors;
            const bool traversed = cell.forEachConst([&](const ConstPtr& ptr) {
                if (descriptors.size() >= maximumEvidenceDescriptors)
                    return false;
                std::string descriptor = ptr.getCellRef().getRefId().serializeText();
                const VFS::Path::NormalizedView model = ptr.getClass().getModel(ptr);
                if (!model.empty())
                    descriptor += " " + std::string(model.value());
                descriptors.insert(lowerCase(descriptor));
                return true;
            });
            if (!traversed && descriptors.empty())
                return {};

            std::vector<AtmosphereEnvironmentSample> samples;
            samples.reserve(descriptors.size());
            for (const std::string& descriptor : descriptors)
                samples.push_back({ descriptor, 1.f });

            CellContentEvidence result;
            result.mDiagnosticAssets = classifyRules(samples, mDiagnosticAssetRules, false);
            result.mFlora = classifyRules(samples, mFloraRules, false);
            std::array regionSamples{ AtmosphereEnvironmentSample{ cell.getCell()->getRegion().serializeText(), 1.f },
                AtmosphereEnvironmentSample{ {}, 1.f } };
            if (mStore != nullptr)
            {
                if (const ESM::Region* region = mStore->get<ESM::Region>().search(cell.getCell()->getRegion()))
                    regionSamples[1].mValue = region->mName;
            }
            result.mRegionFallback = classifyRules(regionSamples, mRegionFallbackRules, false);
            mCellContentCache.emplace(cellId, result);
            return result;
        }

        /// Convert smooth terrain relations into bounded local evidence.
        AtmosphereEnvironmentState classifyTerrainRelations(const AtmosphereEnvironmentEvidence& evidence) const
        {
            AtmosphereEnvironmentState result;
            if (!evidence.mTerrainHeightWorldUnits)
                return result;
            if (mWaterProximity.mEnabled && evidence.mWaterHeightWorldUnits)
            {
                const float heightAboveWater = *evidence.mTerrainHeightWorldUnits - *evidence.mWaterHeightWorldUnits;
                accumulate(result, mWaterProximity.mContributions,
                    smoothTransition(heightAboveWater, mWaterProximity.mEdge, mWaterProximity.mFull));
            }
            if (mElevation.mEnabled)
            {
                accumulate(result, mElevation.mContributions,
                    smoothTransition(*evidence.mTerrainHeightWorldUnits, mElevation.mEdge, mElevation.mFull));
            }
            clamp(result);
            return result;
        }

        /// Convert regional weather probabilities into weak data-defined climate evidence.
        AtmosphereEnvironmentState classifyWeather(std::span<const AtmosphereEnvironmentWeatherSample> weather) const
        {
            AtmosphereEnvironmentState result;
            float totalProbability = 0.f;
            for (const AtmosphereEnvironmentWeatherSample& sample : weather)
            {
                if (!std::isfinite(sample.mProbability) || sample.mProbability <= 0.f)
                    continue;
                AtmosphereEnvironmentState sampleState;
                for (const Rule& rule : mWeatherRules)
                {
                    const bool matches = std::any_of(rule.mContains.begin(), rule.mContains.end(),
                        [&](const std::string& token) { return containsCaseInsensitive(sample.mWeather, token); });
                    if (matches)
                        accumulate(sampleState, rule.mContributions, 1.f);
                }
                clamp(sampleState);
                accumulate(result, sampleState, sample.mProbability);
                totalProbability += sample.mProbability;
            }
            if (totalProbability > 0.f)
            {
                for (auto& [trait, contribution] : result.mTraits)
                    contribution /= totalProbability;
            }
            clamp(result);
            return result;
        }

        /// Resolve ordered evidence, preserving exact overrides and a single continuous trait output.
        AtmosphereEnvironmentState resolve(const AtmosphereEnvironmentEvidence& evidence,
            const std::optional<AtmosphereEnvironmentState>& classifiedTerrain = std::nullopt,
            const std::optional<CellContentEvidence>& classifiedContent = std::nullopt,
            std::span<const AtmosphereEnvironmentWeatherSample> runtimeWeather = {}) const
        {
            if (evidence.mOverride)
                return *evidence.mOverride;

            AtmosphereEnvironmentState result = classifyTerrainRelations(evidence);
            mergeLowerAuthority(
                result, classifiedTerrain.value_or(classifyRules(evidence.mTerrainMaterials, mTextureRules, true)));

            const CellContentEvidence content = classifiedContent.value_or(
                CellContentEvidence{ classifyRules(evidence.mDiagnosticAssets, mDiagnosticAssetRules, false),
                    classifyRules(evidence.mFlora, mFloraRules, false), {} });
            mergeLowerAuthority(result, content.mDiagnosticAssets);
            mergeLowerAuthority(result, content.mFlora);
            mergeLowerAuthority(result,
                classifyWeather(runtimeWeather.empty()
                        ? std::span<const AtmosphereEnvironmentWeatherSample>(evidence.mWeatherProbabilities)
                        : runtimeWeather));
            if (evidence.mMacroClimatePrior)
                mergeLowerAuthority(result, *evidence.mMacroClimatePrior);

            if (classifiedContent)
                mergeLowerAuthority(result, content.mRegionFallback);
            else
            {
                const std::array regionSamples{ AtmosphereEnvironmentSample{ evidence.mRegionId, 1.f },
                    AtmosphereEnvironmentSample{ evidence.mRegionName, 1.f } };
                mergeLowerAuthority(result, classifyRules(regionSamples, mRegionFallbackRules, false));
            }
            clamp(result);
            result.mTerrainHeightWorldUnits = evidence.mTerrainHeightWorldUnits.value_or(0.f);
            return result;
        }

        AtmosphereEnvironmentState classify(const AtmosphereEnvironmentEvidence& evidence) const
        {
            return resolve(evidence);
        }

        AtmosphereEnvironmentState classify(const osg::Vec3f& worldPosition, const CellStore& cell,
            float terrainHeightWorldUnits, std::span<const AtmosphereEnvironmentWeatherSample> weatherProbabilities,
            const std::optional<AtmosphereEnvironmentState>& macroClimatePrior) const
        {
            AtmosphereEnvironmentEvidence evidence;
            evidence.mTerrainHeightWorldUnits = terrainHeightWorldUnits;
            if (cell.getCell()->hasWater())
                evidence.mWaterHeightWorldUnits = cell.getWaterLevel();
            evidence.mMacroClimatePrior = macroClimatePrior;

            const AtmosphereEnvironmentState terrain = classifyTerrain(worldPosition, cell.getCell()->getWorldSpace());
            const CellContentEvidence content = classifyCellContent(cell);
            return resolve(evidence, terrain, content, weatherProbabilities);
        }

        std::vector<Rule> mTextureRules;
        std::vector<Rule> mDiagnosticAssetRules;
        std::vector<Rule> mFloraRules;
        std::vector<Rule> mWeatherRules;
        std::vector<Rule> mRegionFallbackRules;
        TerrainEvidenceRule mWaterProximity;
        TerrainEvidenceRule mElevation;
        const ESMStore* mStore = nullptr;
        mutable std::map<ESM::ExteriorCellLocation, AtmosphereEnvironmentState> mTerrainCellCache;
        mutable std::map<ESM::RefId, CellContentEvidence> mCellContentCache;
    };

    AtmosphereEnvironmentClassifier::AtmosphereEnvironmentClassifier(const VFS::Manager& vfs, const ESMStore& store)
        : mImpl(std::make_unique<Impl>(
              *vfs.get(VFS::Path::NormalizedView("openmwse/atmosphere-environments.json")), store))
    {
    }

    AtmosphereEnvironmentClassifier::AtmosphereEnvironmentClassifier(std::istream& stream)
        : mImpl(std::make_unique<Impl>(stream))
    {
    }

    AtmosphereEnvironmentClassifier::~AtmosphereEnvironmentClassifier() = default;

    AtmosphereEnvironmentState AtmosphereEnvironmentClassifier::classify(
        std::span<const AtmosphereEnvironmentSample> samples) const
    {
        return mImpl->classify(samples);
    }

    AtmosphereEnvironmentState AtmosphereEnvironmentClassifier::classify(
        const AtmosphereEnvironmentEvidence& evidence) const
    {
        return mImpl->classify(evidence);
    }

    AtmosphereEnvironmentState AtmosphereEnvironmentClassifier::classify(const osg::Vec3f& worldPosition,
        const CellStore& cell, float terrainHeightWorldUnits,
        std::span<const AtmosphereEnvironmentWeatherSample> weatherProbabilities,
        const std::optional<AtmosphereEnvironmentState>& macroClimatePrior) const
    {
        return mImpl->classify(worldPosition, cell, terrainHeightWorldUnits, weatherProbabilities, macroClimatePrior);
    }
}
