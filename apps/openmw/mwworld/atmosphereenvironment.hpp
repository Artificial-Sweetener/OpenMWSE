#ifndef OPENMW_MWWORLD_ATMOSPHEREENVIRONMENT_H
#define OPENMW_MWWORLD_ATMOSPHEREENVIRONMENT_H

#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/esm/refid.hpp>

namespace VFS
{
    class Manager;
}

namespace MWWorld
{
    class CellStore;
    class ESMStore;

    /// Carries continuous, data-defined environmental evidence without assigning a region or biome label.
    struct AtmosphereEnvironmentState
    {
        std::map<std::string, float, std::less<>> mTraits;
        float mTerrainHeightWorldUnits = 0.f;
        osg::Vec2f mTerrainSampleCenterWorldUnits;
        osg::Vec2f mTerrainGradient;
        float mValleyDepthWorldUnits = 0.f;

        /// Return the bounded contribution of a named environmental trait.
        float get(std::string_view trait) const;
    };

    /// Derive a local terrain plane and concavity from one center and four cardinal terrain samples.
    AtmosphereEnvironmentState makeAtmosphereOrographicState(const osg::Vec2f& sampleCenterWorldUnits,
        float centerHeightWorldUnits, float eastHeightWorldUnits, float westHeightWorldUnits,
        float northHeightWorldUnits, float southHeightWorldUnits, float sampleRadiusWorldUnits);

    /// Integrates recent precipitation and diurnal cooling into a bounded mist-formation response.
    class AtmosphereCondensateFormationController
    {
    public:
        /// Advance formation state in game hours and return the density multiplier for patchy mist only.
        float update(float elapsedGameHours, bool paused, float precipitationWeight, float gameHour, float sunriseHour,
            const AtmosphereEnvironmentState& environment);

        /// Forget transient weather history after a discontinuous clock or world reset.
        void reset();

        /// Set bounded retained moisture for isolated diagnostics without changing weather or save state.
        void setRecentPrecipitation(float weight);

        /// Return retained precipitation moisture for diagnostics and deterministic tests.
        float getRecentPrecipitation() const { return mRecentPrecipitation; }

    private:
        float mRecentPrecipitation = 0.f;
    };

    /// Describes one weighted terrain-material observation for deterministic classifier tests and tooling.
    struct AtmosphereEnvironmentSample
    {
        std::string mValue;
        float mWeight = 1.f;
    };

    /// Describes one regional weather probability used as weak environmental evidence.
    struct AtmosphereEnvironmentWeatherSample
    {
        std::string_view mWeather;
        float mProbability = 0.f;
    };

    /// Collects ordered local evidence while reserving an optional world-scale climate prior.
    struct AtmosphereEnvironmentEvidence
    {
        std::optional<AtmosphereEnvironmentState> mOverride;
        std::optional<float> mTerrainHeightWorldUnits;
        std::optional<float> mWaterHeightWorldUnits;
        std::vector<AtmosphereEnvironmentSample> mTerrainMaterials;
        std::vector<AtmosphereEnvironmentSample> mDiagnosticAssets;
        std::vector<AtmosphereEnvironmentSample> mFlora;
        std::vector<AtmosphereEnvironmentWeatherSample> mWeatherProbabilities;
        std::string mRegionId;
        std::string mRegionName;
        std::optional<AtmosphereEnvironmentState> mMacroClimatePrior;
    };

    /// Classifies cached terrain-material evidence into continuous atmosphere traits.
    class AtmosphereEnvironmentClassifier
    {
    public:
        /// Load environment evidence rules from the runtime VFS and classify terrain from the world store.
        AtmosphereEnvironmentClassifier(const VFS::Manager& vfs, const ESMStore& store);
        /// Load environment evidence rules from a stream for deterministic tests and data validation.
        explicit AtmosphereEnvironmentClassifier(std::istream& stream);
        ~AtmosphereEnvironmentClassifier();

        AtmosphereEnvironmentClassifier(const AtmosphereEnvironmentClassifier&) = delete;
        AtmosphereEnvironmentClassifier& operator=(const AtmosphereEnvironmentClassifier&) = delete;

        /// Classify arbitrary weighted terrain texture paths using the configured semantic evidence rules.
        AtmosphereEnvironmentState classify(std::span<const AtmosphereEnvironmentSample> samples) const;
        /// Resolve all supplied evidence in deterministic descending-authority order.
        AtmosphereEnvironmentState classify(const AtmosphereEnvironmentEvidence& evidence) const;
        /// Classify a loaded exterior cell using cached content evidence and current regional climatology.
        AtmosphereEnvironmentState classify(const osg::Vec3f& worldPosition, const CellStore& cell,
            float terrainHeightWorldUnits, std::span<const AtmosphereEnvironmentWeatherSample> weatherProbabilities,
            const std::optional<AtmosphereEnvironmentState>& macroClimatePrior = std::nullopt) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
