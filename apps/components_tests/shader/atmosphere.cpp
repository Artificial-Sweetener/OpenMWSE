#include <components/fx/pass.hpp>
#include <components/fx/stateupdater.hpp>
#include <components/fx/technique.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/cloudshaderstate.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>

#include "cloudmodelreference.hpp"

#include <apps/openmw/mwrender/atmosphere.hpp>

#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Image>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/Vec3d>
#include <osg/Viewport>
#include <osgDB/ReadFile>
#include <osgViewer/Viewer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include <SDL_opengl_glext.h>
#include <yaml-cpp/yaml.h>

namespace
{
    /// Represent a linear RGB color used by the independent scattering reference calculations.
    struct Rgb
    {
        float r;
        float g;
        float b;
    };

    /// Own floating-point output captured from one native atmosphere pass.
    struct PassImage
    {
        int width;
        int height;
        std::vector<osg::Vec4f> pixels;

        /// Return the captured pixel at the requested image coordinate.
        const osg::Vec4f& at(int x, int y) const { return pixels[static_cast<std::size_t>(y * width + x)]; }
    };

    /// Return mean absolute scene-linear RGB difference between equally sized pass captures.
    double meanRgbDifference(const PassImage& first, const PassImage& second)
    {
        if (first.pixels.size() != second.pixels.size() || first.pixels.empty())
            throw std::invalid_argument("atmosphere pass comparison requires equal non-empty captures");
        double difference = 0.0;
        for (std::size_t index = 0; index < first.pixels.size(); ++index)
        {
            for (int channel = 0; channel < 3; ++channel)
                difference += std::abs(first.pixels[index][channel] - second.pixels[index][channel]);
        }
        return difference / static_cast<double>(first.pixels.size() * 3);
    }

    /// Select the production height guard or an independent comparison of the condensate's skylight contribution.
    enum class CloudHeightReference
    {
        Production,
        Unconditional,
        Omitted,
    };

    /// Describe deterministic textures, atmosphere state, and camera history for a native pass execution.
    struct NativePassInputs
    {
        std::function<osg::Vec4f(int, int)> lastShader = [](int, int) { return osg::Vec4f(0.3f, 0.4f, 0.55f, 1.f); };
        std::function<float(int, int)> depth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f; };
        std::function<float(int, int)> previousDepth
            = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f; };
        std::function<osg::Vec4f(int, int)> atmosphereHistory = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        std::function<osg::Vec4f(int, int)> atmosphereMetadataHistory;
        std::function<osg::Vec4f(int, int)> passInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        std::function<osg::Vec4f(int, int)> volumeHistoryInput;
        std::function<osg::Vec4f(int, int)> volumeMetadataInput;
        std::function<osg::Vec4f(int, int)> shaftInput = [](int, int) { return osg::Vec4f(); };
        int passWidth = 0;
        int passHeight = 0;
        int rawVolumeWidth = 0;
        int rawVolumeHeight = 0;
        int volumeHistoryWidth = 0;
        int volumeHistoryHeight = 0;
        int volumeMetadataWidth = 0;
        int volumeMetadataHeight = 0;
        int shaftWidth = 0;
        int shaftHeight = 0;
        SceneUtil::CloudLayer cloudModel;
        SceneUtil::CloudLayer incomingCloudModel;
        SceneUtil::CloudLayer environmentalCloudModel;
        osg::Vec4f cloudParameters;
        osg::Vec4f incomingCloudParameters;
        osg::Vec4f cloudScenarioParameters;
        osg::Vec4f lowCloudParameters;
        float lowCloudEnvironmentInfluence = 1.f;
        osg::Vec4f mistParameters{ 1.f, 800.f, 4500.f, 1.f };
        osg::Vec4f fogVolumeParameters;
        osg::Vec4f mediumReferenceParameters;
        osg::Vec4f orographicParameters;
        std::function<float(const osg::Vec2f&)> terrainHeight;
        osg::Vec4f sunDirection{ 1.f, 0.f, 0.f, 0.f };
        osg::Vec4f fogColor{ 0.45f, 0.5f, 0.6f, 1.f };
        osg::Vec4f skyColor{ 0.3f, 0.4f, 0.55f, 1.f };
        osg::Vec4f sunColor{ 1.f, 0.85f, 0.65f, 1.f };
        osg::Vec4f sunDiscColor{ 1.f, 0.85f, 0.65f, 1.f };
        osg::Vec3f inScattering{ 0.25f, 0.38f, 0.48f };
        osg::Vec3f outScattering{ 0.07f, 0.36f, 0.76f };
        osg::Vec3f solarRadiance{ 1.f, 0.85f, 0.65f };
        osg::Vec3f skyIrradiance{ 0.3f, 0.4f, 0.55f };
        osg::Vec3f groundIrradiance{ 0.45f, 0.5f, 0.6f };
        osg::Vec3f eyePosition{ 0.f, 0.f, 1200.f };
        osg::Vec3f viewDirection{ 1.f, 0.f, 0.f };
        osg::Vec3f previousEyeOffset;
        std::optional<osg::Vec3f> previousViewDirection;
        osg::Vec2f cloudAdvectionOffsetWorldUnits;
        osg::Vec2f cloudAdvectionDeltaWorldUnits;
        float fogNear = 1000.f;
        float fogFar = 20000.f;
        float nearPlane = 1.f;
        float farPlane = 20000.f;
        float verticalFov = 70.f;
        float sunVisibility = 1.f;
        float historyLightingWeight = 1.f;
        float weatherWeight = 1.f;
        float simulationTime = 0.f;
        float deltaSimulationTime = 0.f;
        float windSpeed = 0.f;
        double previousVerticalFov = 70.0;
        std::array<osg::Vec4f, SceneUtil::PPLightBuffer::sMaxPPLightsArraySize> pointLights{};
        int pointLightCount = 0;
        bool historyValid = false;
        bool atmosphereEnabled = true;
        bool scatteringEnabled = true;
        bool volumetricCloudsEnabled = true;
        int atmosphereAppearance = 0;
        bool sunlightEnabled = false;
        bool godraysEnabled = true;
        bool sunGlareEnabled = false;
        bool sunGlareAllowed = true;
        bool cloudSunOcclusion = true;
        SceneUtil::TexturedCloudState texturedClouds;
        osg::ref_ptr<osg::Texture2D> sunGlareTexture;
        bool reverseZ = false;
        std::optional<bool> exponentialFog;
        bool useUniformBuffer = false;
        bool useGeneralCloudShader = false;
        bool bindUniformBufferOnAncestor = false;
        bool renderReprojectionCoordinates = false;
        int width = 64;
        int height = 64;
        int sceneWidth = 0;
        int sceneHeight = 0;
        int quality = 2;
        int frameNumber = 4;
        float volumeResolutionScale = 0.5f;
        osg::Vec2f cloudDensitySliceExtent{ 500000.f, 500000.f };
        int cloudDensitySliceAxis = 0;
        int cloudDensitySliceQuantity = 0;
        bool renderCloudDensitySlice = false;
        bool renderCloudSkyReference = false;
        bool renderTexturedCloudProjection = false;
        bool renderTexturedCloudTransmission = false;
        bool renderTexturedCloudDome = false;
        bool renderAuthoredCloudPass = false;
        bool renderFogTransport = false;
        bool fullPrecisionCapture = false;
        int cloudTransportReferenceSubdivisions = 1;
        bool measureCloudMarchProgress = false;
        bool useLegacyCloudMarchReference = false;
        bool usePerSampleCloudIntervalReference = false;
        bool useUnculledLowCloudReference = false;
        bool useSeparateCloudFogReference = false;
        CloudHeightReference cloudHeightReference = CloudHeightReference::Production;
        bool useUncachedLocalLighting = false;
        bool resolveReprojectedVolume = false;
    };

    /// Reject ambiguous shader fixture substitutions rather than silently testing a different path.
    void replaceUniqueShaderText(std::string& source, const std::string& marker, const std::string& replacement)
    {
        const auto position = source.find(marker);
        if (position == std::string::npos || source.find(marker, position + marker.size()) != std::string::npos)
            throw std::runtime_error("Atmosphere shader fixture requires a unique production anchor");
        source.replace(position, marker.size(), replacement);
    }

    /// Compare interval ownership while sharing the production density, lighting, and transport body.
    std::string makePerSampleCloudIntervalReference(std::string source)
    {
        const auto start = source.find("    int sampleIndex = 0;\n    for (int intervalIndex = 0;");
        const auto body = source.find("            float remainingIntervalLength", start);
        const auto end = source.find("\n    openmwseAdvanceLocalMedia(localMedia, maximumDistance", body);
        if (start == std::string::npos || body == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Cloud interval reference requires the production transport loop");
        std::string transport = source.substr(body, end - body);
        replaceUniqueShaderText(transport, "                    sampleIndex = maximumSampleCount;\n", "");
        replaceUniqueShaderText(transport, "\n    }\n", "\n");
        source.replace(start, end - start, R"glsl(
    for (int sampleIndex = 0; sampleIndex < maximumSampleCount; ++sampleIndex)
    {
        if (distanceAlongCloudIntervals >= traversedIntervalLength)
            break;
        bool sampleSecondInterval = separatedIntervals && distanceAlongCloudIntervals >= firstIntervalLength;
        bool activeIntervalIsMain
            = sampleSecondInterval ? !firstIntervalIsMain : firstIntervalIsMain;
        float intervalPathOffset
            = sampleSecondInterval ? distanceAlongCloudIntervals - firstIntervalLength : distanceAlongCloudIntervals;
        vec2 activeInterval = sampleSecondInterval ? secondInterval : firstInterval;
        float activeIntervalLength = sampleSecondInterval ? secondIntervalLength : firstIntervalLength;
        float activeIntervalEnd = sampleSecondInterval ? traversedIntervalLength : firstIntervalLength;
        float fineStepLength = sampleSecondInterval ? secondFineStepLength : firstFineStepLength;
)glsl" + transport);
        return source;
    }

    /// Retain the original accumulation only as a reference for rays that completed without repeated steps.
    std::string makeLegacyCloudMarchReference(std::string source)
    {
        replaceUniqueShaderText(source,
            "return stepLength >= remainingIntervalLength ? intervalEndDistance : distanceAlongIntervals + stepLength;",
            "return distanceAlongIntervals + stepLength;");
        replaceUniqueShaderText(source, "distanceAlongCloudIntervals = activeIntervalEnd;",
            "distanceAlongCloudIntervals = firstIntervalLength;");
        return source;
    }

    /// Observe repeated march positions without replacing production interval or sampling calculations.
    std::string instrumentCloudMarchProgress(std::string source)
    {
        replaceUniqueShaderText(source, "vec4 openmwseIntegrateCloudsInternal(",
            "vec2 openmwseTestMarchProgress = vec2(0.0);\nvec4 openmwseIntegrateCloudsInternal(");
        replaceUniqueShaderText(source, "    float distanceAlongCloudIntervals = 0.0;",
            "    float previousDiagnosticDistance = -1.0;\n    float distanceAlongCloudIntervals = 0.0;");
        replaceUniqueShaderText(source, "float intervalPathOffset\n",
            "        openmwseTestMarchProgress.x += 1.0;\n"
            "        if (distanceAlongCloudIntervals <= previousDiagnosticDistance)\n"
            "            openmwseTestMarchProgress.y += 1.0;\n"
            "        previousDiagnosticDistance = distanceAlongCloudIntervals;\n"
            "        float intervalPathOffset\n");
        const auto output = source.rfind("    omw_FragColor1 = metadata;");
        if (output == std::string::npos)
            throw std::runtime_error("Cloud march instrumentation requires the ray-grid output");
        source.insert(output, "    omw_FragColor = vec4(openmwseTestMarchProgress, 0.0, 1.0);\n");
        return source;
    }

    /// Return the representative shader permutation used by atmosphere rendering tests.
    Shader::ShaderManager::DefineMap getAtmosphereTestDefines();

    /// Read one validated RGB triplet from repository-owned atmosphere fixture data.
    osg::Vec3f readAtmosphereFixtureColor(const YAML::Node& node)
    {
        if (!node.IsSequence() || node.size() != 3)
            throw std::runtime_error("Production atmosphere fixture color must contain three channels");
        return { node[0].as<float>(), node[1].as<float>(), node[2].as<float>() };
    }

    /// Own parsed atmosphere palette and volume data loaded through the packaged runtime VFS layout.
    struct PackagedAtmosphereFixtureData
    {
        YAML::Node mPalette;
        YAML::Node mVolumes;
    };

    /// Load canonical production atmosphere data from the built package once per test process.
    const PackagedAtmosphereFixtureData& getPackagedAtmosphereFixtureData()
    {
        static const PackagedAtmosphereFixtureData data = [] {
            VFS::Manager vfs;
            vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(
                std::filesystem::path(OPENMW_PROJECT_BINARY_DIR) / "resources/vfs-mw"));
            vfs.buildIndex();
            auto paletteStream = vfs.get(VFS::Path::NormalizedView("openmwse/atmosphere.json"));
            auto volumeStream = vfs.get(VFS::Path::NormalizedView("openmwse/atmosphere-volumes.json"));
            return PackagedAtmosphereFixtureData{ YAML::Load(*paletteStream), YAML::Load(*volumeStream) };
        }();
        return data;
    }

    /// Build native-pass inputs from the same packaged palette and weather volumes consumed by production.
    NativePassInputs makePackagedAtmosphereInputs(
        std::string_view weatherName, std::string_view timeOfDay, std::string_view presetName = "default")
    {
        const PackagedAtmosphereFixtureData& packagedData = getPackagedAtmosphereFixtureData();
        const YAML::Node& palette = packagedData.mPalette;
        const YAML::Node& volumes = packagedData.mVolumes;
        const YAML::Node preset = palette["presets"][std::string(presetName)];
        const YAML::Node weather = preset[std::string(weatherName)];
        const YAML::Node volume = volumes[std::string(weatherName)];
        if (!preset.IsMap() || !weather.IsMap() || !volume.IsMap())
            throw std::runtime_error("Production atmosphere fixture references unknown palette or weather data");

        const std::string phase(timeOfDay);
        NativePassInputs result;
        result.inScattering = readAtmosphereFixtureColor(preset["inscatter"]);
        result.outScattering = readAtmosphereFixtureColor(preset["outscatter"]);
        result.skyColor = osg::Vec4f(readAtmosphereFixtureColor(weather["sky" + phase + "Color"]), 1.f);
        result.fogColor = osg::Vec4f(readAtmosphereFixtureColor(weather["fog" + phase + "Color"]), 1.f);
        result.sunColor = osg::Vec4f(readAtmosphereFixtureColor(weather["sun" + phase + "Color"]), 1.f);
        const osg::Vec3f ambientColor = readAtmosphereFixtureColor(weather["ambient" + phase + "Color"]);
        const MWRender::AtmosphereLightingState lighting
            = MWRender::makeAtmosphereLightingState(result.sunColor, osg::Vec4f(ambientColor, 1.f), result.fogColor);
        result.solarRadiance = lighting.mSolarRadiance;
        result.skyIrradiance = lighting.mSkyIrradiance;
        result.groundIrradiance = lighting.mGroundIrradiance;
        result.sunDiscColor = phase == "Sunset"
            ? osg::Vec4f(readAtmosphereFixtureColor(weather["sundiscSunsetColor"]), 1.f)
            : osg::Vec4f(1.f, 1.f, 1.f, 1.f);
        result.cloudParameters = { volume["cloudDeckCoverage"].as<float>(), volume["cloudDensity"].as<float>(),
            volume["cloudAltitude"].as<float>(), volume["cloudThickness"].as<float>() };
        const auto fixtureModel = [](const YAML::Node& source) {
            const std::string profile = source["profile"].as<std::string>();
            SceneUtil::CloudLayer model = profile == "secondary" ? SceneUtil::secondaryCloudLayer()
                : profile == "fair-weather"                      ? SceneUtil::fairWeatherCloudLayer()
                                                                 : SceneUtil::CloudLayer{};
            const float scale = source["physicalScale"].as<float>();
            model.mPhysicalScale = scale;
            model.mNoiseScalePerMeter /= scale;
            model.mWeatherScalePerMeter /= scale;
            model.mShapeSkewMeters *= scale;
            model.mCoverageSkewMeters *= scale;
            model.mCurlDisplacementMeters *= scale;
            model.mWindSpeedMetersPerSecond *= scale;
            model.mCoverageWindSpeedMetersPerSecond *= scale;
            model.mCoverageAmount = source["coverageAmount"].as<float>();
            model.mCoverageMinimum = source["coverageMinimum"] ? source["coverageMinimum"].as<float>() : 0.f;
            model.mTypeAmount = source["typeAmount"].as<float>();
            model.mTypeMinimum = source["typeMinimum"].as<float>();
            model.mRainAmount = source["rainAmount"] ? source["rainAmount"].as<float>() : 0.f;
            return model;
        };
        result.cloudModel = fixtureModel(volume["cloudLayer"]);
        result.environmentalCloudModel = fixtureModel(volume["environmentalCloudLayer"]);
        result.lowCloudParameters = { volume["lowCloudCoverage"].as<float>(), volume["lowCloudDensity"].as<float>(),
            volume["lowCloudAltitude"].as<float>(), volume["lowCloudThickness"].as<float>() };
        result.lowCloudEnvironmentInfluence = volume["lowCloudEnvironmentInfluence"].as<float>();
        result.mistParameters = { volume["mistDensity"].as<float>(), volume["mistAltitude"].as<float>(),
            volume["mistThickness"].as<float>(), volume["shaftIntensity"].as<float>() };
        result.fogVolumeParameters = { volume["fogDensity"].as<float>(), volume["fogAltitude"].as<float>(),
            volume["fogThickness"].as<float>(), volume["fogAnisotropy"].as<float>() };
        result.mediumReferenceParameters
            = { 0.f, volume["cloudTerrainFollowing"].as<float>(), volume["cloudTerrainClearance"].as<float>(), 0.f };
        result.weatherWeight = weatherName == "Clear" || weatherName == "Cloudy" ? 1.f : 0.f;
        result.sunDirection
            = phase == "Day" ? osg::Vec4f(0.25f, 0.f, 0.9682458f, 0.f) : osg::Vec4f(0.9950372f, 0.f, 0.0995037f, 0.f);
        return result;
    }

    /// Encode world-space ray distance using the production metadata transfer function.
    float encodeAtmosphereDistance(float distance)
    {
        return std::log2(1.f + std::max(distance, 0.f)) / 20.f;
    }

    /// Convert positive view-axis distance to the active normalized depth convention.
    float normalizedDepthForViewDistance(float viewDistance, bool reverseZ)
    {
        constexpr float nearPlane = 1.f;
        constexpr float farPlane = 20000.f;
        const float standardDepth
            = farPlane / (farPlane - nearPlane) - farPlane * nearPlane / ((farPlane - nearPlane) * viewDistance);
        return reverseZ ? 1.f - standardDepth : standardDepth;
    }

    /// Calculate world-space camera-ray distance for a perspective pixel at a known view-axis depth.
    float perspectiveRayDistance(int x, int y, int width, int height, float verticalFovDegrees, float viewDistance)
    {
        const float normalizedX = (2.f * (static_cast<float>(x) + 0.5f) / static_cast<float>(width)) - 1.f;
        const float normalizedY = (2.f * (static_cast<float>(y) + 0.5f) / static_cast<float>(height)) - 1.f;
        const float tangentHalfVerticalFov = std::tan(verticalFovDegrees * 0.5f * std::numbers::pi_v<float> / 180.f);
        const float aspectRatio = static_cast<float>(width) / static_cast<float>(height);
        const float horizontalOffset = normalizedX * aspectRatio * tangentHalfVerticalFov;
        const float verticalOffset = normalizedY * tangentHalfVerticalFov;
        return viewDistance * std::sqrt(1.f + horizontalOffset * horizontalOffset + verticalOffset * verticalOffset);
    }

    /// Create a floating-point color texture from deterministic per-pixel test data.
    osg::ref_ptr<osg::Texture2D> makeColorTexture(int width, int height,
        const std::function<osg::Vec4f(int, int)>& value, osg::Texture::FilterMode filter = osg::Texture::LINEAR)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                float* pixel = reinterpret_cast<float*>(image->data(x, y));
                const osg::Vec4f color = value(x, y);
                std::copy(color.ptr(), color.ptr() + 4, pixel);
            }
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setInternalFormat(GL_RGBA16F);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setFilter(osg::Texture::MIN_FILTER, filter);
        texture->setFilter(osg::Texture::MAG_FILTER, filter);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        return texture;
    }

    /// Create a normalized depth texture from deterministic per-pixel test data.
    osg::ref_ptr<osg::Texture2D> makeDepthTexture(int width, int height, const std::function<float(int, int)>& value)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_DEPTH_COMPONENT, GL_FLOAT);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                *reinterpret_cast<float*>(image->data(x, y)) = value(x, y);
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setInternalFormat(GL_DEPTH_COMPONENT32F);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        return texture;
    }

    /// Execute a native pass or focused shared-integrator ray grid and read its simultaneous outputs off-screen.
    /// Compile only the selected density diagnostic while retaining the production shader implementation.
    PassImage renderNativeAtmospherePass(
        std::string_view passName, const NativePassInputs& inputs, PassImage* secondaryOutput = nullptr)
    {
        if (passName == "resolveAtmosphereVolumeHistory" && !inputs.resolveReprojectedVolume)
        {
            PassImage metadata;
            const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs, &metadata);
            if (reprojected.pixels.empty() || metadata.pixels.empty())
                return {};
            NativePassInputs repairInputs = inputs;
            repairInputs.resolveReprojectedVolume = true;
            repairInputs.passInput = [&reprojected](int x, int y) { return reprojected.at(x, y); };
            repairInputs.volumeMetadataInput = [&metadata](int x, int y) { return metadata.at(x, y); };
            repairInputs.volumeMetadataWidth = inputs.width;
            repairInputs.volumeMetadataHeight = inputs.height;
            return renderNativeAtmospherePass(passName, repairInputs, secondaryOutput);
        }
        const int width = inputs.width;
        const int height = inputs.height;
        const bool rayGrid = passName == "integrateAtmosphereRayGrid";
        if (rayGrid)
            passName = "integrateAtmosphereVolume";
        const bool resolvePass
            = passName == "resolveAtmosphereVolumeHistory" || passName == "reprojectAtmosphereVolume";
        const int defaultSceneWidth = resolvePass ? static_cast<int>(width / inputs.volumeResolutionScale) : width;
        const int defaultSceneHeight = resolvePass ? static_cast<int>(height / inputs.volumeResolutionScale) : height;
        const int sceneWidth = inputs.sceneWidth > 0 ? inputs.sceneWidth : defaultSceneWidth;
        const int sceneHeight = inputs.sceneHeight > 0 ? inputs.sceneHeight : defaultSceneHeight;

        VFS::Manager vfs;
        vfs.addArchive(
            std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data"));
        vfs.addArchive(
            std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files"));
        vfs.buildIndex();
        Resource::ImageManager imageManager(&vfs, 0.0);
        Fx::Technique technique(vfs, imageManager, VFS::Path::NormalizedView("shaders/internal_atmosphere.omwfx"),
            "internal_atmosphere", width, height, inputs.useUniformBuffer, true);
        if (!technique.compile())
            return {};
        const auto passIt = std::find_if(technique.getPasses().begin(), technique.getPasses().end(),
            [&](const std::shared_ptr<Fx::Pass>& pass) { return pass->getName() == passName; });
        if (passIt == technique.getPasses().end())
            return {};

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = width;
        traits->height = height;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize())
            return {};

        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        osg::Camera* camera = viewer.getCamera();
        camera->setGraphicsContext(context);
        camera->setViewport(new osg::Viewport(0, 0, width, height));
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setClearMask(GL_COLOR_BUFFER_BIT);
        camera->setClearColor(osg::Vec4f());
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        osg::ref_ptr<osg::Image> capture = new osg::Image;
        capture->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
        capture->setInternalTextureFormat(
            inputs.fullPrecisionCapture || (inputs.renderCloudDensitySlice && inputs.cloudDensitySliceQuantity == 2)
                ? GL_RGBA32F
                : GL_RGBA16F);
        camera->attach(osg::Camera::COLOR_BUFFER0, capture);
        osg::ref_ptr<osg::Image> secondaryCapture;
        if (!(*passIt)->getSecondaryTarget().empty())
        {
            secondaryCapture = new osg::Image;
            secondaryCapture->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
            secondaryCapture->setInternalTextureFormat(GL_RGBA16F);
            camera->attach(osg::Camera::COLOR_BUFFER1, secondaryCapture);
        }

        osg::ref_ptr<osg::Geometry> triangle = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ -1.f, -1.f, 0.f });
        vertices->push_back({ -1.f, 3.f, 0.f });
        vertices->push_back({ 3.f, -1.f, 0.f });
        triangle->setVertexArray(vertices);
        triangle->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3));
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(triangle);
        osg::StateSet* state = geode->getOrCreateStateSet();
        (*passIt)->prepareStateSet(state, technique.getName());
        if (inputs.exponentialFog)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Fog permutation has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                std::string source = shader->getShaderSource();
                const std::string marker = "#define OMW_EXPONENTIAL_FOG ";
                const auto position = source.find(marker);
                if (position == std::string::npos)
                    throw std::runtime_error("Fog permutation cannot locate its production definition");
                source.replace(position + marker.size(), 1, *inputs.exponentialFog ? "1" : "0");
                shader->setShaderSource(source);
            }
        }
        if (inputs.useUncachedLocalLighting)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Local lighting reference has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                const std::string guard = "if (lightRecovery != previousLightRecovery)";
                const auto position = source.find(guard);
                if (position == std::string::npos)
                    throw std::runtime_error("Local lighting reference cannot locate recovery reuse");
                source.replace(position, guard.size(), "if (true)");
                const std::string integral = "vec4 integratedVolume = openmwseIntegrateClouds(";
                const auto integralPosition = source.find(integral);
                if (integralPosition == std::string::npos)
                    throw std::runtime_error("Fragment lighting reference cannot locate cloud transport");
                source.insert(integralPosition,
                    "vec4 referenceLowParameters = openmwseLowCloudParameters();\n"
                    "lowCloudLighting = openmwseLocalVolumeIllumination(omw.atmosphereSolarRadiance, "
                    "omw.atmosphereGroundIrradiance, omw.waterHeight + referenceLowParameters.z "
                    "+ referenceLowParameters.w * 0.5, atmosphere);\n");
                shader->setShaderSource(source);
            }
        }
        if (inputs.cloudHeightReference != CloudHeightReference::Production)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Cloud height reference has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                replaceUniqueShaderText(source, "if (lowDensity > 0.0 && mainDensity > 0.0)",
                    inputs.cloudHeightReference == CloudHeightReference::Unconditional ? "if (lowDensity > 0.0)"
                                                                                       : "if (false)");
                shader->setShaderSource(source);
            }
        }
        if (inputs.useSeparateCloudFogReference)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Separate cloud fog reference has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                replaceUniqueShaderText(source,
                    "openmwseCloudSegmentAtmosphericContribution(radiance * mainFraction, mainContribution,\n"
                    "                    cloudAtmosphericTarget, max(fogVisibility, mainCloudMinimumVisibility) * "
                    "mainPresentationWeight)",
                    "mix(cloudAtmosphericTarget * mainContribution, "
                    "openmwseApplyAerialPerspectiveTowardTargetWithScale(radiance * mainFraction, "
                    "cloudAtmosphericTarget * mainContribution, worldDirection, sampleDistance, "
                    "1.0, mainCloudOpticalDistanceLimit, atmosphere), mainPresentationWeight)");
                replaceUniqueShaderText(source,
                    "openmwseCloudSegmentAtmosphericContribution(lowAlbedo * lowFraction\n"
                    "                    * primaryContribution * (lowPrimaryLight + lowSecondaryLight), "
                    "lowContribution,\n"
                    "                    cloudAtmosphericTarget, fogVisibility)",
                    "openmwseApplyAerialPerspectiveTowardTargetWithScale(lowAlbedo * lowFraction "
                    "* primaryContribution * (lowPrimaryLight + lowSecondaryLight), "
                    "cloudAtmosphericTarget * lowContribution, worldDirection, sampleDistance, 1.0, 1e20, atmosphere)");
                shader->setShaderSource(source);
            }
        }
        if (inputs.useUnculledLowCloudReference)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Unculled cloud reference has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                const auto start = source.find("bool sampleLowLayer =");
                const auto end = source.find(';', start);
                if (start == std::string::npos || end == std::string::npos)
                    throw std::runtime_error("Unculled cloud reference cannot locate the sample rejection predicate");
                // The reference evaluates every original quadrature point through production density and transport.
                source.replace(start, end - start + 1, "bool sampleLowLayer = lowLayerEnabled;");
                shader->setShaderSource(source);
            }
        }
        if (passName == "reprojectAtmosphereVolume" && inputs.renderReprojectionCoordinates)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Atmosphere reprojection test has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                const auto mainPosition = source.rfind("void main()");
                if (mainPosition == std::string::npos)
                    throw std::runtime_error("Atmosphere reprojection test has no fragment entry point");
                source.erase(mainPosition);
                source += R"glsl(
void main()
{
    vec2 pixelCoordinate = floor(gl_FragCoord.xy);
    vec2 blockCoordinate = floor(pixelCoordinate * 0.5);
    vec2 rawUv = (blockCoordinate + 0.5) / atmosphereRawDimensions();
    vec4 metadata = omw_Texture2D(AtmosphereVolumeMetadataRaw, rawUv);
    vec3 worldPosition = omw_GetWorldPosFromUV(omw_TexCoord);
    vec3 rayDirection = normalize(worldPosition - omw.eyePos.xyz);
    vec3 reprojectionPosition = omw.eyePos.xyz
        + rayDirection * decodeAtmosphereDistance(metadata.r);
    float advectionScale = atmosphereMediumAdvectionScale(metadata.a);
    reprojectionPosition.xy += omw.atmosphereCloudAdvectionDeltaWorldUnits * advectionScale;
    vec4 previousViewPosition = omw.prevViewMatrix * vec4(reprojectionPosition, 1.0);
    vec4 previousClipPosition = omw.prevProjectionMatrix * previousViewPosition;
    vec2 previousUv = previousClipPosition.xy / max(previousClipPosition.w, 0.0001) * 0.5 + 0.5;
    vec2 motionPixels = (previousUv - omw_TexCoord) * atmosphereVolumeDimensions();
    omw_FragColor = vec4(motionPixels, advectionScale, 1.0);
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                shader->setShaderSource(source);
            }
        }
        if (rayGrid && !inputs.renderCloudDensitySlice)
        {
            auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (!program)
                throw std::runtime_error("Atmosphere ray-grid test has no production shader");
            for (unsigned index = 0; index < program->getNumShaders(); ++index)
            {
                osg::Shader* shader = program->getShader(index);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                const auto mainPosition = source.rfind("void main()");
                if (mainPosition == std::string::npos)
                    throw std::runtime_error("Atmosphere ray-grid test has no fragment entry point");
                // Focused transport tests exercise the shared integrator at every ray; complete-frame
                // tests below execute the unmodified checkerboard production entry points.
                source.erase(mainPosition);
                source += R"glsl(
uniform vec2 openmwseTestRayGridDimensions;
void main()
{
    vec4 metadata;
    omw_FragColor = integrateAtmosphereAtUv(
        gl_FragCoord.xy / openmwseTestRayGridDimensions,
        OpenMWSEVolumeIllumination(atmosphereLowSolarIrradiance, atmosphereLowDiffuseRadiance),
        atmosphereBelowCloudDiffuse, metadata);
    omw_FragColor1 = metadata;
}
)glsl";
                if (inputs.renderCloudSkyReference)
                {
                    const auto referenceMain = source.rfind("void main()");
                    source.erase(referenceMain);
                    source += R"glsl(
void main()
{
    vec2 uv = gl_FragCoord.xy / openmwseTestRayGridDimensions;
    vec3 direction = normalize(omw_GetWorldPosFromUV(uv) - omw.eyePos.xyz);
    omw_FragColor = vec4(openmwseAtmosphereSkyRadiance(direction, aerialPerspectiveParameters()), 1.0);
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                }
                if (inputs.renderTexturedCloudProjection)
                {
                    source.erase(source.rfind("void main()"));
                    source += R"glsl(
void main()
{
    vec3 point = openmwseTexturedCloudShadowProjection(omw.eyePos.xyz, normalize(omw.eyeVec.xyz));
    omw_FragColor = vec4(point / 16000.0, 1.0);
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                }
                if (inputs.renderTexturedCloudTransmission)
                {
                    source.erase(source.rfind("void main()"));
                    source += R"glsl(
void main()
{
    float transmission = openmwseTexturedCloudTransmittance(omw.eyePos.xyz, normalize(omw.eyeVec.xyz));
    omw_FragColor = vec4(transmission);
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                }
                if (inputs.renderTexturedCloudDome)
                {
                    source.erase(source.rfind("void main()"));
                    source += R"glsl(
void main()
{
    vec3 direction = normalize(omw.eyeVec.xyz);
    vec3 dome = openmwseTexturedCloudDomeSample(direction, openmwseTexturedCloudTransforms[0]);
    omw_FragColor = vec4(dome, openmwseTexturedCloudViewTransmittance(direction));
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                }
                if (inputs.renderAuthoredCloudPass)
                {
                    std::ifstream skyFile(
                        std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders/compatibility/sky.frag");
                    const std::string skySource{ std::istreambuf_iterator<char>(skyFile),
                        std::istreambuf_iterator<char>() };
                    const auto start = skySource.find("void paintClouds");
                    const auto end = skySource.find("void paintMoon", start);
                    if (start == std::string::npos || end == std::string::npos)
                        throw std::runtime_error("Missing production textured cloud pass");
                    std::string clouds = skySource.substr(start, end - start);
                    replaceUniqueShaderText(clouds, "atmosphere.cloudsEnabled", "false");
                    replaceUniqueShaderText(clouds, "isReflection", "false");
                    replaceUniqueShaderText(clouds, "atmosphere.cloudReflectionsEnabled", "true");
                    replaceUniqueShaderText(clouds, "texture2D(diffuseMap,", "omw_Texture2D(omw_SamplerLastShader,");
                    source.erase(source.rfind("void main()"));
                    source += R"glsl(
vec2 diffuseMapUV;
vec4 passColor;
vec4 diffuseColor;
float opacity;
)glsl" + clouds + R"glsl(
void main()
{
    diffuseMapUV = (gl_FragCoord.yx / openmwseTestRayGridDimensions.yx) * 0.5 + vec2(0.2, 0.1);
    passColor = vec4(1.0, 1.0, 1.0, 0.25);
    diffuseColor = vec4(0.7, 0.8, 0.9, 1.0);
    opacity = 0.6;
    vec4 clouds = vec4(0.0);
    paintClouds(clouds);
    omw_FragColor = clouds;
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                }
                if (inputs.renderFogTransport)
                {
                    source.erase(source.rfind("void main()"));
                    source += R"glsl(
void main()
{
    OpenMWSEAerialPerspectiveParameters parameters = aerialPerspectiveParameters();
    float distance = omw.fogFar * 0.5;
    float optical = openmwseFogOpticalDistance(distance, normalize(omw.eyeVec.xyz), parameters);
    omw_FragColor = vec4(optical, openmwseFogVisibility(optical, parameters), 0.0, 1.0);
    omw_FragColor1 = vec4(0.0);
}
)glsl";
                }
                if (inputs.cloudTransportReferenceSubdivisions > 1)
                {
                    const std::string subdivisions = std::to_string(inputs.cloudTransportReferenceSubdivisions);
                    replaceUniqueShaderText(source, "float maximumSteps = float(48 + quality * 16);",
                        "float maximumSteps = float(48 + quality * 16) * " + subdivisions + ".0;");
                    replaceUniqueShaderText(source,
                        "96 * (openmwseSkyCloudTransportSubdivisions + openmwseCondensateTransportSubdivisions) + 8;",
                        "(" + subdivisions + " * (96 * (openmwseSkyCloudTransportSubdivisions + openmwseCondensateTransportSubdivisions) + 8));");
                }
                if (inputs.usePerSampleCloudIntervalReference)
                    source = makePerSampleCloudIntervalReference(std::move(source));
                if (inputs.useLegacyCloudMarchReference)
                    source = makeLegacyCloudMarchReference(std::move(source));
                if (inputs.measureCloudMarchProgress)
                    source = instrumentCloudMarchProgress(std::move(source));
                shader->setShaderSource(source);
            }
            state->addUniform(new osg::Uniform(
                "openmwseTestRayGridDimensions", osg::Vec2f(static_cast<float>(width), static_cast<float>(height))));
        }
        if (inputs.renderCloudDensitySlice)
        {
            osg::Program* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (program == nullptr)
                throw std::runtime_error("Native atmosphere density diagnostic has no shader program");
            for (unsigned int shaderIndex = 0; shaderIndex < program->getNumShaders(); ++shaderIndex)
            {
                osg::Shader* shader = program->getShader(shaderIndex);
                if (shader->getType() != osg::Shader::FRAGMENT)
                    continue;
                std::string source = shader->getShaderSource();
                const std::size_t mainPosition = source.rfind("void main()");
                if (mainPosition == std::string::npos)
                    throw std::runtime_error("Native atmosphere density diagnostic could not replace fragment main");
                source.replace(
                    mainPosition, std::string_view("void main()").size(), "void openmwseProductionAtmosphereMain()");
                source += ShaderTest::cloudModelDensityReference;
                // Specialize the requested diagnostic so unused dense-reference marches do not burden compilation.
                source += "const int openmwseTestCloudDensitySliceQuantity = "
                    + std::to_string(inputs.cloudDensitySliceQuantity) + ";\n";
                source += R"glsl(
uniform vec2 openmwseTestCloudDensitySliceExtent;
uniform int openmwseTestCloudDensitySliceAxis;

void main()
{
    if (openmwseTestCloudDensitySliceQuantity == 10)
    {
        OpenMWSEAerialPerspectiveParameters parameters = aerialPerspectiveParameters();
        vec3 worldDirection = normalize(omw.sunPos.xyz);
        float fogOpticalDistance
            = openmwseFogOpticalDistance(openmwseTestCloudDensitySliceExtent.x, worldDirection, parameters);
        float visibility = openmwseFogVisibility(fogOpticalDistance, parameters);
        vec3 inScattering = openmwseAtmosphereScatteredLight(worldDirection,
            clamp(0.224 * fogOpticalDistance, 0.0, 1.0), visibility, parameters.fogColor, parameters);
        omw_FragColor = vec4(inScattering, visibility);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 9)
    {
        OpenMWSEVolumeIllumination illumination = openmwseCloudIllumination(
            omw.atmosphereSolarRadiance, omw.atmosphereSkyIrradiance, aerialPerspectiveParameters());
        omw_FragColor = vec4(openmwseTestCloudDensitySliceAxis == 0
            ? illumination.solarIrradiance : illumination.diffuseRadiance, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 8)
    {
        float size = omw.resolution.x;
        vec3 coordinates = vec3(gl_FragCoord.x, mod(floor(gl_FragCoord.y), size) + 0.5,
            floor(gl_FragCoord.y / size) + 0.5) / size;
        omw_FragColor = openmwseTestCloudDensitySliceAxis == 0
            ? openmwseCloudNoiseSample(coordinates, openmwseTestCloudDensitySliceExtent.x)
            : openmwseCloudDetailNoiseSample(coordinates, openmwseTestCloudDensitySliceExtent.x);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 3)
    {
        OpenMWSEAerialPerspectiveParameters parameters = aerialPerspectiveParameters();
        parameters.exponentialFog = float(openmwseTestCloudDensitySliceAxis);
        vec3 radiance = openmwseApplyAerialPerspective(vec3(1.0), normalize(omw.sunPos.xyz),
            openmwseTestCloudDensitySliceExtent.x, parameters);
        omw_FragColor = vec4(radiance, 1.0);
        return;
    }
    vec2 sliceCoordinates = gl_FragCoord.xy / omw.resolution.xy - vec2(0.5);
    vec3 samplePosition;
    if (openmwseTestCloudDensitySliceAxis == 0)
        samplePosition = omw.eyePos.xyz + vec3(sliceCoordinates * openmwseTestCloudDensitySliceExtent, 0.0);
    else if (openmwseTestCloudDensitySliceAxis == 1)
        samplePosition = omw.eyePos.xyz + vec3(sliceCoordinates.x * openmwseTestCloudDensitySliceExtent.x, 0.0,
            sliceCoordinates.y * openmwseTestCloudDensitySliceExtent.y);
    else
    {
        vec3 planetCenter = openmwseCloudVirtualPlanetCenter();
        vec2 horizontalPosition = omw.eyePos.xy + sliceCoordinates * openmwseTestCloudDensitySliceExtent;
        float shellRadius = openmwseAtmospherePlanetRadiusWorldUnits + omw.eyePos.z - openmwseCloudWaterHeight();
        vec2 horizontalOffset = horizontalPosition - planetCenter.xy;
        float physicalHeight = planetCenter.z
            + sqrt(max(shellRadius * shellRadius - dot(horizontalOffset, horizontalOffset), 0.0));
        samplePosition = vec3(horizontalPosition, physicalHeight);
    }
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer incomingLayer = openmwseIncomingCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    if (openmwseTestCloudDensitySliceQuantity == 40)
    {
        float boundary = 15000.0 * 69.99125109 * mainLayer.model.physicalScale;
        float before = openmwseCloudDensity(samplePosition, boundary - 1.0, 0.0, mainLayer);
        float after = openmwseCloudDensity(samplePosition, boundary + 1.0, 0.0, mainLayer);
        omw_FragColor = vec4(before, after, abs(before - after), 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 27)
    {
        float opticalDepth = openmwseCloudShadowColumnOpticalDepth(samplePosition,
            normalize(omw.sunPos.xyz), mainLayer.parameters, omw.waterHeight,
            mainLayer.fieldOffsetWorldUnits, mainLayer.model, openmwseCloudWeatherTexture);
        omw_FragColor = vec4(exp(-opticalDepth));
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 26)
    {
        vec3 p = vec3((floor(gl_FragCoord.x) - 32.0) * 125.0, 4000.0,
            (floor(gl_FragCoord.y) - 32.0) * 125.0);
        vec3 weather = cloudModelSampleWeather(p, 0.5, mainLayer.model);
        omw_FragColor = vec4(cloudModelSampleDensity(p, 0.5, mainLayer.model, weather, 0.0, true),
            cloudModelSampleDensity(p, 0.5, mainLayer.model, weather, 0.0, false), weather.r, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 24)
    {
        float distance = openmwseTestCloudDensitySliceExtent.x;
        omw_FragColor = vec4(openmwseCloudMarchStepLength(distance, mainLayer.model.physicalScale,
            omw.atmosphereQuality) / distance);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 23 || openmwseTestCloudDensitySliceQuantity == 36)
    {
        // Evaluate every position in the nonlinear light lattice, including empty samples.
        // The optimized march must retain the same density mip when it skips zero extinction.
        OpenMWSECloudLayer shadowLayer = openmwseTestCloudDensitySliceQuantity == 23 ? mainLayer : lowLayer;
        int quality = openmwseTestCloudDensitySliceQuantity == 23 ? 3 : omw.atmosphereQuality;
        int count = quality >= 2 ? 5 : quality >= 1 ? 4 : 3;
        bool skippedSample = false;
        bool resumedSampling = false;
        vec3 direction = normalize(omw.sunPos.xyz);
        float stepLength = 3000.0 * shadowLayer.model.physicalScale * 69.99125109;
        float viewDistance = length(samplePosition - omw.eyePos.xyz);
        vec3 referenceDepth = vec3(0.0);
        for (int s = 0; s < 5; ++s)
        {
            if (s >= count)
                break;
            float t0 = float(s) / float(count);
            float t1 = float(s + 1) / float(count);
            t0 *= t0;
            t1 *= t1;
            float delta = t1 - t0;
            vec3 p = samplePosition + direction * (stepLength * (t0 + delta * 0.5));
            float density = openmwseCloudDensity(p, viewDistance, 0.5 * float(s + 1), shadowLayer);
            bool emptySample = density <= 0.0;
            resumedSampling = resumedSampling || (!emptySample && skippedSample);
            skippedSample = skippedSample || emptySample;
            referenceDepth += clamp(density * shadowLayer.model.extinctionPerMeter, 0.0, 1.0)
                * (stepLength * delta / 69.99125109);
        }
        vec3 reference = exp(-referenceDepth);
        vec3 actual = exp(-openmwseCloudSunOpticalDepth(samplePosition, direction, quality, shadowLayer));
        vec3 error = abs(actual - reference);
        omw_FragColor = vec4(actual.r, reference.r, max(error.r, max(error.g, error.b)) * 1000.0, resumedSampling ? 1.0 : 0.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 22)
    {
        omw_FragColor = vec4(openmwseCloudNoiseLod(openmwseTestCloudDensitySliceExtent.x,
            float(openmwseTestCloudDensitySliceAxis) * 0.5, mainLayer));
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity >= 19 && openmwseTestCloudDensitySliceQuantity <= 21)
    {
        float lod = float(openmwseTestCloudDensitySliceQuantity - 19);
        vec3 fieldPosition = openmwseCloudFieldPosition(samplePosition, mainLayer);
        float h = (fieldPosition.z - mainLayer.parameters.z) / mainLayer.parameters.w;
        vec3 p = (fieldPosition.xzy
            + vec3(mainLayer.fieldOffsetWorldUnits.x, 0.0, mainLayer.fieldOffsetWorldUnits.y)) / 69.99125109;
        vec4 sourceParameters[14];
        for (int field = 0; field < 14; ++field)
            sourceParameters[field] = openmwseCloudModelParameter(0, field);
        float animation = dot(omw.atmosphereCloudAdvectionOffsetWorldUnits, normalize(vec2(0.78, 0.36)))
            / (30.0 * 69.99125109);
        vec2 coverageDirection = vec2(cos(sourceParameters[13].x), sin(sourceParameters[13].x));
        vec2 weatherPosition = p.xz + sourceParameters[12].w * coverageDirection * animation;
        weatherPosition += h * coverageDirection * sourceParameters[4].x;
        vec3 weather = openmwseCloudWeatherSample(weatherPosition * sourceParameters[4].y).rgb;
        vec3 amounts = vec3(sourceParameters[4].z, sourceParameters[5].x, sourceParameters[5].z);
        vec3 minima = vec3(sourceParameters[4].w, sourceParameters[5].y, sourceParameters[5].w);
        weather = minima + clamp(weather * amounts, 0.0, 1.0) * (vec3(1.0) - minima);
        float reference = referenceCloudDensity(p, h, weather, lod, sourceParameters, animation);
        float actual = openmwseCloudDensity(samplePosition, 0.0, lod, mainLayer);
        omw_FragColor = vec4(actual, reference, abs(actual - reference) * 1000.0, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 18)
    {
        vec3 first = cloudModelShapeCoordinates(openmwseCloudModelPosition(samplePosition, mainLayer), 0.3, mainLayer.model);
        vec3 second = cloudModelShapeCoordinates(openmwseCloudModelPosition(samplePosition + vec3(1000.0, 0.0, 0.0), mainLayer), 0.3, mainLayer.model);
        omw_FragColor = vec4(second - first, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 17)
    {
        vec2 base = cloudModelWeatherCoordinates(vec3(0.0), 0.0, mainLayer.model);
        vec2 top = cloudModelWeatherCoordinates(vec3(0.0), 1.0, mainLayer.model);
        vec3 shapeBase = cloudModelShapeCoordinates(vec3(0.0), 0.0, mainLayer.model);
        vec3 shapeTop = cloudModelShapeCoordinates(vec3(0.0), 1.0, mainLayer.model);
        omw_FragColor = vec4(length(top - base), length(shapeTop.xz - shapeBase.xz),
            mainLayer.model.weatherScale, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 16)
    {
        float outgoingDensity = openmwseCloudDensity(samplePosition, 0.0, 0.0, mainLayer);
        float incomingDensity = openmwseCloudDensity(samplePosition, 0.0, 0.0, incomingLayer);
        omw_FragColor = vec4(outgoingDensity, incomingDensity,
            outgoingDensity + incomingDensity, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 15)
    {
        omw_FragColor = vec4(mainLayer.model.totalNoiseScale, incomingLayer.model.totalNoiseScale,
            mainLayer.parameters.y, incomingLayer.parameters.y);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 14)
    {
        vec2 localLayer = openmwseCloudLocalLayer(samplePosition, mainLayer);
        omw_FragColor = vec4(localLayer / 100000.0, 0.0, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 13)
    {
        omw_FragColor = vec4(openmwseMainCloudPresentationWeight(
            openmwseTestCloudDensitySliceExtent.x));
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 12)
    {
        vec3 mainWeather = cloudModelSampleWeather(openmwseCloudModelPosition(samplePosition, mainLayer), 0.3, mainLayer.model);
        vec3 lowWeather = cloudModelSampleWeather(openmwseCloudModelPosition(samplePosition, lowLayer), 0.3, lowLayer.model);
        omw_FragColor = vec4(cloudModelSampleAnvilDeformation(0.3, mainWeather, mainLayer.model),
            cloudModelSampleAnvilDeformation(0.3, lowWeather, lowLayer.model), mainWeather.g, lowWeather.g);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 11)
    {
        vec3 positionMeters = openmwseCloudModelPosition(samplePosition, mainLayer);
        float height = clamp(openmwseCloudHeightFraction(samplePosition.z,
            openmwseCloudNominalBaseHeight(samplePosition, mainLayer), mainLayer.parameters.w), 0.0, 1.0);
        vec3 weather = cloudModelSampleWeather(positionMeters, height, mainLayer.model);
        vec2 coordinates = cloudModelWeatherCoordinates(positionMeters, height, mainLayer.model);
        omw_FragColor = vec4(openmwseCloudWeatherSample(coordinates).r, weather.r, weather.g, weather.b);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 7)
    {
        vec2 bounds = openmwseCloudDensityHeightBounds(mainLayer);
        omw_FragColor = vec4(bounds / 100000.0, 0.0, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 25)
    {
        omw_FragColor = vec4(aerialPerspectiveParameters().weatherCloudTopHeight / 100000.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 29)
    {
        float density = openmwseCloudDensity(samplePosition, 0.0, 0.0, lowLayer);
        float base = openmwseCloudNominalBaseHeight(samplePosition, lowLayer);
        omw_FragColor = vec4(density, base / 10000.0, openmwseCloudOrographicDensityScale(samplePosition, lowLayer), 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 33)
    {
        vec3 direction = normalize(omw_GetWorldPosFromUV(omw_TexCoord) - omw.eyePos.xyz);
        vec2 shell = openmwseCloudDensityHeightBounds(lowLayer);
        vec2 interval = openmwseCloudLayerIntersection(omw.eyePos.xyz, direction, shell.x, shell.y);
        interval.y = min(interval.y, omw.far);
        if (interval.y <= interval.x)
        {
            omw_FragColor = vec4(0.0);
            return;
        }
        vec2 bounds = openmwseLowCloudRayDensityHeightBounds(omw.eyePos.xyz, direction, interval, lowLayer);
        float rejectedDensity = 0.0;
        float maximumDensity = 0.0;
        float rejectedSamples = 0.0;
        for (int i = 0; i < 256; ++i)
        {
            float distance = mix(interval.x, interval.y, (float(i) + 0.5) / 256.0);
            vec3 position = omw.eyePos.xyz + direction * distance;
            float density = openmwseCloudDensity(position, distance, 0.0, lowLayer);
            maximumDensity = max(maximumDensity, density);
            if (position.z < bounds.x || position.z > bounds.y)
            {
                rejectedDensity = max(rejectedDensity, density);
                rejectedSamples += 1.0;
            }
        }
        omw_FragColor = vec4(rejectedDensity, maximumDensity, rejectedSamples / 256.0, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 28 || openmwseTestCloudDensitySliceQuantity == 34)
    {
        vec3 direction = normalize(omw_GetWorldPosFromUV(omw_TexCoord) - omw.eyePos.xyz);
        bool mainReference = openmwseTestCloudDensitySliceQuantity == 34;
        OpenMWSECloudLayer referenceLayer = mainReference ? mainLayer : lowLayer;
        vec2 bounds = openmwseCloudDensityHeightBounds(referenceLayer);
        vec2 interval = mainReference
            ? openmwseMainCloudLayerIntersection(omw.eyePos.xyz, direction, bounds.x, bounds.y)
            : openmwseCloudLayerIntersection(omw.eyePos.xyz, direction, bounds.x, bounds.y);
        if (mainReference)
            interval.y = min(interval.y, openmwseCloudMaximumRayDistance);
        interval.y = min(interval.y, interval.x + openmwseCloudLayerMaximumMarchingDistance(referenceLayer));
        int sampleCount = openmwseTestCloudDensitySliceAxis == 0 ? 4096 : 8192;
        float stepLength = max(interval.y - interval.x, 0.0) / float(sampleCount);
        vec3 opticalDepth = vec3(0.0);
        for (int i = 0; i < 8192; ++i)
        {
            if (i >= sampleCount)
                break;
            float distance = interval.x + (float(i) + 0.5) * stepLength;
            float density = openmwseCloudDensity(omw.eyePos.xyz + direction * distance, distance, 0.0, referenceLayer);
            if (mainReference)
                density *= openmwseMainCloudPresentationWeight(distance);
            opticalDepth += density * referenceLayer.model.extinctionPerMeter
                * (stepLength / openmwseAtmosphereWorldUnitsPerMeter);
        }
        vec3 transmittance = exp(-opticalDepth);
        omw_FragColor = vec4(transmittance, dot(transmittance, vec3(1.0 / 3.0)));
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 6)
    {
        omw_FragColor = vec4(openmwseCloudCurlNoiseSample(openmwseTestCloudDensitySliceExtent), 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 5)
    {
        omw_FragColor = vec4(cloudModelShapeCoordinates(vec3(0.0),
            openmwseTestCloudDensitySliceExtent.x, mainLayer.model), 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 4)
    {
        float height = openmwseTestCloudDensitySliceExtent.x;
        vec3 weather = vec3(1.0, mainLayer.model.typeMinimum, 0.0);
        omw_FragColor = vec4(max(cloudModelShapeDensity(vec4(1.0), height, weather, mainLayer.model), 0.0));
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 2)
    {
        vec2 bounds = openmwseCloudLayerHeightBounds(mainLayer);
        vec2 interval = openmwseMainCloudLayerIntersection(samplePosition, normalize(omw.sunPos.xyz),
            bounds.x, bounds.y);
        omw_FragColor = vec4(openmwseCloudVirtualAltitude(samplePosition), interval, 1.0);
        return;
    }
    float density = openmwseCloudDensity(samplePosition, 0.0, 0.0, mainLayer);
    vec3 diagnosticValue = vec3(density);
    if (openmwseTestCloudDensitySliceQuantity == 1 && density > 0.01)
    {
        OpenMWSECloudSolarTransmittance sunTransmittance = openmwseCloudSunTransmittance(samplePosition, normalize(omw.sunPos.xyz),
            omw.atmosphereQuality, mainLayer, incomingLayer, lowLayer);
        diagnosticValue = vec3(dot(sunTransmittance.primary, vec3(1.0 / 3.0)),
            dot(sunTransmittance.secondary, vec3(1.0 / 3.0)), 0.0);
    }
    omw_FragColor = vec4(diagnosticValue, density);
}
)glsl";
                shader->setShaderSource(source);
            }
            state->addUniform(new osg::Uniform("openmwseTestCloudDensitySliceExtent", inputs.cloudDensitySliceExtent));
            state->addUniform(new osg::Uniform("openmwseTestCloudDensitySliceAxis", inputs.cloudDensitySliceAxis));
        }
        if (inputs.reverseZ)
        {
            osg::Program* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
            if (program == nullptr)
                throw std::runtime_error("Native atmosphere pass has no shader program");

            constexpr std::string_view standardDepthDefine = "#define OMW_REVERSE_Z 0";
            constexpr std::string_view reversedDepthDefine = "#define OMW_REVERSE_Z 1";
            int replacedShaderCount = 0;
            for (unsigned int shaderIndex = 0; shaderIndex < program->getNumShaders(); ++shaderIndex)
            {
                osg::Shader* shader = program->getShader(shaderIndex);
                std::string source = shader->getShaderSource();
                const std::size_t definePosition = source.find(standardDepthDefine);
                if (definePosition == std::string::npos)
                    continue;
                source.replace(definePosition, standardDepthDefine.size(), reversedDepthDefine);
                shader->setShaderSource(source);
                ++replacedShaderCount;
            }
            if (replacedShaderCount != 2)
                throw std::runtime_error("Native atmosphere reverse-Z permutation was not applied to both shaders");
        }
        state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        const osg::ref_ptr<osg::Texture2D> lastShader = makeColorTexture(sceneWidth, sceneHeight, inputs.lastShader);
        const osg::ref_ptr<osg::Texture2D> depth = makeDepthTexture(sceneWidth, sceneHeight, inputs.depth);
        const osg::ref_ptr<osg::Texture2D> previousDepth
            = makeDepthTexture(sceneWidth, sceneHeight, inputs.previousDepth);
        const osg::ref_ptr<osg::Texture2D> history
            = makeColorTexture(width, height, inputs.atmosphereHistory, osg::Texture::LINEAR);
        const std::function<osg::Vec4f(int, int)> defaultMetadata = [](int, int) {
            const float encodedFar = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(encodedFar, 1.f, encodedFar, 0.f);
        };
        const auto& metadataHistoryFunction
            = inputs.atmosphereMetadataHistory ? inputs.atmosphereMetadataHistory : defaultMetadata;
        const osg::ref_ptr<osg::Texture2D> metadataHistory
            = makeColorTexture(width, height, metadataHistoryFunction, osg::Texture::NEAREST);
        state->setTextureAttribute(0, lastShader);
        state->setTextureAttribute(1, lastShader);
        state->setTextureAttribute(2, depth);
        state->setTextureAttribute(5, lastShader);
        state->setTextureAttribute(6, previousDepth);
        state->setTextureAttribute(7, history);
        state->setTextureAttribute(8, metadataHistory);
        state->addUniform(new osg::Uniform("omw_SamplerLastShader", 0));
        state->addUniform(new osg::Uniform("omw_SamplerLastPass", 1));
        state->addUniform(new osg::Uniform("omw_SamplerDepth", 2));
        state->addUniform(new osg::Uniform("omw_SamplerDistortion", 5));
        state->addUniform(new osg::Uniform("omw_SamplerPreviousDepth", 6));
        state->addUniform(new osg::Uniform("omw_SamplerAtmosphereHistory", 7));
        state->addUniform(new osg::Uniform("omw_SamplerAtmosphereMetadataHistory", 8));
        state->addUniform(new osg::Uniform("omw_AtmosphereHistoryValid", inputs.historyValid));
        int inputUnit = 9;
        for (const osg::ref_ptr<osg::Texture>& texture : technique.getTextures())
        {
            state->setTextureAttribute(inputUnit, texture);
            state->addUniform(new osg::Uniform(texture->getName().c_str(), inputUnit++));
        }
        for (const std::string& inputName : (*passIt)->getRenderTargets())
        {
            if (inputName.empty())
                continue;
            const std::function<osg::Vec4f(int, int)>* input = &inputs.passInput;
            int inputWidth = inputs.passWidth > 0 ? inputs.passWidth : width;
            int inputHeight = inputs.passHeight > 0 ? inputs.passHeight : height;
            if (inputName == "AtmosphereVolumeRaw")
            {
                inputWidth = inputs.rawVolumeWidth > 0 ? inputs.rawVolumeWidth : width;
                inputHeight = inputs.rawVolumeHeight > 0 ? inputs.rawVolumeHeight : height;
            }
            else if (inputName == "AtmosphereVolumeHistory" && inputs.volumeHistoryInput)
            {
                input = &inputs.volumeHistoryInput;
                inputWidth = inputs.volumeHistoryWidth > 0 ? inputs.volumeHistoryWidth : width;
                inputHeight = inputs.volumeHistoryHeight > 0 ? inputs.volumeHistoryHeight : height;
            }
            else if ((inputName == "AtmosphereVolumeMetadataHistory" || inputName == "AtmosphereVolumeMetadataRaw"
                         || inputName == "AtmosphereVolumeMetadataReprojected")
                && inputs.volumeMetadataInput)
            {
                input = &inputs.volumeMetadataInput;
                inputWidth = inputs.volumeMetadataWidth > 0 ? inputs.volumeMetadataWidth : width;
                inputHeight = inputs.volumeMetadataHeight > 0 ? inputs.volumeMetadataHeight : height;
            }
            else if (inputName == "AtmosphereVolumeMetadataHistory" || inputName == "AtmosphereVolumeMetadataRaw"
                || inputName == "AtmosphereVolumeMetadataReprojected")
                input = &defaultMetadata;
            else if (inputName == "AtmosphereShafts" && inputs.shaftInput)
            {
                input = &inputs.shaftInput;
                inputWidth = inputs.shaftWidth > 0 ? inputs.shaftWidth : width;
                inputHeight = inputs.shaftHeight > 0 ? inputs.shaftHeight : height;
            }
            std::function<osg::Vec4f(int, int)> checkerboardInput;
            if (resolvePass && inputs.rawVolumeWidth == 0
                && (inputName == "AtmosphereVolumeRaw" || inputName == "AtmosphereVolumeMetadataRaw"))
            {
                // Synthetic reconstruction fixtures describe a dense ray grid. Pack only the current
                // phase into the quarter-size input, just as the production trace pass does.
                const auto denseInput = *input;
                checkerboardInput = [denseInput, frame = inputs.frameNumber](int x, int y) {
                    const int phase = (frame + (x + y) % 2) % 4;
                    return denseInput(x * 2 + phase % 2, y * 2 + phase / 2);
                };
                input = &checkerboardInput;
                inputWidth = std::max(1, width / 2);
                inputHeight = std::max(1, height / 2);
            }
            state->setTextureAttribute(
                inputUnit, makeColorTexture(inputWidth, inputHeight, *input, osg::Texture::NEAREST));
            state->addUniform(new osg::Uniform(inputName.c_str(), inputUnit++));
        }
        state->addUniform(new osg::Uniform("omw_PointLightsCount", inputs.pointLightCount));
        osg::ref_ptr<osg::Uniform> pointLights = new osg::Uniform(
            osg::Uniform::FLOAT_VEC4, "omw_PointLights", SceneUtil::PPLightBuffer::sMaxPPLightsArraySize);
        for (std::size_t index = 0; index < inputs.pointLights.size(); ++index)
            pointLights->setElement(static_cast<unsigned int>(index), inputs.pointLights[index]);
        state->addUniform(pointLights);

        Fx::StateUpdater updater(inputs.useUniformBuffer);
        const osg::Vec3f eyePosition = inputs.eyePosition;
        const osg::Vec3f viewDirection = inputs.viewDirection;
        const double aspectRatio = static_cast<double>(sceneWidth) / static_cast<double>(sceneHeight);
        osg::Matrixf projection
            = osg::Matrixf::perspective(inputs.verticalFov, aspectRatio, inputs.nearPlane, inputs.farPlane);
        osg::Matrixf previousProjection
            = osg::Matrixf::perspective(inputs.previousVerticalFov, aspectRatio, inputs.nearPlane, inputs.farPlane);
        if (inputs.reverseZ)
        {
            projection = osg::Matrixf(SceneUtil::getReversedZProjectionMatrixAsPerspective(
                inputs.verticalFov, aspectRatio, inputs.nearPlane, inputs.farPlane));
            previousProjection = osg::Matrixf(SceneUtil::getReversedZProjectionMatrixAsPerspective(
                inputs.previousVerticalFov, aspectRatio, inputs.nearPlane, inputs.farPlane));
        }
        const osg::Matrixf view
            = osg::Matrixf::lookAt(eyePosition, eyePosition + viewDirection, osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Vec3f previousEyePosition = eyePosition + inputs.previousEyeOffset;
        const osg::Vec3f previousViewDirection = inputs.previousViewDirection.value_or(viewDirection);
        const osg::Matrixf previousView = osg::Matrixf::lookAt(
            previousEyePosition, previousEyePosition + previousViewDirection, osg::Vec3f(0.f, 0.f, 1.f));
        updater.setProjectionMatrix(projection);
        updater.setPrevProjectionMatrix(previousProjection);
        updater.setViewMatrix(view);
        updater.setPrevViewMatrix(previousView);
        updater.setEyePos(eyePosition);
        updater.setEyeVec(viewDirection);
        updater.setResolution({ static_cast<float>(sceneWidth), static_cast<float>(sceneHeight) });
        updater.setNearFar(inputs.nearPlane, inputs.farPlane);
        updater.setFogRange(inputs.fogNear, inputs.fogFar);
        updater.setFogColor(inputs.fogColor);
        updater.setSkyColor(inputs.skyColor);
        updater.setSunColor(inputs.sunColor);
        updater.setSunPos(inputs.sunDirection, false);
        updater.setSunVis(inputs.sunVisibility);
        updater.setWaterHeight(0.f);
        updater.setSimulationTime(inputs.simulationTime);
        updater.setDeltaSimulationTime(inputs.deltaSimulationTime);
        updater.setFrameNumber(inputs.frameNumber);
        updater.setWindSpeed(inputs.windSpeed);
        SceneUtil::AtmosphereTerrainGrid terrain;
        terrain.update({ inputs.eyePosition.x(), inputs.eyePosition.y() }, [&](const osg::Vec2f& position) {
            if (inputs.terrainHeight)
                return inputs.terrainHeight(position);
            return inputs.mediumReferenceParameters.x()
                + (position - osg::Vec2f(inputs.orographicParameters.x(), inputs.orographicParameters.y()))
                * osg::Vec2f(inputs.orographicParameters.z(), inputs.orographicParameters.w());
        });
        updater.setAtmosphereTerrain(terrain);
        updater.setCloudLayers(
            SceneUtil::packCloudLayers(inputs.cloudModel, inputs.incomingCloudModel, inputs.environmentalCloudModel));
        updater.setAtmosphereScatteringEnabled(inputs.scatteringEnabled);
        updater.setVolumetricCloudsEnabled(inputs.volumetricCloudsEnabled);
        updater.setAtmosphereAppearance(inputs.atmosphereAppearance);
        updater.setSunlightEnabled(inputs.sunlightEnabled);
        updater.setGodraysEnabled(inputs.godraysEnabled);
        updater.setSunGlareEnabled(inputs.sunGlareEnabled);
        updater.setSunGlareAllowed(inputs.sunGlareAllowed);
        updater.setCloudSunOcclusion(inputs.cloudSunOcclusion);
        updater.setTexturedClouds(inputs.texturedClouds);
        updater.setSunGlareTexture(inputs.sunGlareTexture);
        updater.setAtmosphere(inputs.inScattering, inputs.outScattering, inputs.cloudParameters,
            inputs.incomingCloudParameters, inputs.cloudScenarioParameters, inputs.lowCloudParameters,
            inputs.lowCloudEnvironmentInfluence, inputs.mistParameters, inputs.fogVolumeParameters,
            inputs.mediumReferenceParameters, inputs.orographicParameters, inputs.sunDiscColor, inputs.solarRadiance,
            inputs.skyIrradiance, inputs.groundIrradiance, inputs.cloudAdvectionOffsetWorldUnits,
            inputs.cloudAdvectionDeltaWorldUnits, inputs.historyLightingWeight, inputs.weatherWeight, inputs.quality,
            inputs.volumeResolutionScale, inputs.atmosphereEnabled);
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        scene->addChild(geode);
        osg::StateSet* updaterState = inputs.bindUniformBufferOnAncestor ? scene->getOrCreateStateSet() : state;
        updater.setDefaults(updaterState);
        updater.apply(updaterState, nullptr);
        if (inputs.useGeneralCloudShader)
            updaterState->setDefine("OPENMWSE_CLOUD_STEADY", "0", osg::StateAttribute::ON);

        viewer.setSceneData(scene);
        viewer.realize();
        context->makeCurrent();
        auto* nativeProgram = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
        if (!nativeProgram)
            throw std::runtime_error("Native atmosphere test has no shader program");
        nativeProgram->compileGLObjects(*context->getState());
        const osg::Program::PerContextProgram* compiledProgram = nativeProgram->getPCP(*context->getState());
        if (!compiledProgram->isLinked())
        {
            std::string infoLog;
            compiledProgram->getInfoLog(infoLog);
            throw std::runtime_error("Native atmosphere shader failed to link: " + infoLog);
        }
        viewer.frame();
        context->makeCurrent();
        glFinish();

        const auto readAttachment = [width, height](const osg::Image& image) {
            PassImage output{ width, height, {} };
            output.pixels.reserve(width * height);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const float* pixel = reinterpret_cast<const float*>(image.data(x, y));
                    output.pixels.emplace_back(pixel[0], pixel[1], pixel[2], pixel[3]);
                }
            return output;
        };
        PassImage result = readAttachment(*capture);
        if (secondaryOutput && secondaryCapture)
            *secondaryOutput = readAttachment(*secondaryCapture);
        context->close();
        return result;
    }

    /// Read metadata emitted by the actual volume integral, with identical density and visibility samples.
    PassImage renderNativeAtmosphereMetadata(const NativePassInputs& inputs)
    {
        PassImage metadata;
        renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
        return metadata;
    }

    /// Integrate four spatial subpixels and independent temporal samples over each reference pixel's footprint.
    PassImage renderCloudFootprintReference(NativePassInputs inputs, int frameCount)
    {
        const int width = inputs.width;
        const int height = inputs.height;
        PassImage reference{ width, height, std::vector<osg::Vec4f>(width * height) };
        inputs.sceneWidth = (inputs.sceneWidth > 0 ? inputs.sceneWidth : width) * 2;
        inputs.sceneHeight = (inputs.sceneHeight > 0 ? inputs.sceneHeight : height) * 2;
        inputs.width *= 2;
        inputs.height *= 2;
        inputs.previousEyeOffset = {};
        inputs.historyValid = true;
        for (int frame = 0; frame < frameCount; ++frame)
        {
            inputs.frameNumber = frame;
            const PassImage sample = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    for (int offsetY = 0; offsetY < 2; ++offsetY)
                        for (int offsetX = 0; offsetX < 2; ++offsetX)
                            reference.pixels[y * width + x]
                                += sample.at(x * 2 + offsetX, y * 2 + offsetY) / float(frameCount * 4);
        }
        return reference;
    }

    /// Own every native pass output produced for one production-scaled atmosphere frame.
    struct ProductionAtmosphereFrame
    {
        PassImage mRawVolume;
        PassImage mRawMetadata;
        PassImage mMetadata;
        PassImage mResolvedVolume;
        PassImage mShafts;
        PassImage mComposite;
    };

    /// Execute the production volume integration and history passes without unrelated shafts or compositing.
    ProductionAtmosphereFrame renderProductionAtmosphereVolumeFrame(
        const NativePassInputs& requestedInputs, const ProductionAtmosphereFrame* previousFrame = nullptr)
    {
        const int sceneWidth = requestedInputs.width;
        const int sceneHeight = requestedInputs.height;
        const float volumeScale = MWRender::getAtmosphereVolumeResolutionScale(requestedInputs.quality);
        const int volumeWidth = std::max(1, static_cast<int>(sceneWidth * volumeScale));
        const int volumeHeight = std::max(1, static_cast<int>(sceneHeight * volumeScale));
        const int rawWidth = std::max(1, static_cast<int>(sceneWidth * volumeScale * 0.5f));
        const int rawHeight = std::max(1, static_cast<int>(sceneHeight * volumeScale * 0.5f));

        NativePassInputs volumeInputs = requestedInputs;
        volumeInputs.width = rawWidth;
        volumeInputs.height = rawHeight;
        volumeInputs.sceneWidth = sceneWidth;
        volumeInputs.sceneHeight = sceneHeight;
        volumeInputs.volumeResolutionScale = volumeScale;
        ProductionAtmosphereFrame frame;
        volumeInputs.historyValid = requestedInputs.historyValid && previousFrame != nullptr;
        frame.mRawVolume = renderNativeAtmospherePass("integrateAtmosphereVolume", volumeInputs, &frame.mRawMetadata);

        NativePassInputs resolveInputs = volumeInputs;
        resolveInputs.width = volumeWidth;
        resolveInputs.height = volumeHeight;
        resolveInputs.rawVolumeWidth = rawWidth;
        resolveInputs.rawVolumeHeight = rawHeight;
        resolveInputs.historyValid = requestedInputs.historyValid && previousFrame != nullptr;
        resolveInputs.passInput = [&frame](int x, int y) { return frame.mRawVolume.at(x, y); };
        resolveInputs.volumeMetadataInput = [&frame](int x, int y) { return frame.mRawMetadata.at(x, y); };
        resolveInputs.volumeMetadataWidth = rawWidth;
        resolveInputs.volumeMetadataHeight = rawHeight;
        if (previousFrame != nullptr)
        {
            resolveInputs.atmosphereHistory
                = [previousFrame](int x, int y) { return previousFrame->mResolvedVolume.at(x, y); };
            resolveInputs.atmosphereMetadataHistory
                = [previousFrame](int x, int y) { return previousFrame->mMetadata.at(x, y); };
        }
        frame.mResolvedVolume
            = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs, &frame.mMetadata);

        return frame;
    }

    /// Execute the complete depth-mask, source-filter, radial-integration, and ray-filter chain at native resolutions.
    PassImage renderProductionShafts(const NativePassInputs& requested)
    {
        const int width = requested.sceneWidth > 0 ? requested.sceneWidth : requested.width;
        const int height = requested.sceneHeight > 0 ? requested.sceneHeight : requested.height;
        NativePassInputs inputs = requested;
        inputs.sceneWidth = width;
        inputs.sceneHeight = height;
        inputs.width = std::max(1, width / 2);
        inputs.height = std::max(1, height / 2);
        const auto mask = renderNativeAtmospherePass("prepareShaftSource", inputs);
        inputs.passInput = [&mask](int x, int y) { return mask.at(x, y); };
        const auto filtered = renderNativeAtmospherePass("filterShaftSource", inputs);
        inputs.passWidth = inputs.width;
        inputs.passHeight = inputs.height;
        inputs.width = width;
        inputs.height = height;
        inputs.passInput = [&filtered](int x, int y) { return filtered.at(x, y); };
        const auto rays = renderNativeAtmospherePass("integrateShafts", inputs);
        inputs.passWidth = width;
        inputs.passHeight = height;
        inputs.passInput = [&rays](int x, int y) { return rays.at(x, y); };
        return renderNativeAtmospherePass("filterShaftRays", inputs);
    }

    /// Execute the complete native atmosphere technique at its production render-target resolutions.
    ProductionAtmosphereFrame renderProductionAtmosphereFrame(
        const NativePassInputs& requestedInputs, const ProductionAtmosphereFrame* previousFrame = nullptr)
    {
        const int sceneWidth = requestedInputs.width;
        const int sceneHeight = requestedInputs.height;
        const float volumeScale = MWRender::getAtmosphereVolumeResolutionScale(requestedInputs.quality);
        const int volumeWidth = std::max(1, static_cast<int>(sceneWidth * volumeScale));
        const int volumeHeight = std::max(1, static_cast<int>(sceneHeight * volumeScale));
        const int shaftWidth = sceneWidth;
        const int shaftHeight = sceneHeight;
        ProductionAtmosphereFrame frame = renderProductionAtmosphereVolumeFrame(requestedInputs, previousFrame);

        NativePassInputs shaftInputs = requestedInputs;
        shaftInputs.width = shaftWidth;
        shaftInputs.height = shaftHeight;
        shaftInputs.sceneWidth = sceneWidth;
        shaftInputs.sceneHeight = sceneHeight;
        shaftInputs.volumeResolutionScale = volumeScale;
        shaftInputs.volumeHistoryInput = [&frame](int x, int y) { return frame.mResolvedVolume.at(x, y); };
        shaftInputs.volumeHistoryWidth = volumeWidth;
        shaftInputs.volumeHistoryHeight = volumeHeight;
        frame.mShafts = renderProductionShafts(shaftInputs);

        NativePassInputs compositeInputs = requestedInputs;
        compositeInputs.sceneWidth = sceneWidth;
        compositeInputs.sceneHeight = sceneHeight;
        compositeInputs.volumeResolutionScale = volumeScale;
        compositeInputs.volumeHistoryInput = [&frame](int x, int y) { return frame.mResolvedVolume.at(x, y); };
        compositeInputs.volumeHistoryWidth = volumeWidth;
        compositeInputs.volumeHistoryHeight = volumeHeight;
        compositeInputs.volumeMetadataInput = [&frame](int x, int y) { return frame.mMetadata.at(x, y); };
        compositeInputs.volumeMetadataWidth = volumeWidth;
        compositeInputs.volumeMetadataHeight = volumeHeight;
        compositeInputs.shaftInput = [&frame](int x, int y) { return frame.mShafts.at(x, y); };
        compositeInputs.shaftWidth = shaftWidth;
        compositeInputs.shaftHeight = shaftHeight;
        frame.mComposite = renderNativeAtmospherePass("composite", compositeInputs);
        return frame;
    }

    /// Create a mipmapped cutout texture with the dark transparent borders found in foliage assets.
    osg::ref_ptr<osg::Texture2D> makeDistantFoliageTexture(osg::Image* productionImage = nullptr)
    {
        osg::ref_ptr<osg::Image> image = productionImage;
        if (!image)
        {
            constexpr int textureSize = 256;
            image = new osg::Image;
            image->allocateImage(textureSize, textureSize, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            for (int y = 0; y < textureSize; ++y)
                for (int x = 0; x < textureSize; ++x)
                {
                    const float normalizedX = (static_cast<float>(x) + 0.5f) / textureSize;
                    const float normalizedY = (static_cast<float>(y) + 0.5f) / textureSize;
                    const float branchCenter = 0.5f + 0.18f * std::sin(normalizedX * 13.f);
                    const float branchDistance = std::abs(normalizedY - branchCenter);
                    const float twigCenter = 0.5f + 0.22f * (normalizedX - 0.5f);
                    const float twigDistance = std::abs(normalizedY - twigCenter);
                    const float opacity = std::clamp(std::max((0.08f - branchDistance) * textureSize * 0.55f,
                                                         (0.025f - twigDistance) * textureSize * 0.7f),
                        0.f, 1.f);
                    unsigned char* pixel = image->data(x, y);
                    pixel[0] = static_cast<unsigned char>(std::round(43.f * opacity));
                    pixel[1] = static_cast<unsigned char>(std::round(61.f * opacity));
                    pixel[2] = static_cast<unsigned char>(std::round(35.f * opacity));
                    pixel[3] = static_cast<unsigned char>(std::round(255.f * opacity));
                }
        }

        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setInternalFormat(GL_RGBA8);
        texture->setResizeNonPowerOfTwoHint(false);
        texture->setUseHardwareMipMapGeneration(!image->isMipmap());
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        return texture;
    }

    /// Load the active Bitter Coast leaf texture from explicitly supplied production data directories.
    osg::ref_ptr<osg::Image> loadExternalFoliageTexture()
    {
        char* value = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&value, &size, "OPENMW_FOLIAGE_TEST_DATA") != 0 || value == nullptr)
            return nullptr;

        std::vector<std::filesystem::path> directories;
        std::string_view remaining(value);
        while (!remaining.empty())
        {
            const std::size_t separator = remaining.find(';');
            const std::string_view entry = remaining.substr(0, separator);
            if (!entry.empty())
                directories.emplace_back(entry);
            if (separator == std::string_view::npos)
                break;
            remaining.remove_prefix(separator + 1);
        }
        std::free(value);

        constexpr std::string_view leafTexture = "textures/tx_bc_leaves_02.dds";
        for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory)
        {
            const std::filesystem::path candidate = *directory / leafTexture;
            if (std::filesystem::is_regular_file(candidate))
                return osgDB::readRefImageFile(candidate.string());
        }
        return nullptr;
    }

    /// Render the production object shader against a minified alpha-tested foliage card.
    PassImage renderDistantFoliageCard(
        bool adjustCoverage, osg::Image* productionImage = nullptr, int atmosphereDiagnosticMode = 0)
    {
        constexpr int width = 192;
        constexpr int height = 96;
        const osg::Vec4f atmosphericBackground(0.78f, 0.71f, 0.66f, 0.f);

        Shader::ShaderManager manager;
        manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = getAtmosphereTestDefines();
        defines["diffuseMap"] = "1";
        for (const std::string_view mapName : { "bumpMap", "darkMap", "decalMap", "detailMap", "diffuseMap",
                 "emissiveMap", "envMap", "glossMap", "normalMap", "specularMap" })
            defines[std::string(mapName) + "UV"] = "0";
        defines["adjustCoverage"] = adjustCoverage ? "1" : "0";
        defines["alphaFunc"] = "516";
        defines["alphaToCoverage"] = "0";
        defines["disableNormals"] = "1";
        defines["skyBlending"] = "1";
        manager.setGlobalDefines(defines);
        const osg::ref_ptr<osg::Program> program = manager.getProgram("compatibility/objects", defines);
        if (!program)
            return {};

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = width;
        traits->height = height;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize())
            return {};

        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        osg::Camera* camera = viewer.getCamera();
        camera->setGraphicsContext(context);
        camera->setViewport(new osg::Viewport(0, 0, width, height));
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setClearMask(GL_COLOR_BUFFER_BIT);
        camera->setClearColor(atmosphericBackground);
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setCullingMode(0);
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        osg::ref_ptr<osg::Image> capture = new osg::Image;
        capture->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
        capture->setInternalTextureFormat(GL_RGBA16F);
        camera->attach(osg::Camera::COLOR_BUFFER, capture);

        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ -1.f, -1.f, -1.f });
        vertices->push_back({ 1.f, -1.f, -1.f });
        vertices->push_back({ 1.f, 1.f, -1.f });
        vertices->push_back({ -1.f, -1.f, -1.f });
        vertices->push_back({ 1.f, 1.f, -1.f });
        vertices->push_back({ -1.f, 1.f, -1.f });
        osg::ref_ptr<osg::Vec2Array> textureCoordinates = new osg::Vec2Array;
        textureCoordinates->push_back({ 0.f, 0.f });
        textureCoordinates->push_back({ 8.f, 0.f });
        textureCoordinates->push_back({ 8.f, 4.f });
        textureCoordinates->push_back({ 0.f, 0.f });
        textureCoordinates->push_back({ 8.f, 4.f });
        textureCoordinates->push_back({ 0.f, 4.f });
        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
        normals->push_back({ 0.f, 0.f, 1.f });
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back({ 1.f, 1.f, 1.f, 1.f });
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(vertices);
        geometry->setTexCoordArray(0, textureCoordinates);
        geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 6));
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geometry);
        osg::StateSet* state = geode->getOrCreateStateSet();
        state->setAttributeAndModes(program, osg::StateAttribute::ON);
        state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        state->setTextureAttributeAndModes(0, makeDistantFoliageTexture(productionImage), osg::StateAttribute::ON);
        state->setTextureAttributeAndModes(1,
            makeColorTexture(1, 1, [atmosphericBackground](int, int) { return atmosphericBackground; }),
            osg::StateAttribute::ON);
        state->addUniform(new osg::Uniform("diffuseMap", 0));
        state->addUniform(new osg::Uniform("sky", 1));
        state->addUniform(new osg::Uniform("texMat0", osg::Matrixf::identity()));
        state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf::identity()));
        state->addUniform(new osg::Uniform("screenRes", osg::Vec2f(width, height)));
        state->addUniform(new osg::Uniform("near", 0.f));
        state->addUniform(new osg::Uniform("far", 2.f));
        state->addUniform(new osg::Uniform("alphaRef", 0.5f));
        state->addUniform(new osg::Uniform("alpha", 1.f));
        state->addUniform(new osg::Uniform("actorFade", 1.f));
        state->addUniform(new osg::Uniform("distortionStrength", 0.f));
        state->addUniform(new osg::Uniform("skyBlendingStart", 0.8f));
        state->addUniform(new osg::Uniform("waterHeight", 0.f));
        state->addUniform(new osg::Uniform("waterEnabled", false));
        state->addUniform(new osg::Uniform("waterSurface", false));
        state->addUniform(new osg::Uniform("isReflection", false));
        state->addUniform(new osg::Uniform("osg_ViewMatrixInverse", osg::Matrixf::identity()));
        state->addUniform(new osg::Uniform("fog.color", atmosphericBackground));
        state->addUniform(new osg::Uniform("fog.underwaterColor", atmosphericBackground));
        state->addUniform(new osg::Uniform("fog.start", 0.1f));
        state->addUniform(new osg::Uniform("fog.underwaterStart", 0.1f));
        state->addUniform(new osg::Uniform("fog.end", 2.f));
        state->addUniform(new osg::Uniform("fog.underwaterEnd", 2.f));
        state->addUniform(new osg::Uniform("fog.depth", -1.f));
        state->addUniform(new osg::Uniform("atmosphere.enabled", false));
        state->addUniform(new osg::Uniform("openmwseAtmosphereDiagnosticMode", atmosphereDiagnosticMode));
        state->addUniform(new osg::Uniform("material.diffuse", osg::Vec4f(0.f, 0.f, 0.f, 1.f)));
        state->addUniform(new osg::Uniform("material.ambient", osg::Vec4f(0.f, 0.f, 0.f, 1.f)));
        state->addUniform(new osg::Uniform("material.specular", osg::Vec4f(0.f, 0.f, 0.f, 1.f)));
        state->addUniform(new osg::Uniform("material.emission", osg::Vec4f(1.f, 1.f, 1.f, 1.f)));
        state->addUniform(new osg::Uniform("material.shininess", 1.f));
        state->addUniform(new osg::Uniform("material.emissiveMult", 1.f));
        state->addUniform(new osg::Uniform("material.specStrength", 0.f));
        state->addUniform(new osg::Uniform("material.vertexColorMode", 0));

        viewer.setSceneData(geode);
        viewer.realize();
        context->makeCurrent();
        program->compileGLObjects(*context->getState());
        const osg::Program::PerContextProgram* compiled = program->getPCP(*context->getState());
        std::string infoLog;
        compiled->getInfoLog(infoLog);
        if (!compiled->isLinked())
            throw std::runtime_error("Distant foliage shader failed to link: " + infoLog);
        viewer.frame();
        context->makeCurrent();
        glFinish();

        PassImage result{ width, height, {} };
        result.pixels.reserve(width * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const float* pixel = reinterpret_cast<const float*>(capture->data(x, y));
                result.pixels.emplace_back(pixel[0], pixel[1], pixel[2], pixel[3]);
            }
        context->close();
        return result;
    }

    /// Compare sky-vertex illumination against the original fragment integral under perspective interpolation.
    PassImage renderReflectionLightingProbe(const NativePassInputs& inputs, bool gpuShader4)
    {
        Shader::ShaderManager manager;
        manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = getAtmosphereTestDefines();
        defines["useGPUShader4"] = gpuShader4 ? "1" : "0";
        manager.setGlobalDefines(defines);
        const osg::ref_ptr<osg::Program> program = manager.getProgram("compatibility/sky", defines);
        if (!program)
            throw std::runtime_error("Reflection lighting probe has no production sky program");
        bool replaced = false;
        for (unsigned index = 0; index < program->getNumShaders(); ++index)
        {
            osg::Shader* shader = program->getShader(index);
            if (shader->getType() != osg::Shader::FRAGMENT)
                continue;
            std::string source = shader->getShaderSource();
            if (source.find("void paintAtmosphere(") == std::string::npos)
                continue;
            const auto begin = source.find("void main()");
            if (begin == std::string::npos)
                throw std::runtime_error("Reflection lighting probe cannot locate the sky fragment entry point");
            const auto body = source.find('{', begin);
            std::size_t end = body + 1;
            int depth = 1;
            for (; end < source.size() && depth != 0; ++end)
                depth += source[end] == '{' ? 1 : source[end] == '}' ? -1 : 0;
            if (depth != 0)
                throw std::runtime_error("Reflection lighting probe found an incomplete fragment entry point");
            source.replace(begin, end - begin, R"glsl(
void main()
{
    OpenMWSEVolumeIllumination reference = openmwseLocalVolumeIllumination(
        atmosphere.solarRadiance, atmosphere.groundIrradiance,
        waterHeight + atmosphere.lowCloudParameters.z + atmosphere.lowCloudParameters.w * 0.5,
        openmwseAerialPerspectiveParameters());
    vec3 solarError = abs(reference.solarIrradiance - skyLowCloudSolarIrradiance);
    vec3 diffuseError = abs(reference.diffuseRadiance - skyLowCloudDiffuseRadiance);
    gl_FragColor = vec4(max(max(solarError.x, solarError.y), solarError.z),
        max(max(diffuseError.x, diffuseError.y), diffuseError.z),
        dot(reference.diffuseRadiance, vec3(1.0 / 3.0)), 1.0);
}
)glsl");
            shader->setShaderSource(source);
            replaced = true;
        }
        if (!replaced)
            throw std::runtime_error("Reflection lighting probe did not replace the production sky fragment output");

        constexpr int width = 16;
        constexpr int height = 8;
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = width;
        traits->height = height;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize())
            throw std::runtime_error("Reflection lighting probe cannot create its graphics context");
        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        osg::Camera* camera = viewer.getCamera();
        camera->setGraphicsContext(context);
        camera->setViewport(new osg::Viewport(0, 0, width, height));
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setClearMask(GL_COLOR_BUFFER_BIT);
        camera->setClearColor(osg::Vec4f(-1.f, -1.f, -1.f, 0.f));
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setCullingMode(0);
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        osg::ref_ptr<osg::Image> capture = new osg::Image;
        capture->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
        capture->setInternalTextureFormat(GL_RGBA32F);
        camera->attach(osg::Camera::COLOR_BUFFER, capture);
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        for (const osg::Vec3f vertex :
            { osg::Vec3f(-1.f, -1.f, -1.f), osg::Vec3f(6.f, -2.f, -2.f), osg::Vec3f(-4.f, 12.f, -4.f) })
            vertices->push_back(vertex);
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3));
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geometry);
        osg::StateSet* state = geode->getOrCreateStateSet();
        state->setAttributeAndModes(program, osg::StateAttribute::ON);
        state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        state->setMode(GL_BLEND, osg::StateAttribute::OFF);
        state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf::frustum(-1.f, 1.f, -1.f, 1.f, 1.f, 8.f)));
        state->addUniform(new osg::Uniform("pass", 0));
        state->addUniform(new osg::Uniform("isReflection", true));
        state->addUniform(new osg::Uniform("waterHeight", 321.f));
        state->addUniform(new osg::Uniform("osg_ViewMatrixInverse", osg::Matrixf::translate(inputs.eyePosition)));
        state->addUniform(new osg::Uniform("fog.color", inputs.fogColor));
        state->addUniform(new osg::Uniform("fog.start", inputs.fogNear));
        state->addUniform(new osg::Uniform("fog.end", inputs.fogFar));
        state->addUniform(new osg::Uniform("atmosphere.enabled", true));
        state->addUniform(new osg::Uniform("atmosphere.scatteringEnabled", true));
        state->addUniform(new osg::Uniform("atmosphere.cloudsEnabled", true));
        state->addUniform(new osg::Uniform("atmosphere.cloudShadowsEnabled", true));
        state->addUniform(new osg::Uniform("atmosphere.cloudReflectionsEnabled", true));
        state->addUniform(new osg::Uniform("atmosphere.skyColor", inputs.skyColor));
        state->addUniform(new osg::Uniform("atmosphere.inScattering", inputs.inScattering));
        state->addUniform(new osg::Uniform("atmosphere.outScattering", inputs.outScattering));
        state->addUniform(new osg::Uniform("atmosphere.weatherWeight", inputs.weatherWeight));
        state->addUniform(new osg::Uniform("atmosphere.sunDirection",
            osg::Vec3f(inputs.sunDirection.x(), inputs.sunDirection.y(), inputs.sunDirection.z())));
        state->addUniform(new osg::Uniform("atmosphere.solarRadiance", inputs.solarRadiance));
        state->addUniform(new osg::Uniform("atmosphere.skyIrradiance", inputs.skyIrradiance));
        state->addUniform(new osg::Uniform("atmosphere.groundIrradiance", inputs.groundIrradiance));
        state->addUniform(new osg::Uniform("atmosphere.cloudParameters", inputs.cloudParameters));
        state->addUniform(new osg::Uniform("atmosphere.incomingCloudParameters", inputs.incomingCloudParameters));
        state->addUniform(new osg::Uniform("atmosphere.cloudScenarioParameters", inputs.cloudScenarioParameters));
        state->addUniform(new osg::Uniform("atmosphere.lowCloudParameters", inputs.lowCloudParameters));
        state->addUniform(
            new osg::Uniform("atmosphere.cloudAdvectionOffsetWorldUnits", inputs.cloudAdvectionOffsetWorldUnits));
        const auto layers
            = SceneUtil::packCloudLayers(inputs.cloudModel, inputs.incomingCloudModel, inputs.environmentalCloudModel);
        osg::ref_ptr<osg::Uniform> models
            = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "atmosphere.cloudLayers", static_cast<int>(layers.size()));
        for (unsigned index = 0; index < layers.size(); ++index)
            models->setElement(index, layers[index]);
        state->addUniform(models);
        viewer.setSceneData(geode);
        viewer.realize();
        context->makeCurrent();
        program->compileGLObjects(*context->getState());
        const auto* compiled = program->getPCP(*context->getState());
        if (!compiled->isLinked())
        {
            std::string infoLog;
            compiled->getInfoLog(infoLog);
            throw std::runtime_error("Reflection lighting probe failed to link: " + infoLog);
        }
        viewer.frame();
        context->makeCurrent();
        glFinish();
        PassImage result{ width, height, {} };
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const float* pixel = reinterpret_cast<const float*>(capture->data(x, y));
                result.pixels.emplace_back(pixel[0], pixel[1], pixel[2], pixel[3]);
            }
        context->close();
        return result;
    }

    /// Preserve reflected local lighting across sun elevations, layer altitudes, and legacy varying interpolation.
    TEST(AtmosphereShaderTest, reflectedVertexLightingMatchesOriginalFragmentIntegral)
    {
        for (const std::string_view weather : { "Cloudy", "Foggy" })
            for (const bool gpuShader4 : { false, true })
                for (const float sunHeight : { -0.12f, 0.03f, 0.65f })
                    for (const float altitude : { 500.f, 10000.f, 25000.f })
                    {
                        SCOPED_TRACE(::testing::Message() << "weather=" << weather << " gpuShader4=" << gpuShader4
                                                          << " sun=" << sunHeight << " altitude=" << altitude);
                        NativePassInputs inputs = makePackagedAtmosphereInputs(weather, "Sunset");
                        inputs.sunDirection = { 1.f, 0.2f, sunHeight, 0.f };
                        inputs.lowCloudParameters = { 0.4f, 0.9f, altitude, 10000.f };
                        inputs.cloudParameters = { 0.9f, 0.1f, 5000.f, 5000.f };
                        inputs.eyePosition = { -17500.f, 23000.f, altitude + 2000.f };
                        const auto result = renderReflectionLightingProbe(inputs, gpuShader4);
                        ASSERT_EQ(result.pixels.size(), 128u);
                        for (const auto& pixel : result.pixels)
                        {
                            for (int channel = 0; channel < 4; ++channel)
                                ASSERT_TRUE(std::isfinite(pixel[channel]));
                            EXPECT_FLOAT_EQ(pixel.a(), 1.f);
                            EXPECT_GT(pixel.b(), 0.01f);
                            EXPECT_NEAR(pixel.r(), 0.f, 1e-6f);
                            EXPECT_NEAR(pixel.g(), 0.f, 1e-6f);
                        }
                    }
    }

    /// Interpolate reference colors without depending on production shader code.
    Rgb mix(const Rgb& from, const Rgb& to, float factor)
    {
        return { std::lerp(from.r, to.r, factor), std::lerp(from.g, to.g, factor), std::lerp(from.b, to.b, factor) };
    }

    /// Return the representative shader permutation used by atmosphere preprocessing tests.
    Shader::ShaderManager::DefineMap getAtmosphereTestDefines()
    {
        auto defines = Shader::getDefaultDefines();
        defines["lightingMethodClustered"] = "0";
        defines["maxLights"] = "8";
        defines["shadow_texture_unit_list"] = "";
        defines["shadowNormalOffset"] = "0.0";
        defines["exponentialFog"] = "1";
        defines["radialFog"] = "1";
        defines["rainRippleDetail"] = "2";
        defines["rippleMapSize"] = "1024.0";
        defines["rippleMapWorldScale"] = "2.5";
        defines["skyBlending"] = "1";
        defines["sunlightScattering"] = "1";
        defines["waterRefraction"] = "1";
        defines["wobblyShores"] = "1";
        for (const std::string_view name :
            { "additiveBlending", "blendMap", "bumpMap", "darkMap", "decalMap", "detailMap", "diffuseMap",
                "diffuseParallax", "emissiveMap", "envMap", "glossMap", "normalMap", "parallax", "particleOcclusion",
                "reconstructNormalZ", "softParticles", "specularMap", "shadows_enabled", "simpleLighting",
                "adjustCoverage", "alphaToCoverage", "alphaFunc", "useShadowDebugOverlay", "perspectiveShadowMaps",
                "disableNormalOffsetShadows", "limitShadowMapDistance" })
            defines[std::string(name)] = "0";
        return defines;
    }

    /// Evaluate an independent sky-scattering approximation for directional regression checks.
    Rgb referenceSkyScatter(const osg::Vec3f& direction, const osg::Vec3f& sunDirection, const Rgb& skyColor,
        const Rgb& fogColor, float farSkyWeight = 0.44f)
    {
        constexpr Rgb farSkyColor{ 0.371f, 0.637f, 1.108f };
        constexpr Rgb inScattering{ 0.25f, 0.38f, 0.48f };
        constexpr Rgb outScattering{ 0.07f, 0.36f, 0.76f };

        const float skyAmount
            = 1.f - std::pow(std::clamp(1.f - 2.22f * std::clamp(direction.z() - 0.075f, 0.f, 1.f), 0.f, 1.f), 1.15f);
        const Rgb directionalSky = mix(fogColor, skyColor, skyAmount);
        const float sunCosine = direction * sunDirection;
        const float altitude = std::pow(1.f + sunDirection.z(), 10.f);
        const float altitudeScale = 2.8f + 4.3f / altitude;
        const float altitudeBrightness = std::clamp(1.f - std::exp2(-1.9f * altitude), 0.f, 1.f);
        const float horizonSun
            = std::clamp(std::exp(-4.f * sunDirection.z()), 0.f, 1.f) * std::clamp(altitude, 0.f, 1.f);
        const float forwardScatter = (1.58f / (1.24f - sunCosine)) * horizonSun;
        const float diffuseScatter = 1.f - 0.09f * forwardScatter;
        const float atmosphereDepth = 1.33f * std::exp(-2.f * std::clamp(direction.z(), 0.f, 1.f));
        const Rgb directionalScatter = mix(inScattering, outScattering, 0.5f * (1.f + sunCosine));
        const Rgb attenuation{ atmosphereDepth * directionalScatter.r * (altitudeScale + 0.7f * forwardScatter),
            atmosphereDepth * directionalScatter.g * (altitudeScale + 0.7f * forwardScatter),
            atmosphereDepth * directionalScatter.b * (altitudeScale + 0.7f * forwardScatter) };
        const Rgb integrated{ (1.f - std::exp(-attenuation.r)) / attenuation.r,
            (1.f - std::exp(-attenuation.g)) / attenuation.g, (1.f - std::exp(-attenuation.b)) / attenuation.b };
        const Rgb newSky = mix(skyColor, farSkyColor, farSkyWeight);
        const Rgb scattered{ 0.125f * forwardScatter + newSky.r * diffuseScatter,
            0.125f * forwardScatter + newSky.g * diffuseScatter, 0.125f * forwardScatter + newSky.b * diffuseScatter };
        const float depthBrightness = 1.17f * atmosphereDepth + 0.89f;
        const Rgb scatteredSky{ scattered.r * integrated.r * depthBrightness * altitudeBrightness,
            scattered.g * integrated.g * depthBrightness * altitudeBrightness,
            scattered.b * integrated.b * depthBrightness * altitudeBrightness };
        return mix(directionalSky, scatteredSky, 1.f);
    }

    TEST(AtmosphereShaderTest, referenceHorizonMappingsConvergeNumerically)
    {
        constexpr float stockAtmosphereRimZ = -200.f;
        constexpr float alignedRimZ = 50.f * (stockAtmosphereRimZ + 200.f);
        EXPECT_FLOAT_EQ(alignedRimZ, 0.f);

        constexpr float far = 28.f * 8192.f;
        constexpr float skyBlendStart = 0.8f * far;
        float fade = std::clamp((far - far) / (far - skyBlendStart), 0.f, 1.f);
        fade *= fade;

        constexpr float foggedWorldChannel = 0.91f;
        constexpr float sampledSkyChannel = 0.37f;
        const float horizonChannel = foggedWorldChannel * fade + sampledSkyChannel * (1.f - fade);
        EXPECT_FLOAT_EQ(horizonChannel, sampledSkyChannel);
    }

    /// Verify that sky, terrain, objects, foliage, and water include their shared atmospheric interfaces.
    TEST(AtmosphereShaderTest, preprocessesSkyAndWorldShaderPaths)
    {
        Shader::ShaderManager manager;
        manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");

        auto defines = getAtmosphereTestDefines();
        manager.setGlobalDefines(defines);

        const osg::ref_ptr<osg::Shader> skyVertex
            = manager.getShader("compatibility/sky.vert", {}, osg::Shader::VERTEX);
        const osg::ref_ptr<osg::Shader> sky = manager.getShader("compatibility/sky.frag", {}, osg::Shader::FRAGMENT);
        const osg::ref_ptr<osg::Shader> terrain
            = manager.getShader("compatibility/terrain.frag", {}, osg::Shader::FRAGMENT);
        const osg::ref_ptr<osg::Shader> objects
            = manager.getShader("compatibility/objects.frag", {}, osg::Shader::FRAGMENT);
        const osg::ref_ptr<osg::Shader> groundcover
            = manager.getShader("compatibility/groundcover.frag", {}, osg::Shader::FRAGMENT);
        const osg::ref_ptr<osg::Shader> water
            = manager.getShader("compatibility/water.frag", {}, osg::Shader::FRAGMENT);

        ASSERT_TRUE(skyVertex);
        ASSERT_TRUE(sky);
        ASSERT_TRUE(terrain);
        ASSERT_TRUE(objects);
        ASSERT_TRUE(groundcover);
        ASSERT_TRUE(water);
        EXPECT_EQ(skyVertex->getShaderSource().find("position.z = 50.0 * (position.z + 200.0);"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("atmosphereSkyColor"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("atmosphereCloudScattering"), std::string::npos);
        EXPECT_NE(
            sky->getShaderSource().find("atmosphereCloudScattering(worldDirection, skyRadiance,"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseApplyAerialPerspectiveTowardTarget"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseIntegrateCloudsStable"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseApplyAerialPerspective"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseIncidentSolarRadiance"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("lessThan(transmittance, vec3(0.005))"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("atmosphereSunDisc"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("haloAngularDistance"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("float halo ="), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseTriangularDither(gl_FragCoord.xy) * (0.5 / 255.0)"),
            std::string::npos);
        const std::size_t legacyCloudPass = sky->getShaderSource().find("void paintClouds");
        const std::size_t moonPass = sky->getShaderSource().find("void paintMoon");
        ASSERT_NE(legacyCloudPass, std::string::npos);
        ASSERT_NE(moonPass, std::string::npos);
        EXPECT_NE(sky->getShaderSource()
                      .substr(legacyCloudPass, moonPass - legacyCloudPass)
                      .find("if (atmosphere.cloudsEnabled ||"),
            std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("color.a = 1.0;"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("normalize(atmosphere.sunDirection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("dot(worldDirection, sunDirection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("vec3(0.371, 0.637, 1.108), 0.44"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("vec3(0.371, 0.637, 1.108), 0.62"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("atmosphereScatter"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("openmwseAtmosphereScatteredLight"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("sampleSkyColor"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("parameters.scatteringEnabled = atmosphere.scatteringEnabled"),
            std::string::npos);
        EXPECT_NE(objects->getShaderSource().find("atmosphereScatter"), std::string::npos);
        EXPECT_NE(groundcover->getShaderSource().find("atmosphereScatter"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("if (isReflection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("position.z = 2.0 * waterHeight - position.z"), std::string::npos);
        EXPECT_NE(water->getShaderSource().find("sampleReflectionMap(screenCoords + reflectionCoordsOffset)"),
            std::string::npos);
        EXPECT_NE(water->getShaderSource().find("mix(max(reflection, vec3(0.0)), vec3(1.0), 0.05)"), std::string::npos);
        EXPECT_NE(water->getShaderSource().find("vec3 waterInScatteringColor()"), std::string::npos);
        EXPECT_NE(water->getShaderSource().find("vec3 waterColor = waterInScatteringColor()"), std::string::npos);
        EXPECT_EQ(water->getShaderSource().find("WATER_COLOR * length(sun.ambient.xyz)"), std::string::npos);
        EXPECT_NE(objects->getShaderSource().find("vec3 waterInScatteringColor()"), std::string::npos);
        EXPECT_NE(objects->getShaderSource().find("colorOffset += waterInScatteringColor()"), std::string::npos);
        EXPECT_NE(objects->getShaderSource().find("float airEuclideanDistance = euclideanDist"), std::string::npos);
        EXPECT_NE(
            objects->getShaderSource().find(
                "if (atmosphere.enabled && atmosphere.scatteringEnabled && !cameraBelowWater && fog.depth < 0.0)"),
            std::string::npos);
        EXPECT_EQ(objects->getShaderSource().find("!cameraBelowWater && !useWaterDepthFog"), std::string::npos);
        EXPECT_EQ(objects->getShaderSource().find("WATER_COLOR * length(sun.ambient.xyz)"), std::string::npos);
        EXPECT_EQ(water->getShaderSource().find("skyColorEstimate"), std::string::npos);
    }

    TEST(AtmosphereShaderTest, waterInScatteringPreservesAmbientSpectrumWithoutNormAmplification)
    {
        const osg::Vec3f waterFilter(0.090195f, 0.115685f, 0.12745f);
        const osg::Vec3f cloudyAmbient(0.72f, 0.68f, 0.61f);
        const osg::Vec3f filteredAmbient(waterFilter.x() * cloudyAmbient.x(), waterFilter.y() * cloudyAmbient.y(),
            waterFilter.z() * cloudyAmbient.z());
        const osg::Vec3f normAmplified = waterFilter * cloudyAmbient.length();

        EXPECT_LT(filteredAmbient.x(), normAmplified.x());
        EXPECT_LT(filteredAmbient.y(), normAmplified.y());
        EXPECT_LT(filteredAmbient.z(), normAmplified.z());
        EXPECT_NEAR(filteredAmbient.x(), 0.0649404f, 1e-7f);
        EXPECT_NEAR(filteredAmbient.y(), 0.0786658f, 1e-7f);
        EXPECT_NEAR(filteredAmbient.z(), 0.0777445f, 1e-7f);
    }

    TEST(AtmosphereShaderTest, submergedObjectTransportComposesAirBeforeWater)
    {
        const osg::Vec3f objectRadiance(0.18f, 0.12f, 0.08f);
        const osg::Vec3f airInScattering(0.11f, 0.065f, 0.03f);
        const osg::Vec3f waterInScattering(0.035f, 0.052f, 0.061f);
        constexpr float airVisibility = 0.64f;
        constexpr float waterVisibility = 0.25f;

        const osg::Vec3f transported = objectRadiance * (airVisibility * waterVisibility) + airInScattering
            + waterInScattering * ((1.f - waterVisibility) * airVisibility);

        EXPECT_NEAR(transported.x(), 0.1556f, 1e-7f);
        EXPECT_NEAR(transported.y(), 0.10916f, 1e-7f);
        EXPECT_NEAR(transported.z(), 0.07208f, 1e-7f);
        EXPECT_GT(transported.x(), transported.y());
        EXPECT_GT(transported.y(), transported.z());
    }

    /// Compile each reflected cloud pass and verify that its active samplers cannot alias ordinary sky textures.
    TEST(AtmosphereShaderTest, compilesAndLinksSkyProgramInGraphicsContext)
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = 1;
        traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;

        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

        for (const bool skyBlending : { false, true })
            for (const bool gpuShader4 : { false, true })
            {
                SCOPED_TRACE(testing::Message() << "skyBlending=" << skyBlending << " gpuShader4=" << gpuShader4);
                Shader::ShaderManager manager;
                manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
                auto defines = getAtmosphereTestDefines();
                defines["skyBlending"] = skyBlending ? "1" : "0";
                defines["useGPUShader4"] = gpuShader4 ? "1" : "0";
                manager.setGlobalDefines(defines);
                const osg::ref_ptr<osg::Program> program = manager.getProgram("compatibility/sky");
                ASSERT_TRUE(program);
                osg::State& state = *context->getState();
                for (const char* mode : { "0", "1", "2" })
                {
                    SCOPED_TRACE(testing::Message() << "reflectedCloudMode=" << mode);
                    osg::ref_ptr<osg::StateSet> passState = new osg::StateSet;
                    passState->setDefine(Shader::sReflectedCloudModeDefine, mode, osg::StateAttribute::ON);
                    state.pushStateSet(passState);
                    program->compileGLObjects(state);
                    const osg::Program::PerContextProgram* compiled = program->getPCP(state);
                    std::string infoLog;
                    compiled->getInfoLog(infoLog);
                    EXPECT_TRUE(compiled->isLinked()) << infoLog;
                    const auto& uniforms = compiled->getActiveUniforms();
                    EXPECT_EQ(
                        uniforms.count(osg::Uniform::getNameID("reflectedCloudTexture")), mode[0] == '2' ? 1u : 0u);
                    EXPECT_EQ(uniforms.count(osg::Uniform::getNameID("maskMap")), mode[0] == '0' ? 1u : 0u);
                    // The reflected composite also draws the fallback solar texture after its cloud layers.
                    EXPECT_EQ(uniforms.count(osg::Uniform::getNameID("diffuseMap")), mode[0] != '1' ? 1u : 0u);
                    state.popStateSet();
                }
            }

        context->releaseContext();
        context->close();
    }

    TEST(AtmosphereShaderTest, compilesAndLinksWaterReflectionProgramInGraphicsContext)
    {
        Shader::ShaderManager manager;
        manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = getAtmosphereTestDefines();
        manager.setGlobalDefines(defines);

        const osg::ref_ptr<osg::Program> program = manager.getProgram("compatibility/water");
        ASSERT_TRUE(program);

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = 1;
        traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

        program->compileGLObjects(*context->getState());
        const osg::Program::PerContextProgram* compiled = program->getPCP(*context->getState());
        std::string infoLog;
        compiled->getInfoLog(infoLog);
        EXPECT_TRUE(compiled->isLinked()) << infoLog;
        context->releaseContext();
        context->close();
    }

    /// Exercise both linked lighting stages with reversed grass normals and ordinary one-sided surfaces.
    TEST(AtmosphereShaderTest, linkedGroundcoverLightingIlluminatesBothSides)
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->alpha = 8;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        ASSERT_TRUE(context && context->realize() && context->makeCurrent());
        for (const bool clustered : { false, true })
            for (const bool grass : { false, true })
            {
                Shader::ShaderManager manager;
                manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
                auto defines = getAtmosphereTestDefines();
                defines["lightingMethodClustered"] = clustered ? "1" : "0";
                defines["groundcover"] = grass ? "1" : "0";
                manager.setGlobalDefines(defines);
                osg::ref_ptr<osg::Program> program = new osg::Program;
                program->addShader(new osg::Shader(osg::Shader::VERTEX, R"glsl(
                    #version 130
                    float lambert(vec3 n, vec3 l, vec3 v);
                    varying float vertexLight;
                    void main() {
                        vertexLight = lambert(vec3(0,0,-1), vec3(0,0,1), vec3(1,0,0));
                        gl_Position = gl_Vertex;
                    }
                )glsl"));
                program->addShader(new osg::Shader(osg::Shader::FRAGMENT, R"glsl(
                    #version 130
                    float lambert(vec3 n, vec3 l, vec3 v);
                    varying float vertexLight;
                    void main() {
                        gl_FragColor = vec4(lambert(vec3(0,0,-1), vec3(0,0,1), vec3(1,0,0)),
                            vertexLight, lambert(vec3(0,0,1), vec3(0,0,1), vec3(1,0,0)),
                            lambert(vec3(0,0,-1), vec3(0,0,1), vec3(0,0,1)));
                    }
                )glsl"));
                for (const auto stage : { osg::Shader::VERTEX, osg::Shader::FRAGMENT })
                {
                    const std::string name = stage == osg::Shader::VERTEX ? "vertex" : "fragment";
                    auto lighting = manager.getShader(
                        "lib/core/lighting_" + name + (clustered ? ".glsl" : "_legacy.glsl"), defines, stage);
                    ASSERT_TRUE(lighting);
                    program->addShader(lighting);
                }
                program->compileGLObjects(*context->getState());
                const auto* compiled = program->getPCP(*context->getState());
                std::string info;
                compiled->getInfoLog(info);
                ASSERT_TRUE(compiled->isLinked()) << info;
                program->apply(*context->getState());
                glViewport(0, 0, 1, 1);
                glBegin(GL_TRIANGLES);
                glVertex2f(-1.f, -1.f);
                glVertex2f(3.f, -1.f);
                glVertex2f(-1.f, 3.f);
                glEnd();
                osg::Vec4f pixel;
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel.ptr());
                EXPECT_NEAR(pixel.x(), grass ? 1.f : 0.f, 0.01f);
                EXPECT_NEAR(pixel.y(), grass ? 1.f : 0.f, 0.01f);
                EXPECT_NEAR(pixel.z(), 1.f, 0.01f);
                EXPECT_NEAR(pixel.w(), grass ? 0.3f : 0.f, 0.01f);
            }
        context->releaseContext();
        context->close();
    }

    TEST(AtmosphereShaderTest, compilesAndLinksCloudShadowSurfaceProgramsInGraphicsContext)
    {
        Shader::ShaderManager manager;
        manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = getAtmosphereTestDefines();
        defines["diffuseMap"] = "1";
        for (const std::string_view mapName : { "bumpMap", "darkMap", "decalMap", "detailMap", "diffuseMap",
                 "emissiveMap", "envMap", "glossMap", "normalMap", "specularMap" })
            defines[std::string(mapName) + "UV"] = "0";
        defines["groundcoverStompMode"] = "0";
        defines["groundcoverStompIntensity"] = "0.0";
        defines["disableNormals"] = "0";
        manager.setGlobalDefines(defines);

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = 1;
        traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

        for (const std::string_view programName :
            { "compatibility/terrain", "compatibility/objects", "compatibility/groundcover" })
        {
            const osg::ref_ptr<osg::Program> program = manager.getProgram(std::string(programName), defines);
            ASSERT_TRUE(program) << programName;
            program->compileGLObjects(*context->getState());
            const osg::Program::PerContextProgram* compiled = program->getPCP(*context->getState());
            std::string infoLog;
            compiled->getInfoLog(infoLog);
            EXPECT_TRUE(compiled->isLinked()) << programName << ": " << infoLog;
        }

        context->releaseContext();
        context->close();
    }

    TEST(AtmosphereShaderTest, distantAlphaTestedFoliagePreservesCoverageWithoutDarkTraces)
    {
        const PassImage unadjusted = renderDistantFoliageCard(false);
        const PassImage adjusted = renderDistantFoliageCard(true);
        ASSERT_FALSE(unadjusted.pixels.empty());
        ASSERT_EQ(adjusted.pixels.size(), unadjusted.pixels.size());

        float maximumUnadjustedAlpha = 0.f;
        float maximumAdjustedAlpha = 0.f;
        float minimumUnadjustedLuminance = 1.f;
        float maximumUnadjustedLuminance = 0.f;
        for (const osg::Vec4f& pixel : unadjusted.pixels)
        {
            maximumUnadjustedAlpha = std::max(maximumUnadjustedAlpha, pixel.w());
            const float luminance = pixel.x() * 0.2126f + pixel.y() * 0.7152f + pixel.z() * 0.0722f;
            minimumUnadjustedLuminance = std::min(minimumUnadjustedLuminance, luminance);
            maximumUnadjustedLuminance = std::max(maximumUnadjustedLuminance, luminance);
        }
        for (const osg::Vec4f& pixel : adjusted.pixels)
            maximumAdjustedAlpha = std::max(maximumAdjustedAlpha, pixel.w());
        EXPECT_GT(maximumUnadjustedAlpha, 0.5f)
            << "minimumLuminance=" << minimumUnadjustedLuminance << " maximumLuminance=" << maximumUnadjustedLuminance;
        EXPECT_GT(maximumAdjustedAlpha, 0.5f);

        constexpr float backgroundLuminance = 0.78f * 0.2126f + 0.71f * 0.7152f + 0.66f * 0.0722f;
        const auto measure = [](const PassImage& image) {
            std::size_t coveredPixelCount = 0;
            std::size_t darkTracePixelCount = 0;
            for (const osg::Vec4f& pixel : image.pixels)
            {
                if (pixel.w() <= 0.5f)
                    continue;
                ++coveredPixelCount;
                const float luminance = pixel.x() * 0.2126f + pixel.y() * 0.7152f + pixel.z() * 0.0722f;
                if (luminance < backgroundLuminance - 0.08f)
                    ++darkTracePixelCount;
            }
            return std::pair(coveredPixelCount, darkTracePixelCount);
        };
        const auto [unadjustedCoverage, unadjustedDarkTraces] = measure(unadjusted);
        const auto [adjustedCoverage, adjustedDarkTraces] = measure(adjusted);
        double sharedSurfaceRadianceDifference = 0.0;
        std::size_t sharedSurfaceCount = 0;
        for (std::size_t index = 0; index < adjusted.pixels.size(); ++index)
        {
            if (unadjusted.pixels[index].w() <= 0.5f || adjusted.pixels[index].w() <= 0.5f)
                continue;
            for (int channel = 0; channel < 3; ++channel)
                sharedSurfaceRadianceDifference
                    += std::abs(adjusted.pixels[index][channel] - unadjusted.pixels[index][channel]);
            ++sharedSurfaceCount;
        }

        EXPECT_GT(adjustedCoverage, unadjustedCoverage);
        ASSERT_GT(sharedSurfaceCount, 100u);
        EXPECT_LT(sharedSurfaceRadianceDifference / static_cast<double>(sharedSurfaceCount), 1e-5)
            << "Alpha coverage adjustment may change fragment acceptance but not accepted-fragment radiance";
        EXPECT_LE(adjustedDarkTraces, unadjustedDarkTraces)
            << "adjustedCoverage=" << adjustedCoverage << " unadjustedCoverage=" << unadjustedCoverage;
    }

    TEST(AtmosphereShaderTest, externalProductionLeafMipsDoNotBecomeDarkOpaqueTraces)
    {
        const osg::ref_ptr<osg::Image> leafTexture = loadExternalFoliageTexture();
        if (!leafTexture)
            GTEST_SKIP() << "Set OPENMW_FOLIAGE_TEST_DATA to ordered production data directories";

        const PassImage unadjusted = renderDistantFoliageCard(false, leafTexture);
        const PassImage adjusted = renderDistantFoliageCard(true, leafTexture);
        ASSERT_FALSE(unadjusted.pixels.empty());
        ASSERT_EQ(adjusted.pixels.size(), unadjusted.pixels.size());

        constexpr float backgroundLuminance = 0.78f * 0.2126f + 0.71f * 0.7152f + 0.66f * 0.0722f;
        const auto measure = [](const PassImage& image) {
            std::size_t coveredPixelCount = 0;
            std::size_t darkTracePixelCount = 0;
            double accumulatedDeficit = 0.0;
            for (const osg::Vec4f& pixel : image.pixels)
            {
                if (pixel.w() <= 0.5f)
                    continue;
                ++coveredPixelCount;
                const float luminance = pixel.x() * 0.2126f + pixel.y() * 0.7152f + pixel.z() * 0.0722f;
                const float deficit = std::max(backgroundLuminance - luminance, 0.f);
                accumulatedDeficit += deficit;
                darkTracePixelCount += deficit > 0.08f;
            }
            return std::tuple(coveredPixelCount, darkTracePixelCount, accumulatedDeficit);
        };
        const auto [unadjustedCoverage, unadjustedDarkTraces, unadjustedDeficit] = measure(unadjusted);
        const auto [adjustedCoverage, adjustedDarkTraces, adjustedDeficit] = measure(adjusted);

        EXPECT_GT(adjustedCoverage, 0u);
        EXPECT_LE(adjustedDarkTraces, unadjustedDarkTraces)
            << "adjustedCoverage=" << adjustedCoverage << " unadjustedCoverage=" << unadjustedCoverage
            << " adjustedDeficit=" << adjustedDeficit << " unadjustedDeficit=" << unadjustedDeficit;
        EXPECT_LE(adjustedDeficit, unadjustedDeficit + 0.01)
            << "adjustedCoverage=" << adjustedCoverage << " unadjustedCoverage=" << unadjustedCoverage
            << " adjustedDarkTraces=" << adjustedDarkTraces << " unadjustedDarkTraces=" << unadjustedDarkTraces;
    }

    TEST(AtmosphereShaderTest, compilesCoveragePreservingAlphaTestPermutationsInGraphicsContext)
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = 1;
        traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

        for (const bool alphaToCoverage : { false, true })
            for (const std::string_view programName :
                { "compatibility/objects", "compatibility/bs/default", "compatibility/bs/nolighting" })
            {
                Shader::ShaderManager manager;
                manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
                auto defines = getAtmosphereTestDefines();
                defines["diffuseMap"] = "1";
                defines["adjustCoverage"] = "1";
                defines["alphaFunc"] = "516";
                defines["alphaToCoverage"] = alphaToCoverage ? "1" : "0";
                for (const std::string_view mapName : { "bumpMap", "darkMap", "decalMap", "detailMap", "diffuseMap",
                         "emissiveMap", "envMap", "glossMap", "normalMap", "specularMap" })
                    defines[std::string(mapName) + "UV"] = "0";
                manager.setGlobalDefines(defines);

                const osg::ref_ptr<osg::Program> program = manager.getProgram(std::string(programName), defines);
                ASSERT_TRUE(program) << programName;
                program->compileGLObjects(*context->getState());
                const osg::Program::PerContextProgram* compiled = program->getPCP(*context->getState());
                std::string infoLog;
                compiled->getInfoLog(infoLog);
                EXPECT_TRUE(compiled->isLinked())
                    << programName << " alphaToCoverage=" << alphaToCoverage << ": " << infoLog;
            }

        context->releaseContext();
        context->close();
    }

    TEST(AtmosphereShaderTest, productionShaderDiagnosticsExposeEveryDistantFoliageStage)
    {
        const osg::ref_ptr<osg::Image> productionImage = loadExternalFoliageTexture();
        const PassImage unadjusted = renderDistantFoliageCard(false, productionImage);
        const PassImage normal = renderDistantFoliageCard(true, productionImage);
        const PassImage coverageBypassed = renderDistantFoliageCard(true, productionImage, 1);
        const PassImage fogBypassed = renderDistantFoliageCard(true, productionImage, 2);
        const PassImage fogVisibility = renderDistantFoliageCard(true, productionImage, 3);
        const PassImage fogInScattering = renderDistantFoliageCard(true, productionImage, 4);
        const PassImage preFogRadiance = renderDistantFoliageCard(true, productionImage, 5);
        const PassImage sampledSurfaceColor = renderDistantFoliageCard(true, productionImage, 6);

        ASSERT_FALSE(normal.pixels.empty());
        for (const PassImage* diagnostic : { &coverageBypassed, &fogBypassed, &fogVisibility, &fogInScattering,
                 &preFogRadiance, &sampledSurfaceColor })
            ASSERT_EQ(diagnostic->pixels.size(), normal.pixels.size());

        const auto meanRgbDifference = [](const PassImage& first, const PassImage& second) {
            double difference = 0.0;
            for (std::size_t index = 0; index < first.pixels.size(); ++index)
                for (int channel = 0; channel < 3; ++channel)
                    difference += std::abs(first.pixels[index][channel] - second.pixels[index][channel]);
            return difference / static_cast<double>(first.pixels.size());
        };
        EXPECT_LT(meanRgbDifference(coverageBypassed, unadjusted), 1e-5)
            << "The runtime coverage bypass must reproduce the unadjusted material path";
        EXPECT_GT(meanRgbDifference(normal, fogBypassed), 0.001);
        EXPECT_GT(meanRgbDifference(normal, fogVisibility), 0.001);
        EXPECT_LT(meanRgbDifference(fogBypassed, preFogRadiance), 1e-5)
            << "The object pre-fog output must isolate the same lit radiance accepted by the fog bypass";

        std::size_t diagnosticSurfaceCount = 0;
        for (std::size_t index = 0; index < fogVisibility.pixels.size(); ++index)
        {
            if (fogVisibility.pixels[index].w() <= 0.5f)
                continue;
            ++diagnosticSurfaceCount;
            EXPECT_NEAR(fogVisibility.pixels[index].x(), fogVisibility.pixels[index].y(), 1e-5f);
            EXPECT_NEAR(fogVisibility.pixels[index].y(), fogVisibility.pixels[index].z(), 1e-5f);
            for (const PassImage* diagnostic : { &fogInScattering, &preFogRadiance, &sampledSurfaceColor })
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_TRUE(std::isfinite(diagnostic->pixels[index][channel]));
        }
        EXPECT_GT(diagnosticSurfaceCount, 100u);
    }

    /// Compile the compatibility passes and keep their interface within the GL 3.3 scalar uniform budget.
    TEST(AtmosphereShaderTest, compilesAndLinksNativeAtmospherePassesInGraphicsContext)
    {
        VFS::Manager vfs;
        vfs.addArchive(
            std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data"));
        vfs.addArchive(
            std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files"));
        vfs.buildIndex();
        Resource::ImageManager imageManager(&vfs, 0.0);
        Fx::Technique technique(vfs, imageManager, VFS::Path::NormalizedView("shaders/internal_atmosphere.omwfx"),
            "internal_atmosphere", 128, 128, false, true);
        ASSERT_TRUE(technique.compile()) << technique.getLastError();
        ASSERT_EQ(technique.getPasses().size(), 8);
        EXPECT_EQ(technique.getPasses().front()->getSecondaryTarget(), "AtmosphereVolumeMetadataRaw");
        EXPECT_TRUE(technique.getLights());
        EXPECT_NE(technique.getFlags() & Fx::Technique::Flag_Disable_Interiors, 0u);
        EXPECT_NE(technique.getFlags() & Fx::Technique::Flag_Disable_Underwater, 0u);
        // Native glare follows its live preference; the owning effect must not disable its own response.
        EXPECT_EQ(technique.getFlags() & Fx::Technique::Flag_Disable_SunGlare, 0u);

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = 1;
        traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "2.1";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

        for (const std::shared_ptr<Fx::Pass>& pass : technique.getPasses())
        {
            osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
            pass->prepareStateSet(stateSet, technique.getName());
            osg::Program* program = dynamic_cast<osg::Program*>(stateSet->getAttribute(osg::StateAttribute::PROGRAM));
            ASSERT_NE(program, nullptr);
            program->compileGLObjects(*context->getState());
            const osg::Program::PerContextProgram* compiled = program->getPCP(*context->getState());
            std::string infoLog;
            compiled->getInfoLog(infoLog);
            EXPECT_TRUE(compiled->isLinked()) << pass->getName() << ": " << infoLog;
            int uniformComponents = 0;
            for (const auto& [name, uniform] : compiled->getActiveUniforms())
                uniformComponents += uniform._size
                    * osg::Uniform::getTypeNumComponents(static_cast<osg::Uniform::Type>(uniform._type));
            EXPECT_LE(uniformComponents, 1024) << pass->getName();
        }

        context->releaseContext();
        context->close();
    }

    TEST(AtmosphereShaderTest, packagedNativeAtmosphereTechniqueResolvesEveryShaderInclude)
    {
        VFS::Manager vfs;
        vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(
            std::filesystem::path(OPENMW_PROJECT_BINARY_DIR) / "resources/vfs"));
        vfs.buildIndex();
        Resource::ImageManager imageManager(&vfs, 0.0);
        Fx::Technique technique(vfs, imageManager, VFS::Path::NormalizedView("shaders/internal_atmosphere.omwfx"),
            "internal_atmosphere", 128, 128, false, true);

        ASSERT_TRUE(technique.compile()) << technique.getLastError();
        EXPECT_EQ(technique.getPasses().size(), 5);
        EXPECT_EQ(technique.getPasses().front()->getSecondaryTarget(), "AtmosphereVolumeMetadataRaw");
    }

    TEST(AtmosphereShaderTest, compilesAndLinksNativeAtmospherePassesWithUniformBuffers)
    {
        VFS::Manager vfs;
        vfs.addArchive(
            std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data"));
        vfs.addArchive(
            std::make_unique<VFS::FileSystemArchive>(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files"));
        vfs.buildIndex();
        Resource::ImageManager imageManager(&vfs, 0.0);
        Fx::Technique technique(vfs, imageManager, VFS::Path::NormalizedView("shaders/internal_atmosphere.omwfx"),
            "internal_atmosphere", 128, 128, true, true);
        ASSERT_TRUE(technique.compile()) << technique.getLastError();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = 1;
        traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "4.5";
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "No offscreen uniform-buffer OpenGL context is available";

        for (const std::shared_ptr<Fx::Pass>& pass : technique.getPasses())
        {
            osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
            pass->prepareStateSet(stateSet, technique.getName());
            osg::Program* program = dynamic_cast<osg::Program*>(stateSet->getAttribute(osg::StateAttribute::PROGRAM));
            ASSERT_NE(program, nullptr);
            program->compileGLObjects(*context->getState());
            const osg::Program::PerContextProgram* compiled = program->getPCP(*context->getState());
            std::string infoLog;
            compiled->getInfoLog(infoLog);
            EXPECT_TRUE(compiled->isLinked()) << pass->getName() << ": " << infoLog;
        }

        context->releaseContext();
        context->close();
    }

    /// Keep elevated local samples from lifting distant mist and fog off their world-space terrain.
    TEST(AtmosphereShaderTest, localMediaIgnoreObserverTerrainEstimate)
    {
        for (const bool fog : { false, true })
            for (const bool ubo : { false, true })
            {
                NativePassInputs inputs;
                inputs.width = 16;
                inputs.height = 12;
                inputs.useUniformBuffer = ubo;
                inputs.eyePosition = { 0.f, 0.f, 10000.f };
                inputs.viewDirection = { 1.f, 0.f, -0.02f };
                inputs.viewDirection.normalize();
                inputs.farPlane = 204800.f;
                inputs.terrainHeight = [](const osg::Vec2f&) { return 0.f; };
                inputs.mistParameters = fog ? osg::Vec4f() : osg::Vec4f(0.12f, 500.f, 3000.f, 0.f);
                inputs.fogVolumeParameters = fog ? osg::Vec4f(0.12f, 500.f, 3000.f, 0.3f) : osg::Vec4f();
                const auto flat = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                inputs.mediumReferenceParameters.x() = 8050.f;
                inputs.orographicParameters = { 30000.f, 40000.f, 0.1f, -0.05f };
                const auto mountain = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(flat.pixels.empty());
                EXPECT_EQ(flat.pixels, mountain.pixels);
                EXPECT_FLOAT_EQ(mountain.at(8, 8).w(), 1.f) << "Mist must not form a suspended horizon sheet";
                EXPECT_LT(mountain.at(8, 0).w(), 0.999f) << "Ground-directed rays must retain the medium";
                inputs.mediumReferenceParameters = {};
                inputs.orographicParameters = {};
                inputs.terrainHeight = [](const osg::Vec2f&) { return 8000.f; };
                const auto plateau = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                EXPECT_LT(plateau.at(8, 8).w(), 0.999f) << "Mist and fog must follow elevated world terrain";
            }
    }

    TEST(AtmosphereShaderTest, executesMistIntegrationAgainstControlledDepth)
    {
        NativePassInputs clearInputs;
        clearInputs.mistParameters = {};
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereRayGrid", clearInputs);
        ASSERT_FALSE(clear.pixels.empty());

        NativePassInputs mistInputs;
        mistInputs.mistParameters = { 2.5f, 800.f, 4500.f, 1.f };
        const PassImage mist = renderNativeAtmospherePass("integrateAtmosphereRayGrid", mistInputs);
        ASSERT_EQ(mist.pixels.size(), clear.pixels.size());

        double scatteredLight = 0.0;
        double transmittance = 0.0;
        for (const osg::Vec4f& pixel : mist.pixels)
        {
            EXPECT_TRUE(std::isfinite(pixel.x()));
            EXPECT_TRUE(std::isfinite(pixel.y()));
            EXPECT_TRUE(std::isfinite(pixel.z()));
            EXPECT_TRUE(std::isfinite(pixel.w()));
            scatteredLight += pixel.x() + pixel.y() + pixel.z();
            transmittance += pixel.w();
        }
        EXPECT_GT(scatteredLight / static_cast<double>(mist.pixels.size()), 0.01);
        EXPECT_LT(transmittance / static_cast<double>(mist.pixels.size()), 0.995);
        EXPECT_FLOAT_EQ(clear.at(clear.width / 2, clear.height / 2).w(), 1.f);

        NativePassInputs disabledInputs = mistInputs;
        disabledInputs.atmosphereEnabled = false;
        const PassImage disabled = renderNativeAtmospherePass("integrateAtmosphereRayGrid", disabledInputs);
        ASSERT_EQ(disabled.pixels.size(), clear.pixels.size());
        EXPECT_NEAR(disabled.at(disabled.width / 2, disabled.height / 2).x(), 0.f, 1e-6f);
        EXPECT_FLOAT_EQ(disabled.at(disabled.width / 2, disabled.height / 2).w(), 1.f);
    }

    TEST(AtmosphereShaderTest, cloudsRemainVisibleWithoutEnhancedScattering)
    {
        NativePassInputs inputs;
        inputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        inputs.mistParameters = {};
        inputs.scatteringEnabled = false;
        for (const bool useUBO : { false, true })
        {
            inputs.useUniformBuffer = useUBO;
            const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            double opacity = 0.0;
            double radiance = 0.0;
            for (const auto& pixel : clouds.pixels)
            {
                ASSERT_TRUE(std::isfinite(pixel.x()) && std::isfinite(pixel.y()) && std::isfinite(pixel.z()));
                opacity += 1.0 - pixel.w();
                radiance += pixel.x() + pixel.y() + pixel.z();
            }
            ASSERT_FALSE(clouds.pixels.empty());
            EXPECT_GT(opacity / clouds.pixels.size(), 0.005);
            EXPECT_GT(radiance / clouds.pixels.size(), 0.01);
        }
    }

    TEST(AtmosphereShaderTest, integratesCloudsBehindOpenSkyAndRespectsSceneDepth)
    {
        NativePassInputs openSkyInputs;
        openSkyInputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        openSkyInputs.mistParameters = {};
        const PassImage openSky = renderNativeAtmospherePass("integrateAtmosphereRayGrid", openSkyInputs);
        ASSERT_FALSE(openSky.pixels.empty());

        double scatteredLight = 0.0;
        double lostTransmittance = 0.0;
        for (const osg::Vec4f& pixel : openSky.pixels)
        {
            EXPECT_TRUE(std::isfinite(pixel.x()));
            EXPECT_TRUE(std::isfinite(pixel.y()));
            EXPECT_TRUE(std::isfinite(pixel.z()));
            EXPECT_TRUE(std::isfinite(pixel.w()));
            scatteredLight += pixel.x() + pixel.y() + pixel.z();
            lostTransmittance += 1.0 - pixel.w();
        }
        EXPECT_GT(scatteredLight / static_cast<double>(openSky.pixels.size()), 0.01);
        EXPECT_GT(lostTransmittance / static_cast<double>(openSky.pixels.size()), 0.005);

        NativePassInputs occludedInputs = openSkyInputs;
        occludedInputs.depth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 1.f : 0.f; };
        const PassImage occluded = renderNativeAtmospherePass("integrateAtmosphereRayGrid", occludedInputs);
        ASSERT_EQ(occluded.pixels.size(), openSky.pixels.size());
        for (const osg::Vec4f& pixel : occluded.pixels)
        {
            EXPECT_NEAR(pixel.x(), 0.f, 1e-6f);
            EXPECT_NEAR(pixel.y(), 0.f, 1e-6f);
            EXPECT_NEAR(pixel.z(), 0.f, 1e-6f);
            EXPECT_NEAR(pixel.w(), 1.f, 1e-6f);
        }
    }

    TEST(AtmosphereShaderTest, integratesCloudLayerBelowAnElevatedObserver)
    {
        NativePassInputs clearInputs;
        clearInputs.eyePosition = { 0.f, 0.f, 140000.f };
        clearInputs.viewDirection = { 0.18f, 0.05f, -1.f };
        clearInputs.mistParameters = {};
        clearInputs.farPlane = 300000.f;
        clearInputs.fogNear = 1000000.f;
        clearInputs.fogFar = 2000000.f;
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereRayGrid", clearInputs);

        NativePassInputs cloudInputs = clearInputs;
        cloudInputs.cloudParameters = { 0.95f, 1.3f, 48000.f, 22000.f };
        const PassImage cloudy = renderNativeAtmospherePass("integrateAtmosphereRayGrid", cloudInputs);
        ASSERT_EQ(cloudy.pixels.size(), clear.pixels.size());

        double difference = 0.0;
        double lostTransmittance = 0.0;
        for (std::size_t index = 0; index < clear.pixels.size(); ++index)
        {
            for (int channel = 0; channel < 3; ++channel)
                difference += std::abs(cloudy.pixels[index][channel] - clear.pixels[index][channel]);
            lostTransmittance += 1.0 - cloudy.pixels[index].w();
        }
        const double pixelCount = static_cast<double>(clear.pixels.size());
        EXPECT_GT(difference / pixelCount, 0.01)
            << "A world-space cloud layer must contribute radiance below an elevated observer";
        EXPECT_GT(lostTransmittance / pixelCount, 0.005)
            << "A world-space cloud layer must attenuate the scene below an elevated observer";
    }

    /// Attenuate ground-level mist by the cloud deck between it and an elevated observer.
    TEST(AtmosphereShaderTest, overheadCloudsOccludeGroundMist)
    {
        NativePassInputs inputs;
        inputs.eyePosition = { 0.f, 0.f, 140000.f };
        inputs.viewDirection = { 0.18f, 0.05f, -1.f };
        inputs.farPlane = 300000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;
        inputs.cloudParameters = { 0.95f, 1.3f, 48000.f, 22000.f };
        inputs.cloudModel.mExtinctionPerMeter = { 0.086f, 0.086f, 0.086f };
        inputs.mistParameters = {};
        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.mistParameters = { 1.f, 800.f, 4500.f, 1.f };
        const PassImage combined = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.cloudParameters.y() = 0.f;
        const PassImage mist = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        double error = 0.0;
        double mistOpacity = 0.0;
        for (std::size_t i = 0; i < clouds.pixels.size(); ++i)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                const float expected = clouds.pixels[i][channel] + clouds.pixels[i].w() * mist.pixels[i][channel];
                error += std::abs(combined.pixels[i][channel] - expected);
            }
            EXPECT_NEAR(combined.pixels[i].w(), clouds.pixels[i].w() * mist.pixels[i].w(), 0.001f);
            mistOpacity += 1.f - mist.pixels[i].w();
        }
        EXPECT_GT(mistOpacity / clouds.pixels.size(), 0.1);
        EXPECT_LT(error / (clouds.pixels.size() * 3), 0.002)
            << "Mist beneath a separate cloud deck must contribute only through the cloud transmittance";
    }

    /// Verify that main-cloud presentation uses one radial fade at every observer altitude.
    TEST(AtmosphereShaderTest, mainCloudPresentationRangeIsObserverIndependent)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.cloudDensitySliceQuantity = 13;
        inputs.renderCloudDensitySlice = true;
        constexpr float unitsPerMeter = 69.99125109f;
        for (const float altitude : { 1200.f, 90000.f, 260000.f })
        {
            inputs.eyePosition = { 0.f, 0.f, altitude };
            inputs.cloudDensitySliceExtent.x() = 8000.f * unitsPerMeter;
            const PassImage nearby = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.cloudDensitySliceExtent.x() = 11000.f * unitsPerMeter;
            const PassImage fading = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.cloudDensitySliceExtent.x() = 13000.f * unitsPerMeter;
            const PassImage distant = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(nearby.pixels.size(), 1u);
            ASSERT_EQ(fading.pixels.size(), 1u);
            ASSERT_EQ(distant.pixels.size(), 1u);
            EXPECT_FLOAT_EQ(nearby.pixels.front().x(), 1.f);
            EXPECT_NEAR(fading.pixels.front().x(), 0.5f, 0.001f);
            EXPECT_FLOAT_EQ(distant.pixels.front().x(), 0.f);
        }
    }

    /// Stop a spherical-shell traversal at the observer-independent presentation range.
    TEST(AtmosphereShaderTest, sphericalCloudIntersectionStopsAtPresentationRange)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.sunDirection = { 1.f, 0.f, 0.f, 0.f };
        inputs.cloudParameters = { 1.f, 1.f, 1800.f, 100000.f };
        inputs.cloudDensitySliceExtent = { 1.f, 1.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 2;
        inputs.renderCloudDensitySlice = true;
        const PassImage geometry = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(geometry.pixels.size(), 1u);
        const osg::Vec4f interval = geometry.pixels.front();
        constexpr double unitsPerMeter = 69.99125109f;
        const double radius = static_cast<double>(6360000.f * 69.99125109f);
        const double entry = std::sqrt((1800.0 - 1200.0) * (2.0 * radius + 1800.0 + 1200.0));
        ASSERT_GT(entry, 9000.0 * unitsPerMeter);
        ASSERT_LT(entry, 13000.0 * unitsPerMeter);
        EXPECT_NEAR(interval.y(), entry, 0.5);
        EXPECT_NEAR(interval.z(), 13000.0 * unitsPerMeter, 0.5);

        inputs.cloudParameters.z() = 3000.f;
        const PassImage outside = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(outside.pixels.size(), 1u);
        EXPECT_GT(outside.pixels.front().y(), outside.pixels.front().z())
            << "Shells whose entry exceeds the render distance must remain rejected";
    }

    /// Fade distant ground-level shell approaches while retaining nearby cloud transport and occlusion.
    TEST(AtmosphereShaderTest, groundCloudTransportUsesMorrowindPresentationRange)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = 32;
        inputs.height = 16;
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.verticalFov = 0.1f;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        const PassImage distant = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(distant.pixels.size(), 512u);
        for (const osg::Vec4f& pixel : distant.pixels)
            EXPECT_FLOAT_EQ(pixel.w(), 1.f)
                << "The virtual geometric horizon must not retain repeated distant cloud support";

        inputs.viewDirection = { 1.f, 0.f, 0.08f };
        inputs.verticalFov = 0.02f;
        PassImage metadata;
        const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
        ASSERT_EQ(metadata.pixels.size(), volume.pixels.size());
        std::size_t occupied = 0;
        for (std::size_t index = 0; index < volume.pixels.size(); ++index)
        {
            if (volume.pixels[index].w() >= 0.9f)
                continue;
            ++occupied;
            EXPECT_LT(metadata.pixels[index].x(), encodeAtmosphereDistance(13000.f * 69.99125109f));
        }
        EXPECT_GT(occupied, volume.pixels.size() / 4)
            << "Nearby elevated clouds must retain actual transported support";
        RecordProperty("nearby_occupied_pixels", static_cast<int>(occupied));

        inputs.depth = [](int, int) { return 0.5f; };
        const PassImage occluded = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(occluded.pixels.size(), volume.pixels.size());
        for (const osg::Vec4f& pixel : occluded.pixels)
            EXPECT_EQ(pixel, osg::Vec4f(0.f, 0.f, 0.f, 1.f))
                << "Distant clouds must not survive an opaque scene termination before shell entry";
    }

    /// Hide the finite render boundary even when a dense cloud stays optically thick throughout the fade region.
    TEST(AtmosphereShaderTest, distantOpaqueCloudsConvergeToTheAtmosphericSky)
    {
        NativePassInputs inputs;
        inputs.width = 8;
        inputs.height = 4;
        inputs.verticalFov = .02f;
        inputs.farPlane = 1000000.f;
        inputs.fogNear = 20000.f;
        inputs.fogFar = 200000.f;
        inputs.eyePosition = { 0.f, 0.f, 8000.f };
        inputs.cloudParameters = { 1.f, 1.f, 60000.f, 160000.f };
        inputs.cloudModel.mCoverageMinimum = 1.f;
        inputs.cloudModel.mTypeMinimum = 1.f;
        inputs.cloudModel.mTypeAmount = 0.f;
        inputs.cloudModel.mGradientLarge = { .01f, .02f, .7f, .95f };
        inputs.cloudModel.mAnvilLarge = {};
        inputs.cloudModel.mAlbedo = {};
        inputs.mistParameters = {};
        for (const bool exponential : { false, true })
        {
            inputs.exponentialFog = exponential;
            for (const float elevation : { .09f, .064f })
            {
                SCOPED_TRACE(::testing::Message() << "exponential=" << exponential << " elevation=" << elevation);
                inputs.viewDirection = { 1.f, 0.f, elevation };
                inputs.renderCloudSkyReference = true;
                const PassImage sky = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                inputs.renderCloudSkyReference = false;
                const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(sky.pixels.size(), 32u);
                ASSERT_EQ(clouds.pixels.size(), sky.pixels.size());
                double contrast = 0.0;
                for (std::size_t i = 0; i < sky.pixels.size(); ++i)
                {
                    const float background = sky.pixels[i].y();
                    ASSERT_GT(background, .01f);
                    const float composite = clouds.pixels[i].y() + background * clouds.pixels[i].w();
                    contrast += std::abs(composite - background) / background;
                }
                contrast /= sky.pixels.size();
                if (elevation > .08f)
                    EXPECT_GT(contrast, .08) << "Nearby clouds must retain their atmospheric contrast";
                else
                    EXPECT_LT(contrast, .035) << "An opaque deck must not expose the radial render boundary";
            }
        }
    }

    /// Apply the same practical cloud range while flying within or above the main shell.
    TEST(AtmosphereShaderTest, flyingCloudTransportUsesMorrowindPresentationRange)
    {
        NativePassInputs inputs;
        inputs.cloudModel.mCoverageMinimum = .8f;
        inputs.cloudModel.mTypeMinimum = .7f;
        inputs.cloudModel.mTypeAmount = 0.f;
        inputs.width = 32;
        inputs.height = 16;
        inputs.verticalFov = 0.04f;
        inputs.farPlane = 3000000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;
        inputs.cloudParameters = { 1.f, 1.2f, 16000.f, 100000.f };
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};

        for (const auto& [eye, direction] :
            std::array{ std::pair{ osg::Vec3f(0.f, 0.f, 60000.f), osg::Vec3f(1.f, 0.f, 0.f) },
                std::pair{ osg::Vec3f(0.f, 0.f, 180000.f), osg::Vec3f(1.f, 0.f, -0.16f) } })
        {
            inputs.eyePosition = eye;
            inputs.viewDirection = direction;
            PassImage metadata;
            const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
            ASSERT_EQ(metadata.pixels.size(), volume.pixels.size());
            std::size_t occupied = 0;
            for (std::size_t index = 0; index < volume.pixels.size(); ++index)
            {
                if (volume.pixels[index].w() >= 0.98f)
                    continue;
                ++occupied;
                EXPECT_LT(metadata.pixels[index].x(), encodeAtmosphereDistance(13000.f * 69.99125109f));
            }
            EXPECT_GT(occupied, 0u) << "Nearby clouds must remain visible from every flight altitude";
        }
    }

    /// Enclose only reachable type gradients so empty authoring space cannot extend the physical deck.
    TEST(AtmosphereShaderTest, cloudDensityBoundsFollowReachableWeatherTypes)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudDensitySliceQuantity = 7;
        inputs.renderCloudDensitySlice = true;
        for (const auto& model :
            { SceneUtil::CloudLayer{}, SceneUtil::secondaryCloudLayer(), SceneUtil::fairWeatherCloudLayer() })
        {
            inputs.cloudModel = model;
            for (float type : { 0.f, 0.5f, 1.f })
            {
                inputs.cloudModel.mTypeAmount = 0.f;
                inputs.cloudModel.mTypeMinimum = type;
                const PassImage bounds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(bounds.pixels.size(), 1u);
                const auto gradient = type == 0.f ? model.mGradientSmall
                    : type == .5f                 ? model.mGradientMedium
                                                  : model.mGradientLarge;
                EXPECT_NEAR(bounds.pixels.front().x(), 0.16f + gradient.x(), 0.001f);
                EXPECT_NEAR(bounds.pixels.front().y(), 0.16f + gradient.w(), 0.001f);
            }
        }
    }

    /// Supply the occupied cloud top to atmospheric recovery instead of the empty authoring shell.
    TEST(AtmosphereShaderTest, atmosphereCloudTopFollowsOccupiedDensity)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudModel = SceneUtil::fairWeatherCloudLayer();
        inputs.cloudDensitySliceQuantity = 25;
        inputs.renderCloudDensitySlice = true;
        inputs.incomingCloudParameters = { 1.f, 1.f, 40000.f, 200000.f };
        inputs.incomingCloudModel = SceneUtil::fairWeatherCloudLayer();
        for (float progress : { 0.f, .001f, .25f, .5f, .999f, 1.f })
        {
            inputs.cloudScenarioParameters = { 0.f, 0.f, progress, 1.f };
            const PassImage height = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(height.pixels.size(), 1u);
            EXPECT_NEAR(height.pixels.front().x(), .30f + (.68f - .30f) * progress, .001f);
        }
    }

    /// Preserve the source distance allocation under physical scaling and retain a sample in thin strata.
    TEST(AtmosphereShaderTest, cloudMarchAllocationMatchesPhysicalScale)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.quality = 3;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 24;
        for (float scale : { 0.1f, 0.35f, 1.f })
        {
            inputs.cloudModel.mPhysicalScale = scale;
            inputs.cloudModel.mNoiseScalePerMeter = .0006f / scale;
            for (float intervalMeters : { 1.f, 1000.f, 15000.f, 30000.f })
            {
                inputs.cloudDensitySliceExtent.x() = intervalMeters * scale * 69.99125109f;
                const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(result.pixels.size(), 1u);
                const float expectedSteps = std::max(12.f, 96.f * std::min(intervalMeters / 15000.f, 1.f));
                EXPECT_NEAR(result.pixels.front().x(), 1.f / expectedSteps, 0.0001f);
            }
        }
    }

    /// Keep cloud surface shadows below their occupied layer and continuous when a receiver enters it.
    TEST(AtmosphereShaderTest, surfaceCloudColumnClearsAboveItsTypedHeightSupport)
    {
        NativePassInputs inputs;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 27;
        inputs.cloudDensitySliceExtent = { 0.f, 0.f };
        inputs.sunDirection = { 0.f, 0.f, 1.f, 0.f };
        inputs.cloudParameters = { 1.f, 1.f, 10000.f, 10000.f };
        inputs.cloudModel.mCoverageAmount = 0.f;
        inputs.cloudModel.mCoverageMinimum = 1.f;
        inputs.cloudModel.mTypeAmount = 0.f;
        inputs.cloudModel.mTypeMinimum = 0.f;
        inputs.cloudModel.mGradientSmall = { .1f, .2f, .3f, .4f };
        inputs.cloudModel.mAnvilSmall = {};
        float previous = 0.f;
        for (const float altitude : { 10000.f, 11000.f, 12500.f, 13999.f, 14000.f, 20000.f })
        {
            SCOPED_TRACE(altitude);
            inputs.eyePosition.z() = altitude;
            const auto image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_FALSE(image.pixels.empty());
            const float transmittance = image.at(0, 0).x();
            EXPECT_GE(transmittance, previous);
            if (altitude < 14000.f)
                EXPECT_LT(transmittance, 1.f);
            else
                EXPECT_FLOAT_EQ(transmittance, 1.f);
            previous = transmittance;
        }
    }

    /// Preserve fixed nonlinear light-ray offsets and the originating view sample's mip selection.
    TEST(AtmosphereShaderTest, cloudSunShadowMatchesUnculledNonlinearSampling)
    {
        constexpr float unitsPerMeter = 69.99125109f;
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 64;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 23;
        inputs.cloudModel.mCoverageMinimum = 0.8f;
        inputs.cloudModel.mExtinctionPerMeter = { .00071f, .00086f, .001f };
        inputs.cloudModel.mTypeMinimum = 0.7f;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        for (float scale : { 0.35f, 1.f })
        {
            inputs.cloudModel.mPhysicalScale = scale;
            inputs.cloudModel.mNoiseScalePerMeter = .0006f / scale;
            inputs.cloudParameters = { 1.f, 1.f, 1500.f * scale * unitsPerMeter, 5000.f * scale * unitsPerMeter };
            inputs.cloudDensitySliceExtent = { 12000.f * scale * unitsPerMeter, 6000.f * scale * unitsPerMeter };
            inputs.eyePosition = { 43333.f, 236667.f, 4000.f * scale * unitsPerMeter };
            for (const auto& direction : { osg::Vec4f(1.f, 0.f, .1f, 0.f), osg::Vec4f(.3f, .2f, 1.f, 0.f) })
            {
                SCOPED_TRACE(testing::Message() << "scale=" << scale << " sun-z=" << direction.z());
                inputs.sunDirection = direction;
                const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                float maximumError = 0.f;
                std::size_t shadowed = 0;
                for (const auto& pixel : result.pixels)
                {
                    maximumError = std::max(maximumError, pixel.z() / 1000.f);
                    shadowed += pixel.y() < 0.99f ? 1 : 0;
                }
                EXPECT_GT(shadowed, 0u);
                EXPECT_LT(maximumError, 0.0001f);
            }
        }
    }

    /// Retain radiance, opacity, and depth moments across separate and overlapping cloud intervals with dense mist.
    TEST(AtmosphereShaderTest, intervalCloudMarchPreservesPerSampleTransport)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.farPlane = 229376.f;
        inputs.fogFar = 229376.f;
        inputs.verticalFov = 75.f;
        inputs.lowCloudParameters = { 0.5f, 0.25f, 5948.21f, 6589.7f };
        inputs.cloudModel.mCoverageMinimum = 0.6f;
        inputs.environmentalCloudModel.mCoverageMinimum = 0.6f;
        inputs.sunDirection = { -0.924063f, -0.230443f, 0.304965f, 0.f };
        for (const int quality : { 0, 1, 2, 3 })
        {
            inputs.quality = quality;
            for (const bool temporalSampling : { false, true })
            {
                SCOPED_TRACE(testing::Message() << "temporal=" << temporalSampling);
                inputs.historyValid = temporalSampling;
                inputs.reverseZ = temporalSampling;
                inputs.depth = [reverseZ = inputs.reverseZ](int, int) { return reverseZ ? 0.f : 1.f; };
                inputs.previousDepth = inputs.depth;
                inputs.useUniformBuffer = temporalSampling;
                inputs.incomingCloudModel.mCoverageMinimum = 0.65f;
                inputs.incomingCloudParameters
                    = temporalSampling ? osg::Vec4f(0.35f, 0.12f, 98000.f, 94000.f) : osg::Vec4f();
                inputs.cloudScenarioParameters = temporalSampling ? osg::Vec4f(0.f, 0.f, 0.45f, 1.f) : osg::Vec4f();
                for (const float cloudBase : { 4000.f, 66000.f })
                {
                    inputs.cloudParameters = { 0.5f, 0.2f, cloudBase, 124150.f };
                    for (const float altitude : { 1534.89f, 10000.f, 210000.f })
                    {
                        SCOPED_TRACE(testing::Message()
                            << "quality=" << quality << " base=" << cloudBase << " altitude=" << altitude);
                        inputs.eyePosition = { 19962.8f, -97319.7f, altitude };
                        inputs.viewDirection = { 0.97f, 0.f, altitude < 66000.f ? 0.2f : -0.2f };
                        inputs.usePerSampleCloudIntervalReference = true;
                        PassImage referenceMetadata;
                        const PassImage reference
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &referenceMetadata);
                        inputs.usePerSampleCloudIntervalReference = false;
                        PassImage metadata;
                        const PassImage actual
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
                        ASSERT_EQ(actual.pixels.size(), reference.pixels.size());
                        ASSERT_EQ(metadata.pixels.size(), referenceMetadata.pixels.size());
                        float maximumError = 0.f;
                        float maximumMetadataError = 0.f;
                        float maximumOpacity = 0.f;
                        for (std::size_t index = 0; index < actual.pixels.size(); ++index)
                        {
                            maximumOpacity = std::max(maximumOpacity, 1.f - actual.pixels[index].a());
                            for (int channel = 0; channel < 4; ++channel)
                            {
                                ASSERT_TRUE(std::isfinite(actual.pixels[index][channel]));
                                ASSERT_TRUE(std::isfinite(metadata.pixels[index][channel]));
                                maximumError = std::max(maximumError,
                                    std::abs(actual.pixels[index][channel] - reference.pixels[index][channel]));
                                maximumMetadataError = std::max(maximumMetadataError,
                                    std::abs(
                                        metadata.pixels[index][channel] - referenceMetadata.pixels[index][channel]));
                            }
                        }
                        EXPECT_GT(maximumOpacity, 0.001f);
                        EXPECT_LE(maximumError, 1e-5f);
                        EXPECT_LE(maximumMetadataError, 1e-5f);
                    }
                }
            }
        }
    }

    /// Finish separated strata without replaying or stalling at a rounded interval endpoint.
    TEST(AtmosphereShaderTest, separatedCloudStrataMakeForwardProgress)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 64;
        inputs.measureCloudMarchProgress = true;
        inputs.farPlane = 229376.f;
        inputs.fogFar = 229376.f;
        inputs.verticalFov = 75.f;
        inputs.mistParameters = {};
        inputs.cloudParameters = { 0.5f, 0.04f, 66000.f, 124150.f };
        inputs.lowCloudParameters = { 0.25f, 0.04f, 5948.21f, 6589.7f };
        inputs.cloudModel.mCoverageMinimum = 0.6f;
        inputs.environmentalCloudModel.mCoverageMinimum = 0.6f;
        inputs.sunDirection = { -0.924063f, -0.230443f, 0.304965f, 0.f };
        for (const int quality : { 0, 1, 2, 3 })
        {
            inputs.quality = quality;
            for (const float altitude : { 1534.89f, 210000.f })
            {
                SCOPED_TRACE(testing::Message() << "quality=" << quality << " altitude=" << altitude);
                inputs.eyePosition = { 19962.8f, -97319.7f, altitude };
                inputs.viewDirection = { 0.97f, 0.f, altitude < 66000.f ? 0.2f : -0.2f };
                const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                std::size_t sampledRays = 0;
                std::size_t repeatedRays = 0;
                float maximumRepeatedSteps = 0.f;
                for (const auto& pixel : result.pixels)
                {
                    ASSERT_TRUE(std::isfinite(pixel.x()));
                    ASSERT_TRUE(std::isfinite(pixel.y()));
                    sampledRays += pixel.x() > 0.f ? 1 : 0;
                    repeatedRays += pixel.y() > 0.f ? 1 : 0;
                    maximumRepeatedSteps = std::max(maximumRepeatedSteps, pixel.y());
                }
                EXPECT_GT(sampledRays, 100u);
                EXPECT_EQ(repeatedRays, 0u) << "Maximum repeated/backward steps: " << maximumRepeatedSteps;

                inputs.useLegacyCloudMarchReference = true;
                const PassImage legacyProgress = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                inputs.measureCloudMarchProgress = false;
                PassImage legacyMetadata;
                const PassImage legacy
                    = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &legacyMetadata);
                inputs.useLegacyCloudMarchReference = false;
                PassImage metadata;
                const PassImage actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
                inputs.measureCloudMarchProgress = true;
                ASSERT_EQ(actual.pixels.size(), legacy.pixels.size());
                ASSERT_EQ(actual.pixels.size(), legacyProgress.pixels.size());
                ASSERT_EQ(metadata.pixels.size(), legacyMetadata.pixels.size());
                std::size_t unchangedRays = 0;
                float maximumError = 0.f;
                float maximumMetadataError = 0.f;
                float maximumOpacity = 0.f;
                for (std::size_t index = 0; index < actual.pixels.size(); ++index)
                {
                    if (legacyProgress.pixels[index].x() == 0.f || legacyProgress.pixels[index].y() > 0.f)
                        continue;
                    ++unchangedRays;
                    maximumOpacity = std::max(maximumOpacity, 1.f - actual.pixels[index].a());
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        ASSERT_TRUE(std::isfinite(actual.pixels[index][channel]));
                        ASSERT_TRUE(std::isfinite(metadata.pixels[index][channel]));
                        maximumError = std::max(
                            maximumError, std::abs(actual.pixels[index][channel] - legacy.pixels[index][channel]));
                        maximumMetadataError = std::max(maximumMetadataError,
                            std::abs(metadata.pixels[index][channel] - legacyMetadata.pixels[index][channel]));
                    }
                }
                EXPECT_GT(unchangedRays, 100u);
                EXPECT_GT(maximumOpacity, 0.001f);
                EXPECT_LE(maximumError, 1e-5f);
                EXPECT_LE(maximumMetadataError, 1e-5f);
            }
        }
    }

    /// Preserve spectral attenuation and mip advancement for rising and setting sunlight across terrain-cloud gaps.
    TEST(AtmosphereShaderTest, lowCloudSunShadowPreservesSamplingAcrossTerrainAndMips)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 32;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 36;
        inputs.lowCloudParameters = { 1.f, 0.9f, 2600.f, 9000.f };
        inputs.environmentalCloudModel.mCoverageAmount = 0.6f;
        // Retain shadowed support at the coarsest tested mip; the terrain profile supplies empty gaps.
        inputs.environmentalCloudModel.mCoverageMinimum = 0.6f;
        inputs.environmentalCloudModel.mExtinctionPerMeter = { 0.00071f, 0.00086f, 0.001f };
        inputs.cloudDensitySliceExtent = { 60000.f, 18000.f };
        inputs.mediumReferenceParameters = { 16000.f, 1.f, 2600.f, 0.f };
        const float cellCenter = 0.5f * SceneUtil::AtmosphereTerrainGrid::sSpacingWorldUnits;
        inputs.orographicParameters = { cellCenter, cellCenter, 0.08f, 0.f };
        std::size_t resumedLightRays = 0;
        for (const float influence : { 0.f, 0.65f })
        {
            inputs.lowCloudEnvironmentInfluence = influence;
            inputs.eyePosition = { cellCenter, cellCenter, 2600.f + 16000.f * influence + 4500.f };
            for (const float scale : { 0.04f, 0.35f })
            {
                inputs.environmentalCloudModel.mPhysicalScale = scale;
                inputs.environmentalCloudModel.mNoiseScalePerMeter = 0.0006f / scale;
                for (const int quality : { 0, 1, 2, 3 })
                {
                    inputs.quality = quality;
                    for (const auto& direction : { osg::Vec4f(1.f, 0.f, 0.08f, 0.f), osg::Vec4f(0.2f, -0.5f, 0.8f, 0.f),
                             osg::Vec4f(0.2f, -0.5f, -0.8f, 0.f) })
                    {
                        SCOPED_TRACE(testing::Message() << "influence=" << influence << " scale=" << scale
                                                        << " quality=" << quality << " sun-z=" << direction.z());
                        inputs.sunDirection = direction;
                        const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                        ASSERT_FALSE(result.pixels.empty());
                        float maximumError = 0.f;
                        std::size_t shadowed = 0;
                        for (const auto& pixel : result.pixels)
                        {
                            maximumError = std::max(maximumError, pixel.z() / 1000.f);
                            shadowed += pixel.y() < 0.99f ? 1 : 0;
                            resumedLightRays += pixel.a() > 0.5f ? 1 : 0;
                        }
                        EXPECT_GT(shadowed, 0u);
                        EXPECT_LT(maximumError, 0.0001f);
                    }
                }
            }
        }
        EXPECT_GT(resumedLightRays, 0u) << "Exercise mip selection after a rejected light sample";
    }

    /// Compare the GLSL density model with independently computed GPU reference samples.
    TEST(AtmosphereShaderTest, cloudDensityMatchesReferenceGpuSamples)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 64;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 26;
        inputs.cloudModel.mNoiseScalePerMeter = .0002f;
        inputs.cloudModel.mCoverageAmount = .3f;
        inputs.cloudModel.mCoverageMinimum = .7f;
        inputs.cloudModel.mTypeAmount = 0.f;
        inputs.cloudModel.mTypeMinimum = 1.f;
        inputs.cloudModel.mAnvilLarge = { 0.f, 0.f, 0.f, 0.f };
        inputs.cloudModel.mGradientLarge = { .01f, .06f, .1f, .95f };
        inputs.cloudModel.mWindSpeedMetersPerSecond = 0.f;
        inputs.cloudModel.mCoverageWindSpeedMetersPerSecond = 0.f;
        // Reference samples use
        // canonical RGBA8 textures, mip zero, Y-up metres, h=.5, p=((x-32)*125,4000,(y-32)*125).
        // Include empty, eroded, and occupied points; half-float readback bounds the comparison precision.
        const std::array<std::array<float, 5>, 32> expected{ {
            { 0, 0, 0.000000000f, 0.022985181f, 0.699999988f },
            { 1, 4, 0.000000000f, 0.000000000f, 0.699999988f },
            { 2, 8, 0.000000000f, 0.000000000f, 0.699999988f },
            { 3, 12, 0.000000000f, 0.041061535f, 0.699999988f },
            { 4, 16, 0.000000000f, 0.097016931f, 0.713816881f },
            { 5, 20, 0.000000000f, 0.132288009f, 0.736803353f },
            { 30, 22, 0.076937057f, 0.251696825f, 0.776082873f },
            { 31, 22, 0.077577859f, 0.247621089f, 0.772281528f },
            { 29, 23, 0.065933518f, 0.246847719f, 0.783307850f },
            { 31, 23, 0.079244636f, 0.258746088f, 0.776552498f },
            { 6, 24, 0.000000000f, 0.139271215f, 0.742738843f },
            { 31, 24, 0.088766731f, 0.264436185f, 0.780083299f },
            { 32, 24, 0.069852248f, 0.267240405f, 0.775687337f },
            { 33, 24, 0.062553309f, 0.251873463f, 0.770272374f },
            { 32, 25, 0.054821111f, 0.254185379f, 0.778396904f },
            { 7, 28, 0.000000000f, 0.117188893f, 0.747384727f },
            { 43, 28, 0.054858215f, 0.253588408f, 0.774948478f },
            { 41, 29, 0.057639182f, 0.232234225f, 0.768117666f },
            { 42, 29, 0.097973198f, 0.259771287f, 0.769201159f },
            { 43, 29, 0.094200991f, 0.272938728f, 0.775776625f },
            { 62, 29, 0.057060733f, 0.224463746f, 0.741894245f },
            { 63, 29, 0.055944804f, 0.248730645f, 0.734016955f },
            { 42, 30, 0.076836690f, 0.263178229f, 0.769099534f },
            { 43, 30, 0.059801631f, 0.247351736f, 0.775823295f },
            { 8, 32, 0.000000000f, 0.172714069f, 0.742179871f },
            { 9, 36, 0.000000000f, 0.201772124f, 0.738047600f },
            { 10, 40, 0.000000000f, 0.171195403f, 0.714936137f },
            { 11, 44, 0.000000000f, 0.112512536f, 0.721188843f },
            { 12, 48, 0.000000000f, 0.090060718f, 0.699999988f },
            { 13, 52, 0.000000000f, 0.070003957f, 0.699999988f },
            { 14, 56, 0.000000000f, 0.083389409f, 0.699999988f },
            { 15, 60, 0.000000000f, 0.139649734f, 0.699999988f },
        } };
        const PassImage actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        for (const auto& sample : expected)
        {
            SCOPED_TRACE(testing::Message() << "x=" << sample[0] << " z=" << sample[1]);
            const osg::Vec4f& pixel = actual.at(static_cast<int>(sample[0]), static_cast<int>(sample[1]));
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(pixel[channel], sample[channel + 2], .0005f);
        }
    }

    /// Compare textured density against the independent evaluator across types, mips, and translated fields.
    TEST(AtmosphereShaderTest, cloudDensityMatchesPrimaryLayerReference)
    {
        NativePassInputs inputs;
        inputs.width = 256;
        inputs.height = 128;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceAxis = 1;
        constexpr float unitsPerMeter = 69.99125109f;
        inputs.cloudDensitySliceExtent = { 100000.f * unitsPerMeter, 5000.f * unitsPerMeter };
        inputs.eyePosition = { 43333.f, 236667.f, 4000.f * unitsPerMeter };
        inputs.cloudParameters = { (1.f - 0.65f) / 1.2f, 1.f, 1500.f * unitsPerMeter, 5000.f * unitsPerMeter };
        for (float type : { 0.f, 0.3f, 0.5f, 0.7f, 1.f })
        {
            inputs.cloudModel.mTypeAmount = 0.f;
            inputs.cloudModel.mTypeMinimum = type;
            for (int mip : { 0, 1, 2 })
            {
                inputs.cloudDensitySliceQuantity = 19 + mip;
                for (float phase : { 0.f, 1.f, 3.7f })
                {
                    SCOPED_TRACE(testing::Message() << "type=" << type << " mip=" << mip << " phase=" << phase);
                    inputs.cloudScenarioParameters.x() = phase;
                    const PassImage density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_EQ(density.pixels.size(), static_cast<std::size_t>(inputs.width * inputs.height));
                    std::size_t occupiedReference = 0;
                    std::size_t mismatches = 0;
                    float maximumError = 0.f;
                    for (const osg::Vec4f& pixel : density.pixels)
                    {
                        occupiedReference += pixel.y() > 0.001f ? 1 : 0;
                        maximumError = std::max(maximumError, pixel.z() / 1000.f);
                        // Independently grouped float operations and texture interpolation may differ at
                        // large translated coordinates; require agreement within 0.01% of unit density.
                        mismatches += pixel.z() > 0.1f ? 1 : 0;
                    }
                    EXPECT_GT(occupiedReference, 0u) << "An empty comparison cannot qualify cloud density";
                    EXPECT_EQ(mismatches, 0u) << "Maximum source-reference density error: " << maximumError;
                }
            }
        }
    }

    /// Match terrain, secondary, and custom source profiles, including independent weather and wind domains.
    TEST(AtmosphereShaderTest, cloudDensityMatchesSecondaryAndAuthoredLayers)
    {
        NativePassInputs inputs;
        inputs.width = 256;
        inputs.height = 128;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceAxis = 1;
        constexpr float unitsPerMeter = 69.99125109f;
        inputs.cloudDensitySliceExtent = { 100000.f * unitsPerMeter, 5000.f * unitsPerMeter };
        inputs.eyePosition = { 43333.f, 236667.f, 4000.f * unitsPerMeter };
        inputs.cloudParameters = { 1.f, 1.f, 1500.f * unitsPerMeter, 5000.f * unitsPerMeter };
        for (int profile : { 0, 1, 2 })
        {
            inputs.cloudModel = profile == 2 ? SceneUtil::fairWeatherCloudLayer()
                : profile == 1               ? SceneUtil::secondaryCloudLayer()
                                             : SceneUtil::CloudLayer{};
            if (profile != 2)
            {
                inputs.cloudModel.mCoverageAmount = 1.7f;
                inputs.cloudModel.mCoverageMinimum = .45f;
                inputs.cloudModel.mTypeAmount = 2.2f;
                inputs.cloudModel.mTypeMinimum = .25f;
                inputs.cloudModel.mRainAmount = .8f;
                inputs.cloudModel.mRainMinimum = .1f;
            }
            if (profile == 0)
            {
                inputs.cloudModel.mGradientMedium = { .03f, .12f, .48f, .63f };
                inputs.cloudModel.mAnvilMedium = { 7.f, .2f, 11.f, .15f };
                inputs.cloudModel.mCurlHeightExponent = 3.f;
                inputs.cloudModel.mCurlDisplacementMeters = 220.f;
                inputs.cloudModel.mDetailHeightFraction = 4.f;
                inputs.cloudModel.mDetailErosion = .55f;
            }
            for (float motion : { 0.f, 17000.f })
            {
                inputs.cloudAdvectionOffsetWorldUnits = { motion, motion * .4f };
                for (int mip : { 0, 2 })
                {
                    SCOPED_TRACE(testing::Message() << "profile=" << profile << " motion=" << motion << " mip=" << mip);
                    inputs.cloudDensitySliceQuantity = 19 + mip;
                    const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_EQ(image.pixels.size(), static_cast<std::size_t>(inputs.width * inputs.height));
                    std::size_t occupied = 0;
                    float maximumError = 0.f;
                    for (const auto& pixel : image.pixels)
                    {
                        occupied += pixel.y() > .001f;
                        maximumError = std::max(maximumError, pixel.z() / 1000.f);
                        ASSERT_TRUE(std::isfinite(pixel.x()));
                    }
                    EXPECT_GT(occupied, 0u);
                    EXPECT_LE(maximumError, .0001f);
                }
            }
        }
    }

    /// Publish the cloud array and subsequent camera uniforms identically through both OpenMW graphics bindings.
    TEST(AtmosphereShaderTest, cloudLayerPublicationMatchesUniformAndBufferPaths)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 64;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceExtent = { 300000.f, 180000.f };
        inputs.eyePosition = { 43333.f, 236667.f, 120000.f };
        inputs.cloudParameters = { 1.f, 1.f, 30000.f, 180000.f };
        inputs.cloudModel.mCoverageMinimum = .8f;
        inputs.cloudModel.mTypeMinimum = .7f;
        inputs.useUniformBuffer = false;
        const PassImage uniforms = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.useUniformBuffer = true;
        const PassImage buffer = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(uniforms.pixels.size(), buffer.pixels.size());
        ASSERT_EQ(uniforms.pixels.size(), 4096u);
        std::size_t occupied = 0;
        float maximumError = 0.f;
        for (std::size_t index = 0; index < uniforms.pixels.size(); ++index)
        {
            occupied += uniforms.pixels[index].x() > .001f;
            maximumError = std::max(maximumError, std::abs(uniforms.pixels[index].x() - buffer.pixels[index].x()));
        }
        EXPECT_GT(occupied, 0u);
        EXPECT_LE(maximumError, .0001f);
    }

    /// Keep fixed cloud points continuous as observer distance crosses a texture-detail boundary.
    TEST(AtmosphereShaderTest, cloudDensityDoesNotJumpAtDistanceMipBoundaries)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 64;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 40;
        inputs.cloudParameters = { 1.f, 1.f, 0.f, 70000.f };
        inputs.cloudDensitySliceExtent = { 800000.f, 800000.f };
        inputs.eyePosition = { 43333.f, 236667.f, 21000.f };
        inputs.cloudModel.mCoverageMinimum = .6f;
        inputs.cloudModel.mCoverageAmount = .4f;
        inputs.cloudModel.mTypeAmount = 0.f;
        inputs.cloudModel.mTypeMinimum = 1.f;
        inputs.cloudModel.mAnvilLarge = {};
        for (const float scale : { .18f, .5f })
        {
            inputs.cloudModel.mPhysicalScale = scale;
            const auto density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            float maximumChange = 0.f;
            float minimumDensity = 1.f;
            float maximumDensity = 0.f;
            for (const auto& sample : density.pixels)
            {
                maximumChange = std::max(maximumChange, sample.z());
                minimumDensity = std::min(minimumDensity, sample.x());
                maximumDensity = std::max(maximumDensity, sample.x());
            }
            EXPECT_GT(maximumDensity - minimumDensity, .1f);
            EXPECT_LT(maximumChange, .001f) << "A two-world-unit observer movement must not reshape the cloud";
            RecordProperty("maximum_mip_boundary_density_jump", std::to_string(maximumChange));
        }
    }

    /// Preserve physical distance scaling while continuously selecting the cloud texture footprint.
    TEST(AtmosphereShaderTest, scaledCloudsRetainSourceNoiseSamplingFootprint)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 22;
        inputs.fullPrecisionCapture = true;
        for (float featureScale : { 0.25f, 0.5f, 1.f })
        {
            inputs.cloudModel.mPhysicalScale = featureScale;
            inputs.cloudModel.mNoiseScalePerMeter = .0006f / featureScale;
            for (float distanceRatio : { 0.f, .1f, .49f, .51f, 1.f, 1.49f, 1.51f, 2.49f, 2.51f, 4.f })
            {
                inputs.cloudDensitySliceExtent.x() = 30000.f * 69.99125109f * featureScale * distanceRatio;
                for (int shadowStep : { 0, 1, 5 })
                {
                    inputs.cloudDensitySliceAxis = shadowStep;
                    const PassImage lod = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_EQ(lod.pixels.size(), 1u);
                    const float expected = std::min(distanceRatio, 3.f) + static_cast<float>(shadowStep) * .5f;
                    EXPECT_NEAR(lod.pixels.front().x(), expected, .001f)
                        << "scale=" << featureScale << " distance=" << distanceRatio << " shadow=" << shadowStep;
                }
            }
        }
    }

    /// Keep source noise-frequency authoring independent from physical sampling distances.
    TEST(AtmosphereShaderTest, cloudNoiseFrequencyDoesNotRescaleTransport)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 22;
        inputs.cloudDensitySliceExtent.x() = 30000.f * 69.99125109f;
        for (const auto& model : { SceneUtil::CloudLayer{}, SceneUtil::fairWeatherCloudLayer() })
        {
            inputs.cloudModel = model;
            const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(result.pixels.size(), 1u);
            EXPECT_NEAR(result.pixels.front().x(), 1.f, 0.001f);
        }
    }

    /// Keep billow size independent of episode placement and local weather organization.
    TEST(AtmosphereShaderTest, cloudNoiseSpatialFrequencyIsIndependentOfEpisodePhase)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 18;
        inputs.cloudDensitySliceExtent = {};
        inputs.cloudModel = SceneUtil::CloudLayer{};
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        constexpr float expectedDelta = 1000.f * 0.0006f / 69.99125109f;
        for (float phase : { 0.f, 0.2f, 1.f, 2.4f, 5.f })
        {
            inputs.cloudScenarioParameters.x() = phase;
            for (float weatherScale : { 0.1f, 0.3f, 1.f, 3.f })
            {
                inputs.cloudModel.mWeatherScalePerMeter = .00002f / weatherScale;
                inputs.eyePosition = { 43333.f, 236667.f, 65000.f };
                const PassImage coordinates = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(coordinates.pixels.size(), 1u);
                EXPECT_NEAR(coordinates.pixels.front().x(), expectedDelta, 0.00002f);
                EXPECT_NEAR(coordinates.pixels.front().y(), expectedDelta / 3.f, 0.00002f);
                EXPECT_NEAR(coordinates.pixels.front().z(), 0.f, 0.00002f);
            }
        }
    }

    /// Enclose custom source gradients independently of their small, medium, or large labels.
    TEST(AtmosphereShaderTest, cloudDensityBoundsEncloseDisplacedTypeProfiles)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudDensitySliceQuantity = 7;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudModel.mGradientSmall = { .2f, .3f, .8f, .9f };
        inputs.cloudModel.mGradientMedium = { .05f, .1f, .2f, .3f };
        inputs.cloudModel.mGradientLarge = { .1f, .2f, .3f, .4f };
        const PassImage bounds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(bounds.pixels.size(), 1u);
        EXPECT_NEAR(bounds.pixels.front().x(), .21f, .001f);
        EXPECT_NEAR(bounds.pixels.front().y(), 1.06f, .001f);
    }

    /// Preserve local altitude and shell intersections when Earth-scale geometry is evaluated in game units.
    TEST(AtmosphereShaderTest, sphericalCloudGeometryRetainsLocalPrecision)
    {
        NativePassInputs inputs;
        inputs.width = 16;
        inputs.height = 16;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudDensitySliceExtent = { 16.f, 16.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 2;
        inputs.renderCloudDensitySlice = true;
        // Match the shader's representable radius; use double precision only for the independent reference.
        const double planetRadius = static_cast<double>(6360000.f * 69.99125109f);
        const double presentationEndDistance = static_cast<double>(13000.f * 69.99125109f);
        for (const osg::Vec3f eye : { osg::Vec3f(0.f, 0.f, 1200.f), osg::Vec3f(0.f, 0.f, 40000.f),
                 osg::Vec3f(1500000.f, -1000000.f, 180000.f) })
        {
            inputs.eyePosition = eye;
            for (const osg::Vec3f ray :
                { osg::Vec3f(0.f, 0.f, 1.f), osg::Vec3f(0.8f, 0.f, -0.6f), osg::Vec3f(1.f, 0.f, 0.02f) })
            {
                inputs.sunDirection = { ray.x(), ray.y(), ray.z(), 0.f };
                const PassImage geometry = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                osg::Vec3d direction(ray);
                direction.normalize();
                for (int y = 0; y < inputs.height; ++y)
                {
                    for (int x = 0; x < inputs.width; ++x)
                    {
                        const osg::Vec3d position(
                            eye.x() + x + 0.5 - inputs.width * 0.5, eye.y(), eye.z() + y + 0.5 - inputs.height * 0.5);
                        const osg::Vec3d relativeOrigin = position + osg::Vec3d(0.0, 0.0, planetRadius);
                        const double originRadius = relativeOrigin.length();
                        const osg::Vec4f pixel = geometry.at(x, y);
                        EXPECT_NEAR(pixel.x(), originRadius - planetRadius, 0.125);
                        const double projectedOrigin = relativeOrigin * direction;
                        /// Evaluate the unconditioned quadratic in double precision as an independent reference.
                        const auto intersections = [&](double height) {
                            const double radius = planetRadius + height;
                            const double discriminant
                                = projectedOrigin * projectedOrigin - (relativeOrigin.length2() - radius * radius);
                            const double root = std::sqrt(std::max(discriminant, 0.0));
                            return osg::Vec2d(-projectedOrigin - root, -projectedOrigin + root);
                        };
                        const osg::Vec2d upper = intersections(116000.0);
                        if (upper.y() <= 0.0)
                        {
                            EXPECT_GT(pixel.y(), pixel.z());
                            continue;
                        }
                        const osg::Vec2d lower = intersections(16000.0);
                        const double entry = originRadius < planetRadius + 16000.0 ? std::max(lower.y(), 0.0)
                                                                                   : std::max(upper.x(), 0.0);
                        const double exit = lower.x() > entry ? std::min(upper.y(), lower.x()) : upper.y();
                        if (entry > presentationEndDistance)
                        {
                            EXPECT_GT(pixel.y(), pixel.z());
                            continue;
                        }
                        EXPECT_NEAR(pixel.y(), entry, 0.5);
                        EXPECT_NEAR(pixel.z(), std::min(exit, presentationEndDistance), 0.5);
                    }
                }
            }
        }
    }

    /// Keep vertical main-cloud traversal valid from below, within, and above the spherical shell.
    TEST(AtmosphereShaderTest, verticalCloudRaysDoNotClipAtTheObserver)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudDensitySliceExtent = { 1.f, 1.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 2;
        inputs.renderCloudDensitySlice = true;

        const auto intervalFor = [&](float altitude, float verticalDirection) {
            inputs.eyePosition = { 0.f, 0.f, altitude };
            inputs.sunDirection = { 0.f, 0.f, verticalDirection, 0.f };
            const PassImage geometry = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            EXPECT_EQ(geometry.pixels.size(), 1u);
            return geometry.pixels.front();
        };

        const osg::Vec4f below = intervalFor(1200.f, 1.f);
        EXPECT_LT(below.y(), below.z());
        EXPECT_GT(below.y(), 0.f);

        const osg::Vec4f withinUp = intervalFor(54000.f, 1.f);
        const osg::Vec4f withinDown = intervalFor(54000.f, -1.f);
        EXPECT_FLOAT_EQ(withinUp.y(), 0.f);
        EXPECT_FLOAT_EQ(withinDown.y(), 0.f);
        EXPECT_GT(withinUp.z(), 0.f);
        EXPECT_GT(withinDown.z(), 0.f);

        const osg::Vec4f above = intervalFor(180000.f, -1.f);
        EXPECT_LT(above.y(), above.z());
        EXPECT_GT(above.y(), 0.f);
    }

    /// Compare shared view-path extinction against numerical integration of molecular and aerosol density.
    TEST(AtmosphereShaderTest, aerialPerspectiveIntegratesHeightAlongTheViewPath)
    {
        NativePassInputs inputs;
        inputs.width = 4;
        inputs.height = 4;
        inputs.weatherWeight = 0.f;
        inputs.fogColor = { 0.f, 0.f, 0.f, 1.f };
        inputs.fogNear = 0.f;
        inputs.fogFar = 200000.f;
        inputs.cloudDensitySliceQuantity = 3;
        inputs.renderCloudDensitySlice = true;
        constexpr double worldUnitsPerMeter = 69.99125109;
        constexpr double rayleighScaleHeight = 8000.0 * worldUnitsPerMeter;
        constexpr double aerosolScaleHeight = 1200.0 * worldUnitsPerMeter;
        for (const bool exponential : { false, true })
        {
            inputs.cloudDensitySliceAxis = exponential ? 1 : 0;
            const double opticalRange = inputs.fogFar / (exponential ? 4.4 : 1.0);
            const double molecularFraction = std::min(1.0, 0.013558 * opticalRange / (1000.0 * worldUnitsPerMeter));
            for (const auto& [startHeight, endHeight, distance] :
                std::array{ std::tuple{ -100.f, -100.f, 50000.f }, std::tuple{ 0.f, 0.f, 50000.f },
                    std::tuple{ 450000.f, 450000.f, 50000.f }, std::tuple{ 450000.f, 200000.f, 500000.f },
                    std::tuple{ 0.f, 450000.f, 900000.f }, std::tuple{ 450000.f, 0.f, 900000.f } })
            {
                SCOPED_TRACE(::testing::Message() << startHeight << " -> " << endHeight << " distance=" << distance);
                inputs.eyePosition.z() = startHeight;
                inputs.cloudDensitySliceExtent.x() = distance;
                const float verticalDirection = (endHeight - startHeight) / distance;
                inputs.sunDirection
                    = { std::sqrt(1.f - verticalDirection * verticalDirection), 0.f, verticalDirection, 0.f };
                double meanDensity = 0.0;
                constexpr int steps = 4096;
                for (int step = 0; step < steps; ++step)
                {
                    const double height = std::max(0.0, startHeight + (endHeight - startHeight) * (step + 0.5) / steps);
                    meanDensity += molecularFraction * std::exp(-height / rayleighScaleHeight)
                        + (1.0 - molecularFraction) * std::exp(-height / aerosolScaleHeight);
                }
                meanDensity /= steps;
                const double opticalDepth = distance * meanDensity / opticalRange;
                const double expectedVisibility
                    = exponential ? std::exp(-opticalDepth) : 1.0 - std::min(opticalDepth, 1.0);
                const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                for (const osg::Vec4f& pixel : image.pixels)
                    EXPECT_NEAR(pixel.x(), expectedVisibility, 0.002);
            }
        }
    }

    /// Preserve each stratum's canonical fog response when sharing view-path evaluation at the same sample distance.
    TEST(AtmosphereShaderTest, sharedCloudFogPreservesSeparateStratumTransfer)
    {
        float maximumOpacity = 0.f;
        for (const auto& [weather, phase] :
            { std::pair{ "Foggy", "Day" }, std::pair{ "Cloudy", "Sunset" }, std::pair{ "Clear", "Day" } })
            for (const int quality : { 0, 1, 2, 3 })
                for (const bool exponential : { false, true })
                    for (const float altitude : { 9000.f, 90000.f })
                    {
                        SCOPED_TRACE(::testing::Message() << weather << " quality=" << quality << " exponential="
                                                          << exponential << " altitude=" << altitude);
                        NativePassInputs inputs = makePackagedAtmosphereInputs(weather, phase);
                        inputs.fullPrecisionCapture = true;
                        inputs.width = 12;
                        inputs.height = 8;
                        inputs.quality = quality;
                        inputs.useUniformBuffer = quality == 3;
                        inputs.exponentialFog = exponential;
                        inputs.mistParameters = {};
                        inputs.fogVolumeParameters = {};
                        inputs.cloudParameters = { .8f, .7f, 40000.f, 100000.f };
                        inputs.lowCloudParameters = { .8f, .7f, 5000.f, 25000.f };
                        inputs.cloudModel.mCoverageMinimum = .8f;
                        inputs.environmentalCloudModel.mCoverageMinimum = .8f;
                        inputs.mediumReferenceParameters = { 1500.f, .6f, 6000.f, 0.f };
                        inputs.orographicParameters = { 0.f, 0.f, .08f, -.03f };
                        inputs.eyePosition = { 150000.f, -70000.f, altitude };
                        inputs.viewDirection = { .92f, .25f, altitude < 40000.f ? .2f : -.2f };
                        inputs.viewDirection.normalize();
                        inputs.fogNear = 5000.f;
                        inputs.fogFar = 150000.f;
                        inputs.farPlane = 229376.f;
                        PassImage sharedMetadata;
                        const PassImage shared
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &sharedMetadata);
                        inputs.useSeparateCloudFogReference = true;
                        PassImage referenceMetadata;
                        const PassImage reference
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &referenceMetadata);
                        ASSERT_EQ(shared.pixels.size(), reference.pixels.size());
                        ASSERT_FALSE(reference.pixels.empty());
                        for (std::size_t pixel = 0; pixel < reference.pixels.size(); ++pixel)
                        {
                            maximumOpacity = std::max(maximumOpacity, 1.f - reference.pixels[pixel].a());
                            for (int channel = 0; channel < 4; ++channel)
                            {
                                EXPECT_NEAR(shared.pixels[pixel][channel], reference.pixels[pixel][channel], 1e-6f);
                                EXPECT_NEAR(sharedMetadata.pixels[pixel][channel],
                                    referenceMetadata.pixels[pixel][channel], 1e-6f);
                            }
                        }
                    }
        EXPECT_GT(maximumOpacity, .05f);
    }

    /// Preserve mixed-deck skylight and independent condensate radiance when omitting unused terrain height.
    TEST(AtmosphereShaderTest, unusedCloudHeightPreservesStratumLighting)
    {
        std::array<float, 2> maximumOpacity{};
        float maximumMixedHeightContribution = 0.f;
        for (const auto& [weather, phase] :
            { std::pair{ "Foggy", "Day" }, std::pair{ "Cloudy", "Sunset" }, std::pair{ "Clear", "Day" } })
            for (const int quality : { 0, 1, 2, 3 })
                for (const bool mainDeck : { false, true })
                    for (const float altitude : { 9000.f, 22000.f })
                    {
                        SCOPED_TRACE(::testing::Message() << weather << " quality=" << quality
                                                          << " mainDeck=" << mainDeck << " altitude=" << altitude);
                        NativePassInputs inputs = makePackagedAtmosphereInputs(weather, phase);
                        inputs.width = 12;
                        inputs.height = 8;
                        inputs.quality = quality;
                        inputs.useUniformBuffer = quality == 3;
                        inputs.exponentialFog = true;
                        inputs.mistParameters = {};
                        inputs.fogVolumeParameters = {};
                        inputs.cloudParameters = mainDeck ? osg::Vec4f(.8f, .7f, 5000.f, 25000.f) : osg::Vec4f();
                        inputs.lowCloudParameters = { .8f, .7f, 5000.f, 25000.f };
                        inputs.cloudModel.mCoverageMinimum = .8f;
                        inputs.environmentalCloudModel.mCoverageMinimum = .8f;
                        inputs.mediumReferenceParameters = { 1500.f, .6f, 6000.f, 0.f };
                        inputs.orographicParameters = { 0.f, 0.f, .08f, -.03f };
                        inputs.eyePosition = { 150000.f, -70000.f, altitude };
                        inputs.viewDirection = { .92f, .25f, altitude < 40000.f ? .2f : -.2f };
                        inputs.viewDirection.normalize();
                        inputs.fogNear = 5000.f;
                        inputs.fogFar = 150000.f;
                        inputs.farPlane = 229376.f;
                        PassImage sharedMetadata;
                        const PassImage shared
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &sharedMetadata);
                        inputs.cloudHeightReference = CloudHeightReference::Unconditional;
                        PassImage referenceMetadata;
                        const PassImage reference
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &referenceMetadata);
                        if (mainDeck)
                        {
                            inputs.cloudHeightReference = CloudHeightReference::Omitted;
                            const PassImage omitted = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                            ASSERT_EQ(omitted.pixels.size(), reference.pixels.size());
                            for (std::size_t pixel = 0; pixel < reference.pixels.size(); ++pixel)
                                for (int channel = 0; channel < 3; ++channel)
                                    maximumMixedHeightContribution = std::max(maximumMixedHeightContribution,
                                        std::abs(omitted.pixels[pixel][channel] - reference.pixels[pixel][channel]));
                        }
                        ASSERT_EQ(shared.pixels.size(), reference.pixels.size());
                        ASSERT_FALSE(reference.pixels.empty());
                        for (std::size_t pixel = 0; pixel < reference.pixels.size(); ++pixel)
                        {
                            maximumOpacity[mainDeck]
                                = std::max(maximumOpacity[mainDeck], 1.f - reference.pixels[pixel].a());
                            for (int channel = 0; channel < 4; ++channel)
                            {
                                EXPECT_NEAR(shared.pixels[pixel][channel], reference.pixels[pixel][channel], 1e-6f);
                                EXPECT_NEAR(sharedMetadata.pixels[pixel][channel],
                                    referenceMetadata.pixels[pixel][channel], 1e-6f);
                            }
                        }
                    }
        EXPECT_GT(maximumOpacity[0], .05f);
        EXPECT_GT(maximumOpacity[1], .05f);
        EXPECT_GT(maximumMixedHeightContribution, 1e-6f);
        RecordProperty("maximum_mixed_height_contribution", maximumMixedHeightContribution);
    }

    /// Compare a uniform cloud's fogged radiance with numerical front-to-back transport at each physical distance.
    TEST(AtmosphereShaderTest, cloudAerialPerspectiveIntegratesDistributedRadiance)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.eyePosition = {};
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.mistParameters = {};
        inputs.lowCloudParameters = { 1.f, 1.f, -3000.f, 10000.f };
        inputs.lowCloudEnvironmentInfluence = 0.f;
        inputs.environmentalCloudModel.mNoiseScalePerMeter = 0.f;
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mTypeMinimum = 1.f;
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        inputs.environmentalCloudModel.mExtinctionPerMeter = { .0001f, .0001f, .0001f };
        inputs.environmentalCloudModel.mAlbedo = { 1.f, 1.f, 1.f };
        inputs.solarRadiance = {};
        inputs.skyIrradiance = {};
        inputs.groundIrradiance = { 1.f, 1.f, 1.f };
        inputs.skyColor = { 0.f, 0.f, 0.f, 1.f };
        inputs.fogColor = inputs.skyColor;
        inputs.weatherWeight = 0.f;
        inputs.fogNear = 20000.f;
        inputs.fogFar = 180000.f;
        inputs.farPlane = 1000000.f;

        NativePassInputs densityInputs = inputs;
        densityInputs.renderCloudDensitySlice = true;
        densityInputs.cloudDensitySliceQuantity = 29;
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", densityInputs);
        ASSERT_EQ(density.pixels.size(), 1u);
        ASSERT_GT(density.pixels.front().x(), .01f);
        constexpr double worldUnitsPerMeter = 69.99125109;
        constexpr double cloudDistance = 13000.0 * worldUnitsPerMeter;
        const double extinction
            = density.pixels.front().x() * inputs.environmentalCloudModel.mExtinctionPerMeter.x() / worldUnitsPerMeter;
        const double expectedTransmittance = std::exp(-extinction * cloudDistance);
        for (const bool exponential : { false, true })
        {
            SCOPED_TRACE(exponential);
            inputs.exponentialFog = exponential;
            const double fogScale = exponential ? 4.4 : 1.0;
            const double curveStart = inputs.fogNear / fogScale;
            const double curveRange = (inputs.fogFar - curveStart) / fogScale;
            double expectedRadiance = 0.0;
            constexpr int steps = 16384;
            constexpr double stepLength = cloudDistance / steps;
            for (int step = 0; step < steps; ++step)
            {
                const double distance = (step + .5) * stepLength;
                const double opticalDepth = std::max(0.0, (distance - curveStart) / curveRange);
                const double visibility = exponential ? std::exp(-opticalDepth) : 1.0 - std::min(opticalDepth, 1.0);
                expectedRadiance += .5 * extinction * std::exp(-extinction * distance) * visibility * stepLength;
            }
            const PassImage actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(actual.pixels.size(), 1u);
            EXPECT_NEAR(actual.pixels.front().a(), expectedTransmittance, .002);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(actual.pixels.front()[channel], expectedRadiance, .002);
        }
    }

    /// Keep sunset in-scattering at zero until an opaque surface's view path reaches the fog volume.
    TEST(AtmosphereShaderTest, sunsetInScatteringDoesNotOverlayNearOpaqueGeometry)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Clear", "Sunset");
        inputs.width = 1;
        inputs.height = 1;
        inputs.sunDirection = { 0.9998f, 0.f, -0.02f, 0.f };
        inputs.fogNear = 5000.f;
        inputs.fogFar = 20000.f;
        inputs.cloudDensitySliceQuantity = 10;
        inputs.renderCloudDensitySlice = true;

        inputs.cloudDensitySliceExtent.x() = 2500.f;
        const PassImage nearSurfaceImage = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(nearSurfaceImage.pixels.size(), 1u);
        const osg::Vec4f nearSurface = nearSurfaceImage.pixels.front();
        EXPECT_FLOAT_EQ(nearSurface.w(), 1.f);
        EXPECT_NEAR(nearSurface.x(), 0.f, 1e-6f);
        EXPECT_NEAR(nearSurface.y(), 0.f, 1e-6f);
        EXPECT_NEAR(nearSurface.z(), 0.f, 1e-6f);

        inputs.cloudDensitySliceExtent.x() = 15000.f;
        const PassImage distantSurfaceImage = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(distantSurfaceImage.pixels.size(), 1u);
        const osg::Vec4f distantSurface = distantSurfaceImage.pixels.front();
        EXPECT_LT(distantSurface.w(), 1.f);
        EXPECT_GT(distantSurface.x() + distantSurface.y() + distantSurface.z(), 0.01f);
    }

    /// Verify that only the environmental low stratum follows the cached world-space terrain model.
    TEST(AtmosphereShaderTest, lowCloudStratumFollowsWorldSpaceTerrainReference)
    {
        NativePassInputs clearInputs;
        clearInputs.width = 96;
        clearInputs.height = 96;
        clearInputs.mistParameters = {};
        clearInputs.viewDirection = { 1.f, 0.f, 0.14f };
        clearInputs.farPlane = 200000.f;
        clearInputs.fogNear = 1000000.f;
        clearInputs.fogFar = 2000000.f;
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereRayGrid", clearInputs);

        NativePassInputs lowCloudInputs = clearInputs;
        lowCloudInputs.lowCloudParameters = { 0.78f, 1.05f, 2600.f, 9000.f };
        lowCloudInputs.lowCloudEnvironmentInfluence = 1.f;
        const PassImage lowClouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", lowCloudInputs);

        NativePassInputs shiftedTerrainInputs = lowCloudInputs;
        shiftedTerrainInputs.mediumReferenceParameters = { 42000.f, 1.f, 6000.f, 0.f };
        const PassImage shiftedTerrain = renderNativeAtmospherePass("integrateAtmosphereRayGrid", shiftedTerrainInputs);
        ASSERT_EQ(clear.pixels.size(), lowClouds.pixels.size());
        ASSERT_EQ(lowClouds.pixels.size(), shiftedTerrain.pixels.size());

        double cloudDifference = 0.0;
        double terrainDifference = 0.0;
        for (std::size_t index = 0; index < clear.pixels.size(); ++index)
        {
            for (int channel = 0; channel < 4; ++channel)
            {
                cloudDifference += std::abs(lowClouds.pixels[index][channel] - clear.pixels[index][channel]);
                terrainDifference += std::abs(shiftedTerrain.pixels[index][channel] - lowClouds.pixels[index][channel]);
            }
        }
        const double sampleCount = static_cast<double>(clear.pixels.size() * 4);
        EXPECT_GT(cloudDifference / sampleCount, 0.005)
            << "The low cloud stratum must contribute without the main weather stratum";
        EXPECT_GT(terrainDifference / sampleCount, 0.001)
            << "Low environmental cloud must respond to the cached world-space terrain reference";
    }

    TEST(AtmosphereShaderTest, mainWeatherCloudStratumIgnoresLocalTerrainReference)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 96;
        inputs.cloudParameters = { 0.78f, 1.1f, 36000.f, 22000.f };
        inputs.mistParameters = {};
        inputs.viewDirection = { 1.f, 0.f, 0.24f };
        inputs.farPlane = 240000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;
        const PassImage baseline = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);

        inputs.mediumReferenceParameters = { 42000.f, 1.f, 6000.f, 5000.f };
        inputs.orographicParameters = { 0.f, 0.f, 0.35f, -0.2f };
        const PassImage terrainShifted = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(baseline.pixels.size(), terrainShifted.pixels.size());

        EXPECT_LT(meanRgbDifference(baseline, terrainShifted), 1e-6)
            << "The main weather deck must retain its world altitude while low condensate follows terrain";
    }

    /// Compare droplet density at the same world position and cloud height on opposing wind slopes.
    TEST(AtmosphereShaderTest, lowCloudCondensateFormsWindwardAndDissipatesLeeward)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 29;
        inputs.lowCloudParameters = { 1.f, 0.9f, 2600.f, 9000.f };
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        inputs.lowCloudEnvironmentInfluence = 1.f;
        inputs.mediumReferenceParameters = { 100000.f, 1.f, 2600.f, 0.f };
        const float cellCenter = 0.5f * SceneUtil::AtmosphereTerrainGrid::sSpacingWorldUnits;
        inputs.eyePosition = { cellCenter, cellCenter, 103800.f };
        inputs.orographicParameters = { cellCenter, cellCenter, -0.2f, 0.f };
        const PassImage windward = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.orographicParameters.z() = 0.2f;
        const PassImage leeward = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(windward.pixels.size(), 1u);
        EXPECT_FLOAT_EQ(windward.pixels[0].g(), leeward.pixels[0].g());
        EXPECT_GT(windward.pixels[0].r(), 0.001f);
        EXPECT_GT(windward.pixels[0].r(), leeward.pixels[0].r() * 1.15f);
    }

    /// Keep droplet density and placement independent of terrain whenever either influence factor is zero.
    TEST(AtmosphereShaderTest, zeroTerrainInfluencePreservesLowCloudDensityAndPlacement)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 29;
        inputs.lowCloudParameters = { 1.f, 0.9f, 2600.f, 9000.f };
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        inputs.lowCloudEnvironmentInfluence = 0.f;
        inputs.mediumReferenceParameters = {};
        const float cellCenter = 0.5f * SceneUtil::AtmosphereTerrainGrid::sSpacingWorldUnits;
        inputs.eyePosition = { cellCenter, cellCenter, 3800.f };
        const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(reference.pixels.size(), 1u);
        ASSERT_GT(reference.pixels[0].r(), 0.001f);
        // The diagnostic slice stores normalized height in RGBA16F.
        EXPECT_NEAR(reference.pixels[0].g(), 0.26f, 1.f / 4096.f);
        EXPECT_FLOAT_EQ(reference.pixels[0].b(), 1.f);
        for (const osg::Vec2f weights : { osg::Vec2f(0.f, 1.f), osg::Vec2f(1.f, 0.f), osg::Vec2f(0.5f, 0.f) })
        {
            inputs.lowCloudEnvironmentInfluence = weights.x();
            inputs.mediumReferenceParameters = { 100000.f, weights.y(), 16000.f, 0.f };
            for (const float slope : { -0.2f, 0.f, 0.2f })
            {
                inputs.orographicParameters = { cellCenter, cellCenter, slope, -slope };
                const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(result.pixels.size(), 1u);
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_FLOAT_EQ(result.pixels[0][channel], reference.pixels[0][channel]);
            }
        }
    }

    /// Preserve terrain-weighted placement and droplet formation on analytic flat, windward, and leeward slopes.
    TEST(AtmosphereShaderTest, terrainFollowingCloudDensityPreservesWeightedPlacementAndFormation)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 29;
        inputs.lowCloudParameters = { 1.f, 0.9f, 2600.f, 9000.f };
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        const float cellCenter = 0.5f * SceneUtil::AtmosphereTerrainGrid::sSpacingWorldUnits;
        for (const osg::Vec2f weights : { osg::Vec2f(0.25f, 1.f), osg::Vec2f(1.f, 0.5f), osg::Vec2f(1.f, 1.f) })
        {
            const float weight = weights.x() * weights.y();
            const float expectedBase = 2600.f + 100000.f * weight;
            inputs.lowCloudEnvironmentInfluence = weights.x();
            inputs.mediumReferenceParameters = { 100000.f, weights.y(), 2600.f, 0.f };
            inputs.eyePosition = { cellCenter, cellCenter, expectedBase + 1200.f };
            for (const float slope : { -0.2f, 0.f, 0.2f })
            {
                SCOPED_TRACE(::testing::Message() << "weight=" << weight << ", slope=" << slope);
                inputs.orographicParameters = { cellCenter, cellCenter, slope, 0.f };
                const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                NativePassInputs fixedPlacement = inputs;
                fixedPlacement.lowCloudEnvironmentInfluence = 0.f;
                fixedPlacement.lowCloudParameters.z() = expectedBase;
                const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", fixedPlacement);
                ASSERT_EQ(result.pixels.size(), 1u);
                ASSERT_EQ(reference.pixels.size(), 1u);
                ASSERT_GT(reference.pixels[0].r(), 0.001f);
                // At the cell midpoint, smooth interpolation multiplies the analytic slope by 1.5;
                // these opposing slopes saturate the windward and leeward responses.
                const float expectedScale = 1.f + weight * (slope < 0.f ? 0.85f : slope > 0.f ? -0.7f : 0.f);
                EXPECT_FLOAT_EQ(result.pixels[0].g(), reference.pixels[0].g());
                EXPECT_NEAR(result.pixels[0].b(), expectedScale, expectedScale / 1024.f);
                EXPECT_NEAR(result.pixels[0].r(), reference.pixels[0].r() * expectedScale,
                    reference.pixels[0].r() * expectedScale / 512.f);
            }
        }
    }

    TEST(AtmosphereShaderTest, valleyConcavityPoolsMistMoreStronglyThanContinuousFog)
    {
        NativePassInputs mistInputs;
        mistInputs.cloudParameters = {};
        mistInputs.mistParameters = { 0.8f, 500.f, 4200.f, 0.f };
        mistInputs.fogVolumeParameters = {};
        mistInputs.mediumReferenceParameters = { 0.f, 0.f, 0.f, 0.f };
        const PassImage flatMist = renderNativeAtmospherePass("integrateAtmosphereRayGrid", mistInputs);
        mistInputs.mediumReferenceParameters.w() = 5000.f;
        const PassImage valleyMist = renderNativeAtmospherePass("integrateAtmosphereRayGrid", mistInputs);

        NativePassInputs fogInputs = mistInputs;
        fogInputs.mistParameters = {};
        fogInputs.fogVolumeParameters = { 0.8f, 0.f, 4200.f, 0.45f };
        fogInputs.mediumReferenceParameters.w() = 0.f;
        const PassImage flatFog = renderNativeAtmospherePass("integrateAtmosphereRayGrid", fogInputs);
        fogInputs.mediumReferenceParameters.w() = 5000.f;
        const PassImage valleyFog = renderNativeAtmospherePass("integrateAtmosphereRayGrid", fogInputs);

        const double mistDifference = meanRgbDifference(flatMist, valleyMist);
        const double fogDifference = meanRgbDifference(flatFog, valleyFog);
        EXPECT_GT(mistDifference, 0.001);
        EXPECT_GT(mistDifference, fogDifference * 1.5)
            << "Patchy mist must pool more strongly than sustained volumetric fog";
    }

    /// Verify that low environmental clouds remain volumetric from above as well as below.
    TEST(AtmosphereShaderTest, integratesLowCloudStratumBelowAnElevatedObserver)
    {
        NativePassInputs clearInputs;
        clearInputs.width = 96;
        clearInputs.height = 96;
        clearInputs.eyePosition = { 0.f, 0.f, 36000.f };
        clearInputs.viewDirection = { 0.4f, 0.1f, -1.f };
        clearInputs.mistParameters = {};
        clearInputs.farPlane = 200000.f;
        clearInputs.fogNear = 1000000.f;
        clearInputs.fogFar = 2000000.f;
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereRayGrid", clearInputs);

        NativePassInputs lowCloudInputs = clearInputs;
        lowCloudInputs.lowCloudParameters = { 0.82f, 1.1f, 2400.f, 10000.f };
        lowCloudInputs.lowCloudEnvironmentInfluence = 1.f;
        const PassImage lowClouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", lowCloudInputs);
        ASSERT_EQ(clear.pixels.size(), lowClouds.pixels.size());

        double difference = 0.0;
        double lostTransmittance = 0.0;
        for (std::size_t index = 0; index < clear.pixels.size(); ++index)
        {
            for (int channel = 0; channel < 3; ++channel)
                difference += std::abs(lowClouds.pixels[index][channel] - clear.pixels[index][channel]);
            lostTransmittance += 1.0 - lowClouds.pixels[index].w();
        }
        const double pixelCount = static_cast<double>(clear.pixels.size());
        EXPECT_GT(difference / pixelCount, 0.01);
        EXPECT_GT(lostTransmittance / pixelCount, 0.005);
    }

    /// Verify that low environmental clouds contribute above a ground observer as well as below an elevated one.
    TEST(AtmosphereShaderTest, integratesLowCloudStratumAboveAGroundObserver)
    {
        NativePassInputs clearInputs;
        clearInputs.width = 96;
        clearInputs.height = 64;
        clearInputs.eyePosition = { 0.f, 0.f, 1200.f };
        clearInputs.viewDirection = { 1.f, 0.1f, 0.28f };
        clearInputs.mistParameters = {};
        clearInputs.farPlane = 200000.f;
        clearInputs.fogNear = 1000000.f;
        clearInputs.fogFar = 2000000.f;
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereRayGrid", clearInputs);

        NativePassInputs lowCloudInputs = clearInputs;
        lowCloudInputs.lowCloudParameters = { 0.82f, 1.1f, 2400.f, 10000.f };
        lowCloudInputs.lowCloudEnvironmentInfluence = 1.f;
        const PassImage lowClouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", lowCloudInputs);
        ASSERT_EQ(clear.pixels.size(), lowClouds.pixels.size());

        const double difference = meanRgbDifference(clear, lowClouds);
        double lostTransmittance = 0.0;
        for (const osg::Vec4f& pixel : lowClouds.pixels)
            lostTransmittance += 1.0 - pixel.w();
        EXPECT_GT(difference, 0.01);
        EXPECT_GT(lostTransmittance / static_cast<double>(lowClouds.pixels.size()), 0.005);
    }

    /// Keep horizon rays stable when the observer is inside the compressed low-cloud formation domain.
    TEST(AtmosphereShaderTest, insideLowCloudHorizonDoesNotUndersampleCompressedFeatures)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 36;
        inputs.cloudParameters = { 0.502f, 0.769f, 35428.f, 179681.f };
        inputs.lowCloudParameters = { 0.149f, 0.542f, 4727.f, 6788.f };
        inputs.lowCloudEnvironmentInfluence = 1.f;
        inputs.mistParameters = { 0.045f, 759.f, 4370.f, 0.f };
        inputs.fogVolumeParameters = { 0.193f, 10.f, 5172.f, 0.445f };
        inputs.eyePosition = { -10880.f, 15904.f, 7700.f };
        inputs.viewDirection = { 0.650371f, -0.755118f, -0.082552f };
        inputs.cloudAdvectionOffsetWorldUnits = { 230.f, 106.f };
        inputs.farPlane = 230000.f;
        inputs.fogNear = 844.f;
        inputs.fogFar = 141987.f;
        inputs.historyValid = true;

        PassImage previous;
        double variation = 0.0;
        std::size_t comparedValues = 0;
        std::vector<double> rowVariation(static_cast<std::size_t>(inputs.height));
        std::vector<double> rowSpatialResidual(static_cast<std::size_t>(inputs.height));
        for (int frameNumber = 4; frameNumber < 8; ++frameNumber)
        {
            inputs.frameNumber = frameNumber;
            const PassImage frame = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_FALSE(frame.pixels.empty());
            if (!previous.pixels.empty())
            {
                for (std::size_t pixelIndex = 0; pixelIndex < frame.pixels.size(); ++pixelIndex)
                {
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        variation += std::abs(frame.pixels[pixelIndex][channel] - previous.pixels[pixelIndex][channel]);
                        ++comparedValues;
                    }
                    const osg::Vec4f difference = frame.pixels[pixelIndex] - previous.pixels[pixelIndex];
                    rowVariation[pixelIndex / static_cast<std::size_t>(inputs.width)]
                        += std::abs(difference.x()) * 0.2126 + std::abs(difference.y()) * 0.7152
                        + std::abs(difference.z()) * 0.0722;
                }
            }
            const auto luma
                = [](const osg::Vec4f& pixel) { return pixel.x() * 0.2126 + pixel.y() * 0.7152 + pixel.z() * 0.0722; };
            for (int y = 1; y < frame.height - 1; ++y)
                for (int x = 1; x < frame.width - 1; ++x)
                {
                    const double center = luma(frame.at(x, y));
                    const double crossMean = (center + luma(frame.at(x - 1, y)) + luma(frame.at(x + 1, y))
                                                 + luma(frame.at(x, y - 1)) + luma(frame.at(x, y + 1)))
                        / 5.0;
                    rowSpatialResidual[static_cast<std::size_t>(y)] += std::abs(center - crossMean);
                }
            previous = frame;
        }

        ASSERT_GT(comparedValues, 0u);
        const double meanVariation = variation / static_cast<double>(comparedValues);
        const double rowSampleCount = static_cast<double>(inputs.width * 3);
        const double maximumRowVariation = *std::max_element(rowVariation.begin(), rowVariation.end()) / rowSampleCount;
        const double spatialRowSampleCount = static_cast<double>((inputs.width - 2) * 4);
        const double maximumRowSpatialResidual
            = *std::max_element(rowSpatialResidual.begin(), rowSpatialResidual.end()) / spatialRowSampleCount;
        RecordProperty("mean_variation", meanVariation);
        RecordProperty("maximum_row_variation", maximumRowVariation);
        RecordProperty("maximum_row_spatial_residual", maximumRowSpatialResidual);
        EXPECT_LT(meanVariation, 0.01)
            << "A rotating ray offset must not make compressed low-cloud features scintillate at the horizon";
        EXPECT_LT(maximumRowVariation, 0.04)
            << "A narrow low-cloud horizon band must not evade the whole-frame stability bound";
        EXPECT_LT(maximumRowSpatialResidual, 0.2)
            << "A narrow low-cloud horizon band must not contain spatially incoherent ray estimates";
    }

    /// Verify typed density deformation without suppressing it a second time through shell development.
    TEST(AtmosphereShaderTest, typedCloudDeformationMatchesItsRequestedStrength)
    {
        const auto smooth = [](double lower, double upper, double height) {
            const double amount = std::clamp((height - lower) / (upper - lower), 0.0, 1.0);
            return amount * amount * (3.0 - 2.0 * amount);
        };
        for (float type : { 0.2f, 0.5f, 0.85f, 1.f })
        {
            const double small = std::max(1.0 - 2.0 * type, 0.0);
            const double medium = 1.0 - std::abs(2.0 * type - 1.0);
            const double large = std::max(2.0 * type - 1.0, 0.0);
            const std::array gradient{ 0.01, 0.1 * small + 0.08 * medium + 0.06 * large,
                0.11 * small + 0.3 * medium + 0.75 * large, 0.2 * small + 0.4 * medium + 0.95 * large };
            const double exponent = 15.0 * medium + 5.0 * large;
            for (float height : { 0.15f, 0.35f, 0.65f })
            {
                const double top = std::pow(
                    std::clamp(height + 0.1 * medium + 0.25 * large + 1.0 - gradient[3], 0.0, 1.0), exponent);
                const double bottom = std::pow(
                    std::clamp(1.0 - height + 0.1 * medium + 0.15 * large + gradient[0], 0.0, 1.0), exponent);
                const double profile
                    = smooth(gradient[0], gradient[1], height) - smooth(gradient[2], gradient[3], height);
                for (float coverageAmount : { 1.f, 2.f })
                {
                    NativePassInputs inputs;
                    inputs.width = 4;
                    inputs.height = 4;
                    inputs.renderCloudDensitySlice = true;
                    inputs.cloudDensitySliceQuantity = 4;
                    inputs.cloudDensitySliceExtent.x() = height;
                    inputs.cloudParameters = { 1.f, 1.f, 48000.f, 200000.f };
                    inputs.cloudModel.mTypeMinimum = type;
                    inputs.cloudModel.mCoverageAmount = coverageAmount;
                    inputs.lowCloudParameters = {};
                    inputs.mistParameters = {};
                    const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_FALSE(image.pixels.empty());
                    EXPECT_NEAR(image.at(0, 0).x(), profile * std::min(top + bottom, 1.0), 0.002)
                        << "type=" << type << " height=" << height << " coverageAmount=" << coverageAmount;
                }
            }
        }
    }

    /// Keep terrain influence independent of the environmental layer's source deformation.
    TEST(AtmosphereShaderTest, lowCloudEnvironmentInfluencePreservesSourceDeformation)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 12;
        inputs.lowCloudEnvironmentInfluence = 1.f;
        inputs.mistParameters = {};
        const PassImage influenced = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(influenced.pixels.size(), 1u);
        EXPECT_GT(influenced.pixels.front().y(), 0.f);

        inputs.lowCloudEnvironmentInfluence = 0.f;
        const PassImage neutral = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(neutral.pixels.size(), 1u);
        EXPECT_FLOAT_EQ(neutral.pixels.front().y(), influenced.pixels.front().y());
    }

    /// Keep spatial variation within fair-weather profiles without requiring deep convection in Clear skies.
    TEST(AtmosphereShaderTest, productionClearWeatherVariesWithinFairCloudTypes)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Clear", "Day");
        inputs.width = 128;
        inputs.height = 128;
        inputs.eyePosition = { 43333.f, 236667.f, 65000.f };
        inputs.cloudDensitySliceExtent = { 1800000.f, 1800000.f };
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 11;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        const PassImage field = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(field.pixels.empty());
        const auto [minimum, maximum]
            = std::ranges::minmax_element(field.pixels, {}, [](const osg::Vec4f& pixel) { return pixel.z(); });
        EXPECT_GE(minimum->z(), 0.f);
        EXPECT_LT(maximum->z(), 0.5f);
        EXPECT_GT(maximum->z() - minimum->z(), 0.04f)
            << "Fair-weather density profiles must vary across the source weather field";
    }

    /// Preserve source wind deformation independently of the authored weather-map and shape scales.
    TEST(AtmosphereShaderTest, cloudDomainScalesPreserveWindDeformation)
    {
        const auto deformationFor = [](float featureScale, float weatherScale) {
            NativePassInputs inputs;
            inputs.width = 1;
            inputs.height = 1;
            inputs.renderCloudDensitySlice = true;
            inputs.cloudDensitySliceQuantity = 17;
            inputs.cloudParameters = { 1.f, 1.f, 50000.f, 100000.f };
            inputs.cloudModel.mNoiseScalePerMeter = .0006f / featureScale;
            inputs.cloudModel.mShapeSkewMeters = 700.f * featureScale;
            inputs.cloudModel.mWeatherScalePerMeter = .00002f / weatherScale;
            inputs.cloudModel.mCoverageSkewMeters = 2500.f * weatherScale;
            inputs.lowCloudParameters = {};
            inputs.mistParameters = {};
            return renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs).pixels.front();
        };

        const osg::Vec4f fair = deformationFor(0.34f, 0.34f);
        const osg::Vec4f organized = deformationFor(1.7f, 0.9f);
        constexpr float expectedWeatherSkew = 2500.f * 0.00002f;
        EXPECT_NEAR(fair.x(), expectedWeatherSkew, 0.001f);
        EXPECT_NEAR(organized.x(), expectedWeatherSkew, 0.001f);
        EXPECT_GT(fair.y(), 0.f);
        EXPECT_NEAR(organized.y(), fair.y(), 0.001f);
        EXPECT_NEAR(fair.y(), 700.f * 0.0006f, 0.001f);
        EXPECT_NEAR(fair.z(), .00002f / .34f, 1e-7f);
        EXPECT_NEAR(organized.z(), .00002f / .9f, 1e-7f);
    }

    /// Keep the physical shell fixed while changing the weather field realization.
    TEST(AtmosphereShaderTest, mainCloudShellPlacementIsIndependentOfWeatherTexture)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Day");
        inputs.width = 96;
        inputs.height = 96;
        inputs.eyePosition = { 43333.f, 236667.f, 65000.f };
        inputs.cloudDensitySliceExtent = { 1800000.f, 1800000.f };
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 14;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};

        inputs.cloudScenarioParameters.x() = 0.f;
        const PassImage restrained = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.cloudScenarioParameters.x() = 2.7f;
        const PassImage varied = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(restrained.pixels.size(), varied.pixels.size());

        float maximumBaseDifference = 0.f;
        for (std::size_t index = 0; index < varied.pixels.size(); ++index)
        {
            maximumBaseDifference
                = std::max(maximumBaseDifference, std::abs(varied.pixels[index].x() - restrained.pixels[index].x()));
            EXPECT_NEAR(varied.pixels[index].y(), restrained.pixels[index].y(), 0.001f);
        }
        EXPECT_FLOAT_EQ(maximumBaseDifference, 0.f);
    }

    /// Prepare weather and reflection passes at first compilation without changing caller define state.
    TEST(AtmosphereShaderTest, cloudProgramsPrecompileWeatherVariants)
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->width = traits->height = 1;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        ASSERT_TRUE(context && context->realize());
        ASSERT_TRUE(context->makeCurrent());
        osg::State& state = *context->getState();
        for (const bool stacked : { false, true })
        {
            osg::ref_ptr<Shader::CloudProgram> program = new Shader::CloudProgram;
            program->addShader(
                new osg::Shader(osg::Shader::VERTEX, "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n"));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT, R"glsl(
                #version 120
                #pragma import_defines(OPENMWSE_CLOUD_STEADY, OPENMWSE_REFLECTED_CLOUD_MODE, TEST_UNRELATED)
                #ifndef OPENMWSE_CLOUD_STEADY
                #define OPENMWSE_CLOUD_STEADY 0
                #endif
                #ifndef OPENMWSE_REFLECTED_CLOUD_MODE
                #define OPENMWSE_REFLECTED_CLOUD_MODE 0
                #endif
                void main() { gl_FragColor = vec4(float(OPENMWSE_CLOUD_STEADY),
                    float(OPENMWSE_REFLECTED_CLOUD_MODE), 0.0, 1.0); }
            )glsl"));
            osg::ref_ptr<osg::StateSet> initial = new osg::StateSet;
            initial->setDefine(Shader::sSteadyCloudDefine, "0", osg::StateAttribute::ON);
            initial->setDefine(Shader::sReflectedCloudModeDefine, "2", osg::StateAttribute::ON);
            initial->setDefine("TEST_UNRELATED", "7", osg::StateAttribute::ON);
            if (stacked)
                state.pushStateSet(initial);
            else
                state.apply(initial);
            const std::string before = state.getDefineString(program->getShaderDefines());
            program->compileGLObjects(state);
            EXPECT_EQ(state.getDefineString(program->getShaderDefines()), before);
            for (const char* variant : { "1", "0", "1", "0" })
            {
                auto& defines = state.getDefineMap();
                defines.currentDefines[Shader::sSteadyCloudDefine] = { variant, osg::StateAttribute::ON };
                for (const char* mode : { "2", "0", "1" })
                {
                    defines.currentDefines[Shader::sReflectedCloudModeDefine] = { mode, osg::StateAttribute::ON };
                    const auto* compiled = program->getPCP(state);
                    ASSERT_NE(compiled, nullptr);
                    EXPECT_FALSE(compiled->needsLink());
                    EXPECT_TRUE(compiled->isLinked());
                }
            }
            if (stacked)
                state.popStateSet();
            else
                state.apply();
        }
        context->close();
    }

    /// Preserve cloud radiance and visible-depth metadata when the incoming weather branch is compiled away.
    TEST(AtmosphereShaderTest, steadyCloudVariantPreservesWeatherTransport)
    {
        for (const bool useUniformBuffer : { false, true })
            for (const float transition : { 0.f, 0.0001f, 0.5f, 1.f })
                for (const int quality : { 0, 1, 2, 3 })
                {
                    SCOPED_TRACE(::testing::Message()
                        << "ubo=" << useUniformBuffer << " transition=" << transition << " quality=" << quality);
                    NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
                    inputs.width = 12;
                    inputs.height = 8;
                    inputs.useUniformBuffer = useUniformBuffer;
                    inputs.bindUniformBufferOnAncestor = useUniformBuffer;
                    inputs.quality = quality;
                    inputs.cloudParameters = { 0.8f, 0.8f, 4000.f, 12000.f };
                    inputs.incomingCloudParameters = { 0.7f, 0.9f, 6000.f, 16000.f };
                    inputs.cloudModel.mCoverageMinimum = 0.9f;
                    inputs.incomingCloudModel.mCoverageMinimum = 0.9f;
                    inputs.cloudScenarioParameters = { 0.2f, 2.4f, transition, 1.f };
                    inputs.eyePosition = { 0.f, 0.f, 7000.f };
                    inputs.viewDirection = { 1.f, 0.f, 0.2f };
                    inputs.viewDirection.normalize();
                    inputs.farPlane = 40000.f;
                    inputs.lowCloudParameters = {};
                    inputs.mistParameters = {};
                    inputs.fogVolumeParameters = {};
                    PassImage metadata;
                    const PassImage actual
                        = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
                    inputs.useGeneralCloudShader = true;
                    PassImage referenceMetadata;
                    const PassImage reference
                        = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &referenceMetadata);
                    ASSERT_EQ(actual.pixels.size(), reference.pixels.size());
                    ASSERT_EQ(metadata.pixels.size(), referenceMetadata.pixels.size());
                    ASSERT_FALSE(actual.pixels.empty());
                    float maximumError = 0.f;
                    float maximumMetadataError = 0.f;
                    float maximumOpacity = 0.f;
                    for (std::size_t index = 0; index < actual.pixels.size(); ++index)
                    {
                        maximumOpacity = std::max(maximumOpacity, 1.f - reference.pixels[index].a());
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            ASSERT_TRUE(std::isfinite(actual.pixels[index][channel]));
                            ASSERT_TRUE(std::isfinite(metadata.pixels[index][channel]));
                            maximumError = std::max(maximumError,
                                std::abs(actual.pixels[index][channel] - reference.pixels[index][channel]));
                            maximumMetadataError = std::max(maximumMetadataError,
                                std::abs(metadata.pixels[index][channel] - referenceMetadata.pixels[index][channel]));
                        }
                    }
                    EXPECT_GT(maximumOpacity, 0.01f);
                    EXPECT_LE(maximumError, 1e-6f);
                    EXPECT_LE(maximumMetadataError, 1e-6f);
                }
    }

    /// Keep outgoing and incoming feature domains independent while their optical influence crossfades.
    TEST(AtmosphereShaderTest, weatherTransitionRetainsBothScenarioFields)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 15;
        inputs.cloudParameters = { 0.3f, 0.8f, 80000.f, 120000.f };
        inputs.cloudModel.mNoiseScalePerMeter = .0006f / .48f;
        inputs.incomingCloudParameters = { 0.8f, 1.2f, 30000.f, 60000.f };
        inputs.incomingCloudModel.mNoiseScalePerMeter = .0006f / 1.65f;
        inputs.cloudScenarioParameters = { 0.2f, 2.4f, 0.25f, 1.f };

        const PassImage transition = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(transition.pixels.size(), 1u);
        EXPECT_NEAR(transition.pixels.front().x(), .0006f / .48f, 1e-6f);
        EXPECT_NEAR(transition.pixels.front().y(), .0006f / 1.65f, 1e-6f);
        EXPECT_NEAR(transition.pixels.front().z(), 0.6f, 0.001f);
        EXPECT_NEAR(transition.pixels.front().w(), 0.3f, 0.001f);
    }

    /// Crossfade optical density without moving, rescaling, or rebuilding either episode field.
    TEST(AtmosphereShaderTest, weatherTransitionCrossfadesPersistentScenarioDensity)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 96;
        inputs.eyePosition = { 43333.f, 236667.f, 65000.f };
        inputs.cloudDensitySliceExtent = { 900000.f, 900000.f };
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 16;
        inputs.cloudParameters = { 0.8f, 1.f, 50000.f, 105000.f };
        inputs.incomingCloudParameters = { 0.82f, 1.05f, 45000.f, 125000.f };
        inputs.cloudScenarioParameters = { 0.2f, 2.4f, 0.f, 1.f };
        const PassImage outgoing = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.cloudScenarioParameters.z() = 1.f;
        const PassImage incoming = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.cloudScenarioParameters.z() = 0.5f;
        const PassImage midpoint = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(outgoing.pixels.size(), midpoint.pixels.size());
        ASSERT_EQ(incoming.pixels.size(), midpoint.pixels.size());

        double outgoingDensity = 0.0;
        double incomingDensity = 0.0;
        float maximumOutgoingError = 0.f;
        float maximumIncomingError = 0.f;
        float maximumSumError = 0.f;
        for (std::size_t index = 0; index < midpoint.pixels.size(); ++index)
        {
            EXPECT_FLOAT_EQ(outgoing.pixels[index].y(), 0.f);
            EXPECT_FLOAT_EQ(incoming.pixels[index].x(), 0.f);
            maximumOutgoingError = std::max(
                maximumOutgoingError, std::abs(midpoint.pixels[index].x() - outgoing.pixels[index].x() * 0.5f));
            maximumIncomingError = std::max(
                maximumIncomingError, std::abs(midpoint.pixels[index].y() - incoming.pixels[index].y() * 0.5f));
            maximumSumError = std::max(maximumSumError,
                std::abs(midpoint.pixels[index].z() - midpoint.pixels[index].x() - midpoint.pixels[index].y()));
            outgoingDensity += midpoint.pixels[index].x();
            incomingDensity += midpoint.pixels[index].y();
        }
        EXPECT_LT(maximumOutgoingError, 6e-4f);
        EXPECT_LT(maximumIncomingError, 6e-4f);
        EXPECT_LT(maximumSumError, 6e-4f);
        EXPECT_GT(outgoingDensity, 1.0);
        EXPECT_GT(incomingDensity, 1.0);
    }

    /// Preserve cloud radiance and extinction when a weather transition blends identical episode fields.
    TEST(AtmosphereShaderTest, identicalWeatherCloudsPreserveRadianceThroughoutTransition)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        inputs.width = 32;
        inputs.height = 24;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.farPlane = 229376.f;
        inputs.cloudParameters = { .7f, .04f, 42660.f, 153203.f };
        inputs.cloudModel.mCoverageMinimum = .6f;
        inputs.incomingCloudParameters = inputs.cloudParameters;
        inputs.incomingCloudModel = inputs.cloudModel;
        inputs.cloudScenarioParameters = { 0.f, 0.f, 0.f, 1.f };
        inputs.eyePosition = { -30126.f, 72900.f, 41595.f };
        inputs.viewDirection = { -.8f, .55f, .08f };
        inputs.viewDirection.normalize();
        inputs.verticalFov = 35.f;
        const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_GT(std::count_if(reference.pixels.begin(), reference.pixels.end(),
                      [](const osg::Vec4f& pixel) { return pixel.a() < .9f; }),
            0);
        for (const float weight : { .1f, .5f, .9f, 1.f })
        {
            SCOPED_TRACE(weight);
            inputs.cloudScenarioParameters.z() = weight;
            const PassImage transition = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(reference.pixels.size(), transition.pixels.size());
            float maximumError = 0.f;
            for (std::size_t index = 0; index < reference.pixels.size(); ++index)
                for (int channel = 0; channel < 4; ++channel)
                    maximumError = std::max(
                        maximumError, std::abs(transition.pixels[index][channel] - reference.pixels[index][channel]));
            EXPECT_LT(maximumError, .002f);
        }
    }

    /// Verify that production Clear clouds do not carry their lower support unchanged into the upper layer.
    TEST(AtmosphereShaderTest, productionClearCloudsDoNotExtrudeCoverageColumns)
    {
        const auto densitySupport = [](float heightFraction) {
            NativePassInputs inputs = makePackagedAtmosphereInputs("Clear", "Day");
            inputs.width = 96;
            inputs.height = 96;
            inputs.eyePosition
                = { 43333.f, 236667.f, inputs.cloudParameters.z() + heightFraction * inputs.cloudParameters.w() };
            inputs.cloudDensitySliceExtent = { 1800000.f, 1800000.f };
            inputs.cloudDensitySliceAxis = 2;
            inputs.renderCloudDensitySlice = true;
            inputs.mistParameters = {};
            inputs.lowCloudParameters = {};
            const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            std::vector<bool> support(image.pixels.size());
            for (std::size_t index = 0; index < image.pixels.size(); ++index)
                support[index] = image.pixels[index].x() > 0.01f;
            return support;
        };

        const std::vector<bool> lowerSupport = densitySupport(.1f);
        const std::vector<bool> upperSupport = densitySupport(.18f);
        ASSERT_EQ(lowerSupport.size(), upperSupport.size());
        std::size_t lowerCount = 0;
        std::size_t upperCount = 0;
        std::size_t unionCount = 0;
        std::size_t intersectionCount = 0;
        for (std::size_t index = 0; index < lowerSupport.size(); ++index)
        {
            lowerCount += lowerSupport[index] ? 1u : 0u;
            upperCount += upperSupport[index] ? 1u : 0u;
            unionCount += lowerSupport[index] || upperSupport[index] ? 1u : 0u;
            intersectionCount += lowerSupport[index] && upperSupport[index] ? 1u : 0u;
        }
        ASSERT_GT(lowerCount, 0u);
        ASSERT_GT(upperCount, 0u);
        ASSERT_GT(unionCount, 0u);
        EXPECT_LT(upperCount * 10, lowerCount * 9)
            << "Clear-weather crowns must occupy materially less area than their lower cloud mass"
            << " lower=" << lowerCount << " upper=" << upperCount;
        EXPECT_LT(static_cast<double>(intersectionCount) / static_cast<double>(unionCount), 0.8)
            << "Clear-weather upper density must not repeat its lower coverage footprint"
            << " lower=" << lowerCount << " upper=" << upperCount;
    }

    /// Verify that production Clear-cloud side profiles do not expose long vertical weather-column walls.
    TEST(AtmosphereShaderTest, productionClearCloudProfilesAvoidVerticalSupportWalls)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Clear", "Day");
        inputs.width = 192;
        inputs.height = 96;
        inputs.eyePosition = { 43333.f, 355000.f, 220000.f };
        inputs.cloudDensitySliceExtent = { 1800000.f, 300000.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(density.width, inputs.width);
        ASSERT_EQ(density.height, inputs.height);

        int longestVerticalBoundary = 0;
        for (int x = 1; x < density.width; ++x)
        {
            int currentBoundaryLength = 0;
            for (int y = 0; y < density.height; ++y)
            {
                const bool previousOccupied = density.at(x - 1, y).x() > 0.01f;
                const bool currentOccupied = density.at(x, y).x() > 0.01f;
                currentBoundaryLength = previousOccupied != currentOccupied ? currentBoundaryLength + 1 : 0;
                longestVerticalBoundary = std::max(longestVerticalBoundary, currentBoundaryLength);
            }
        }
        EXPECT_LT(longestVerticalBoundary, density.height / 4)
            << "Clear-weather density must not expose tall vertical coverage-map boundaries";
    }

    /// Verify that production Clear-cloud profiles do not repeat broad horizontal density shelves.
    TEST(AtmosphereShaderTest, productionClearCloudProfilesAvoidRepeatedHorizontalBands)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Clear", "Day");
        inputs.width = 192;
        inputs.height = 128;
        inputs.eyePosition = { 43333.f, 355000.f, 220000.f };
        inputs.cloudDensitySliceExtent = { 1800000.f, 300000.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(density.width, inputs.width);
        ASSERT_EQ(density.height, inputs.height);

        int broadBoundaryRows = 0;
        int longestBroadBoundaryRun = 0;
        int currentBroadBoundaryRun = 0;
        for (int y = 1; y < density.height; ++y)
        {
            int changedSupport = 0;
            for (int x = 0; x < density.width; ++x)
            {
                const bool previousOccupied = density.at(x, y - 1).x() > 0.01f;
                const bool currentOccupied = density.at(x, y).x() > 0.01f;
                changedSupport += previousOccupied != currentOccupied ? 1 : 0;
            }
            const bool broadBoundary = changedSupport > density.width / 5;
            broadBoundaryRows += broadBoundary ? 1 : 0;
            currentBroadBoundaryRun = broadBoundary ? currentBroadBoundaryRun + 1 : 0;
            longestBroadBoundaryRun = std::max(longestBroadBoundaryRun, currentBroadBoundaryRun);
        }

        EXPECT_EQ(broadBoundaryRows, 0)
            << "Clear-weather density must not be assembled from repeated full-width height bands";
        EXPECT_LE(longestBroadBoundaryRun, 2)
            << "Cloud support must change through a shaped boundary rather than a horizontal density shelf";
    }

    /// Verify that the resolved fair-cumulus scenario produces several independent cloud masses.
    TEST(AtmosphereShaderTest, fairCumulusPopulationUsesConvectiveCellScale)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Clear", "Day");
        inputs.cloudParameters = { 0.3f, 0.8f, 60000.f, 90000.f };
        inputs.width = 192;
        inputs.height = 192;
        inputs.eyePosition = { 43333.f, 236667.f, 75000.f };
        inputs.cloudDensitySliceExtent = { 1200000.f, 1200000.f };
        inputs.renderCloudDensitySlice = true;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(density.width, inputs.width);
        ASSERT_EQ(density.height, inputs.height);

        std::vector<bool> visited(density.pixels.size());
        std::size_t occupiedPixels = 0;
        std::size_t largestComponent = 0;
        std::size_t componentCount = 0;
        std::size_t substantialComponents = 0;
        for (int y = 0; y < density.height; ++y)
            for (int x = 0; x < density.width; ++x)
            {
                const std::size_t startIndex = static_cast<std::size_t>(y * density.width + x);
                if (visited[startIndex] || density.pixels[startIndex].x() <= 0.05f)
                    continue;

                std::vector<std::pair<int, int>> pending{ { x, y } };
                visited[startIndex] = true;
                std::size_t componentSize = 0;
                while (!pending.empty())
                {
                    const auto [currentX, currentY] = pending.back();
                    pending.pop_back();
                    ++componentSize;
                    for (const auto [offsetX, offsetY] :
                        std::array{ std::pair{ -1, 0 }, std::pair{ 1, 0 }, std::pair{ 0, -1 }, std::pair{ 0, 1 } })
                    {
                        const int neighborX = currentX + offsetX;
                        const int neighborY = currentY + offsetY;
                        if (neighborX < 0 || neighborX >= density.width || neighborY < 0 || neighborY >= density.height)
                            continue;
                        const std::size_t neighborIndex
                            = static_cast<std::size_t>(neighborY * density.width + neighborX);
                        if (visited[neighborIndex] || density.pixels[neighborIndex].x() <= 0.05f)
                            continue;
                        visited[neighborIndex] = true;
                        pending.emplace_back(neighborX, neighborY);
                    }
                }
                occupiedPixels += componentSize;
                ++componentCount;
                largestComponent = std::max(largestComponent, componentSize);
                substantialComponents += componentSize >= 4 ? 1u : 0u;
            }

        ASSERT_GT(occupiedPixels, 0u);
        EXPECT_GE(substantialComponents, 4u) << "Clear weather must contain several independently shaped cloud masses"
                                             << " components=" << substantialComponents << " total=" << componentCount
                                             << " largest=" << largestComponent << " occupied=" << occupiedPixels;
        EXPECT_LT(static_cast<double>(largestComponent) / static_cast<double>(occupiedPixels), 0.7)
            << "A single weather-map feature must not dominate the entire Clear cloud population"
            << " largest=" << largestComponent << " occupied=" << occupiedPixels;
    }

    /// Verify that cloud sun access varies continuously across an occupied density field.
    TEST(AtmosphereShaderTest, productionCloudLightingAvoidsQuantizedSunAccessContours)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Thunderstorm", "Day");
        inputs.width = 192;
        inputs.height = 128;
        inputs.eyePosition = { 43333.f, 355000.f, 78000.f };
        inputs.cloudDensitySliceExtent = { 333333.f, 130000.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        inputs.sunDirection = { 0.55f, 0.25f, 0.8f, 0.f };
        const PassImage lighting = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(lighting.width, inputs.width);
        ASSERT_EQ(lighting.height, inputs.height);

        std::array<std::size_t, 64> sunAccessBins{};
        std::size_t occupiedPixels = 0;
        for (const osg::Vec4f& pixel : lighting.pixels)
        {
            if (pixel.w() <= 0.08f)
                continue;
            const std::size_t bin = std::min<std::size_t>(
                static_cast<std::size_t>(std::clamp(pixel.x(), 0.f, 1.f) * sunAccessBins.size()),
                sunAccessBins.size() - 1);
            ++sunAccessBins[bin];
            ++occupiedPixels;
        }
        const std::size_t populatedBins
            = std::ranges::count_if(sunAccessBins, [](std::size_t population) { return population > 0; });

        ASSERT_GT(occupiedPixels, 0u);
        EXPECT_GE(populatedBins, 32u)
            << "Cloud sun access must remain a continuous optical-depth response instead of a small shade palette"
            << " populated-bins=" << populatedBins << " occupied=" << occupiedPixels;
    }

    /// Retain arbitrarily deep solar extinction in both scattering orders instead of flooring transmittance.
    TEST(AtmosphereShaderTest, denseCloudCoresCanExtinguishBothSolarScatteringOrders)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Thunderstorm", "Day");
        inputs.width = 96;
        inputs.height = 64;
        inputs.eyePosition = { 43333.f, 355000.f, 78000.f };
        inputs.cloudDensitySliceExtent = { 333333.f, 130000.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 1;
        inputs.renderCloudDensitySlice = true;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        inputs.cloudParameters.y() = 8.f;
        inputs.sunDirection = { 0.55f, 0.25f, 0.8f, 0.f };
        const PassImage lighting = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(lighting.pixels.empty());

        std::size_t deepCorePixels = 0;
        std::size_t translucentPixels = 0;
        for (const osg::Vec4f& pixel : lighting.pixels)
        {
            if (pixel.w() <= 0.08f)
                continue;
            EXPECT_GE(pixel.x(), 0.f);
            EXPECT_LE(pixel.y(), 1.f);
            EXPECT_LE(pixel.x(), pixel.y() + 1e-6f);
            deepCorePixels += pixel.x() < 1e-6f && pixel.y() < 0.001f ? 1u : 0u;
            translucentPixels += pixel.x() > 0.01f && pixel.x() < 0.9f ? 1u : 0u;
        }
        EXPECT_GT(deepCorePixels, lighting.pixels.size() / 100)
            << "Deep storm cores must not receive a forced minimum fraction of direct solar light";
        EXPECT_GT(translucentPixels, 0u) << "Exposed cloud edges must still transmit sunlight";
    }

    TEST(AtmosphereShaderTest, coverageHeavyWeatherRetainsZenithAndMultiAzimuthCloudSupport)
    {
        const auto occupiedFraction = [](const PassImage& image) {
            return static_cast<double>(std::count_if(image.pixels.begin(), image.pixels.end(),
                       [](const osg::Vec4f& pixel) { return 1.f - pixel.w() > 0.03f; }))
                / static_cast<double>(image.pixels.size());
        };
        const std::array<osg::Vec3f, 5> directions{ osg::Vec3f(0.01f, 0.f, 1.f), osg::Vec3f(1.f, 0.f, 0.24f),
            osg::Vec3f(-1.f, 0.f, 0.24f), osg::Vec3f(0.f, 1.f, 0.24f), osg::Vec3f(0.f, -1.f, 0.24f) };

        for (std::string_view weather : { "Overcast", "Rain", "Thunderstorm" })
        {
            NativePassInputs inputs = makePackagedAtmosphereInputs(weather, "Day");
            inputs.width = 64;
            inputs.height = 48;
            inputs.mistParameters = {};
            inputs.lowCloudParameters = {};
            inputs.eyePosition = { 0.f, 0.f, 1200.f };
            inputs.farPlane = 500000.f;
            inputs.fogNear = 1000000.f;
            inputs.fogFar = 2000000.f;
            for (std::size_t directionIndex = 0; directionIndex < directions.size(); ++directionIndex)
            {
                inputs.viewDirection = directions[directionIndex];
                const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(clouds.pixels.empty());
                const double minimumSupport = directionIndex == 0 ? 0.52 : 0.28;
                EXPECT_GT(occupiedFraction(clouds), minimumSupport) << weather << " direction=" << directionIndex;
            }
        }
    }

    TEST(AtmosphereShaderTest, cloudRadianceParticipatesInDistanceFogWithoutLosingDensity)
    {
        NativePassInputs foggedInputs;
        foggedInputs.cloudParameters = { 0.72f, 1.f, 48000.f, 22000.f };
        foggedInputs.mistParameters = {};
        foggedInputs.sunDirection = { 0.97f, 0.f, 0.24f, 0.f };
        foggedInputs.weatherWeight = 0.f;
        foggedInputs.skyColor = foggedInputs.fogColor;
        const PassImage fogged = renderNativeAtmospherePass("integrateAtmosphereRayGrid", foggedInputs);
        ASSERT_FALSE(fogged.pixels.empty());

        NativePassInputs clearDistanceInputs = foggedInputs;
        clearDistanceInputs.fogNear = 1000000.f;
        clearDistanceInputs.fogFar = 2000000.f;
        const PassImage clearDistance = renderNativeAtmospherePass("integrateAtmosphereRayGrid", clearDistanceInputs);
        ASSERT_EQ(clearDistance.pixels.size(), fogged.pixels.size());

        double radianceDifference = 0.0;
        double alphaDifference = 0.0;
        std::size_t occupiedPixelCount = 0;
        for (std::size_t index = 0; index < fogged.pixels.size(); ++index)
        {
            for (int channel = 0; channel < 3; ++channel)
                radianceDifference += std::abs(fogged.pixels[index][channel] - clearDistance.pixels[index][channel]);
            alphaDifference += std::abs(fogged.pixels[index].w() - clearDistance.pixels[index].w());

            const float opacity = 1.f - fogged.pixels[index].w();
            if (opacity <= 0.02f)
                continue;
            ++occupiedPixelCount;
        }
        const double pixelCount = static_cast<double>(fogged.pixels.size());
        EXPECT_GT(radianceDifference / pixelCount, 0.01);
        EXPECT_LT(alphaDifference / pixelCount, 1e-5);
        ASSERT_GT(occupiedPixelCount, 0u);
        EXPECT_NEAR(alphaDifference, 0.0, 1e-5)
            << "Aerial perspective must change cloud radiance without changing physical cloud density";
    }

    /// Preserve the source wind-shear vector instead of shearing it again through the top-view coordinate warp.
    TEST(AtmosphereShaderTest, cloudShapeDomainPreservesHeightDependentWindShear)
    {
        for (float featureScale : { 0.25f, 1.f, 2.f })
        {
            for (float relativeHeight : { 0.f, 0.25f, 0.5f, 1.f })
            {
                NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Day");
                inputs.width = 4;
                inputs.height = 4;
                inputs.renderCloudDensitySlice = true;
                inputs.cloudDensitySliceQuantity = 5;
                inputs.cloudDensitySliceExtent.x() = relativeHeight;
                inputs.cloudModel.mNoiseScalePerMeter = .0006f / featureScale;
                inputs.cloudModel.mShapeSkewMeters = 700.f * featureScale;
                const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(image.pixels.empty());
                // Scale the physical shear with its feature domain so authored size changes do not alter its phase
                // offset.
                const double displacement = relativeHeight * 700.0 * 0.0006;
                const osg::Vec3d expected = osg::Vec3d(std::cos(0.75), -0.5, std::sin(0.75)) * displacement;
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(image.at(0, 0)[channel], expected[channel], 0.001)
                        << "height=" << relativeHeight << " scale=" << featureScale << " channel=" << channel;
            }
        }
    }

    /// Keep shape wind independent from weather-map motion and outside the top-view domain warp.
    TEST(AtmosphereShaderTest, cloudShapeDomainUsesIndependentThreeDimensionalWind)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Day");
        inputs.width = 4;
        inputs.height = 4;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 5;
        inputs.cloudDensitySliceExtent.x() = 0.f;
        inputs.cloudModel = SceneUtil::CloudLayer{};
        for (bool useUniformBuffer : { false, true })
        {
            inputs.useUniformBuffer = useUniformBuffer;
            for (float phase : { -1.f, 0.f, 1.f, 2.f })
            {
                inputs.cloudAdvectionOffsetWorldUnits = osg::Vec2f(7800.f, 3600.f) * phase;
                const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(image.pixels.empty());
                const double alongWind = std::hypot(7800.0, 3600.0) * phase;
                const double angle = inputs.cloudModel.mWindAngleRadians;
                const double noiseScale = 0.0006 / 69.99125109;
                const osg::Vec3d expected(std::cos(angle) * alongWind * 0.5 * noiseScale,
                    -alongWind * 0.25 * noiseScale, std::sin(angle) * alongWind * 0.5 * noiseScale);
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(image.at(0, 0)[channel], expected[channel], 0.0001);
            }
        }
    }

    /// Require internal cloud evolution after compensating weather travel, with continuous short-step density.
    TEST(AtmosphereShaderTest, cloudDensityEvolvesWithinAdvectedWeatherColumns)
    {
        NativePassInputs inputs;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 200000.f };
        inputs.cloudModel.mCoverageMinimum = .8f;
        inputs.cloudModel.mTypeMinimum = .7f;
        inputs.cloudModel.mTypeAmount = 0.f;
        inputs.width = 128;
        inputs.height = 128;
        inputs.eyePosition = { 0.f, 0.f, 120000.f };
        inputs.cloudDensitySliceExtent = { 1800000.f, 1800000.f };
        inputs.renderCloudDensitySlice = true;
        const PassImage initial = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(initial.pixels.empty());
        for (float phase : { 0.0001f, 1.f })
        {
            NativePassInputs moving = inputs;
            moving.cloudAdvectionOffsetWorldUnits = osg::Vec2f(15600.f, 7200.f) * phase;
            osg::Vec2f clockDirection(.78f, .36f);
            clockDirection.normalize();
            const float animationSeconds
                = (moving.cloudAdvectionOffsetWorldUnits * clockDirection) / (30.f * 69.99125109f);
            const float weatherTravel
                = animationSeconds * inputs.cloudModel.mCoverageWindSpeedMetersPerSecond * 69.99125109f;
            moving.eyePosition.x() -= std::cos(inputs.cloudModel.mCoverageWindAngleRadians) * weatherTravel;
            moving.eyePosition.y() -= std::sin(inputs.cloudModel.mCoverageWindAngleRadians) * weatherTravel;
            const PassImage evolved = renderNativeAtmospherePass("integrateAtmosphereRayGrid", moving);
            ASSERT_EQ(evolved.pixels.size(), initial.pixels.size());
            double meanDifference = 0.0;
            std::size_t occupied = 0;
            for (std::size_t pixel = 0; pixel < initial.pixels.size(); ++pixel)
            {
                meanDifference += std::abs(initial.pixels[pixel].x() - evolved.pixels[pixel].x());
                occupied += initial.pixels[pixel].x() > 0.01f || evolved.pixels[pixel].x() > 0.01f;
            }
            ASSERT_GT(occupied, initial.pixels.size() / 20);
            meanDifference /= static_cast<double>(initial.pixels.size());
            if (phase < 1.f)
                EXPECT_LT(meanDifference, 0.001);
            else
                EXPECT_GT(meanDifference, 0.002) << "A weather-following cloud must evolve rather than remain frozen";
        }
    }

    /// Verify every uploaded shape/detail texel and the energy-preserving mip chain sampled by cloud transport.
    TEST(AtmosphereShaderTest, cloudNoiseMipChainsPreserveChannelsAndReduceVariance)
    {
        for (int textureIndex = 0; textureIndex < 2; ++textureIndex)
        {
            const std::string_view textureName
                = textureIndex == 0 ? "atmosphere_cloud_noise.dds" : "atmosphere_cloud_detail.dds";
            const auto texturePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data/textures/openmwse" / textureName;
            osg::ref_ptr<osg::Image> source = osgDB::readRefImageFile(texturePath.string());
            ASSERT_TRUE(source.valid());
            const int size = textureIndex == 0 ? 64 : 32;
            ASSERT_EQ(source->s(), size);
            ASSERT_EQ(source->t(), size);
            ASSERT_EQ(source->r(), size);
            // Check the DDS coordinate layout independently of OSG's decoder before validating GPU upload.
            std::ifstream textureStream(texturePath, std::ios::binary);
            ASSERT_TRUE(textureStream.good());
            textureStream.seekg(128);
            std::vector<unsigned char> authoredTexels(static_cast<std::size_t>(size * size * size * 4));
            textureStream.read(reinterpret_cast<char*>(authoredTexels.data()), authoredTexels.size());
            ASSERT_EQ(textureStream.gcount(), static_cast<std::streamsize>(authoredTexels.size()));
            EXPECT_TRUE(std::equal(authoredTexels.begin(), authoredTexels.end(), source->data()))
                << textureName << " changed its authored X/Y/Z/channel layout during DDS decoding";

            NativePassInputs inputs;
            inputs.width = size;
            inputs.height = size * size;
            inputs.renderCloudDensitySlice = true;
            inputs.cloudDensitySliceQuantity = 8;
            inputs.cloudDensitySliceAxis = textureIndex;
            std::array<double, 4> baseMean{};
            std::array<double, 4> previousVariance{};
            for (int mip = 0; mip <= 3; ++mip)
            {
                inputs.cloudDensitySliceExtent.x() = static_cast<float>(mip);
                const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(image.pixels.size(), static_cast<std::size_t>(size * size * size));
                std::array<double, 4> mean{};
                std::array<double, 4> variance{};
                double maximumUploadError = 0.0;
                for (std::size_t index = 0; index < image.pixels.size(); ++index)
                {
                    const osg::Vec4f expected = mip == 0 ? source->getColor(static_cast<unsigned int>(index % size),
                                                    static_cast<unsigned int>((index / size) % size),
                                                    static_cast<unsigned int>(index / (size * size)))
                                                         : osg::Vec4f();
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        const double value = image.pixels[index][channel];
                        ASSERT_TRUE(std::isfinite(value));
                        ASSERT_GE(value, 0.0);
                        ASSERT_LE(value, 1.0);
                        mean[channel] += value;
                        variance[channel] += value * value;
                        if (mip == 0)
                            maximumUploadError = std::max(maximumUploadError, std::abs(value - expected[channel]));
                    }
                }
                EXPECT_LT(maximumUploadError, 0.001) << textureName;
                for (int channel = 0; channel < 4; ++channel)
                {
                    mean[channel] /= image.pixels.size();
                    variance[channel] = variance[channel] / image.pixels.size() - mean[channel] * mean[channel];
                    if (mip == 0)
                    {
                        baseMean[channel] = mean[channel];
                        if (textureIndex == 0 || channel < 3)
                            EXPECT_GT(variance[channel], 0.001) << textureName << " channel=" << channel;
                    }
                    else
                    {
                        // RGBA8 mip quantization and RGBA16F readback permit less than one byte of mean drift per
                        // level.
                        EXPECT_NEAR(mean[channel], baseMean[channel], static_cast<double>(mip) / 255.0 + 0.001);
                        if (textureIndex == 0 || channel < 3)
                            EXPECT_LT(variance[channel], previousVariance[channel] * 0.95);
                        else
                            EXPECT_NEAR(variance[channel], 0.0, 1e-6);
                    }
                    previousVariance[channel] = variance[channel];
                    const std::string property = std::string(textureIndex == 0 ? "shape" : "detail") + "_mip"
                        + std::to_string(mip) + "_channel" + std::to_string(channel);
                    RecordProperty(property + "_mean", std::to_string(mean[channel]));
                    RecordProperty(property + "_variance", std::to_string(variance[channel]));
                }
            }
        }
    }

    /// Keep advected curl displacement continuous across texel boundaries, including the periodic seam.
    TEST(AtmosphereShaderTest, cloudCurlDisplacementIsContinuousAcrossTextureCells)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Day");
        inputs.width = 4;
        inputs.height = 4;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 6;
        const auto sampleCurl = [&inputs](float texelX) {
            inputs.cloudDensitySliceExtent = { texelX / 128.f, 49.5f / 128.f };
            const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            if (image.pixels.empty())
                throw std::runtime_error("Cloud curl continuity test has no rendered samples");
            return image.at(0, 0);
        };
        float fieldVariation = 0.f;
        for (float boundary : { 0.f, 33.f, 65.f, 97.f })
        {
            const osg::Vec4f left = sampleCurl(boundary - 0.001f);
            const osg::Vec4f right = sampleCurl(boundary + 0.001f);
            const osg::Vec4f center = sampleCurl(boundary);
            const osg::Vec4f firstTexel = sampleCurl(boundary - 0.5f);
            const osg::Vec4f secondTexel = sampleCurl(boundary + 0.5f);
            for (int channel = 0; channel < 3; ++channel)
            {
                EXPECT_NEAR(left[channel], right[channel], 0.001f) << "boundary=" << boundary << " channel=" << channel;
                EXPECT_NEAR(center[channel], (firstTexel[channel] + secondTexel[channel]) * 0.5f, 0.001f);
                fieldVariation += std::abs(firstTexel[channel] - secondTexel[channel]);
            }
        }
        EXPECT_GT(fieldVariation, 0.01f) << "The continuity check must exercise nonconstant curl data";
    }

    /// Keep cloud aerial perspective at the real observer even when incident illumination is sampled above the deck.
    TEST(AtmosphereShaderTest, groundCloudScatteringDoesNotUseAboveDeckSkyChromaticity)
    {
        NativePassInputs inputs;
        // Isolate aerial-perspective chromaticity from the cloud's independently spectral extinction.
        inputs.cloudModel.mExtinctionPerMeter = { .086f, .086f, .086f };
        inputs.cloudParameters = { 0.95f, 1.8f, 16000.f, 42000.f };
        inputs.mistParameters = {};
        inputs.weatherWeight = 0.f;
        inputs.fogColor = { 0.4f, 0.4f, 0.4f, 1.f };
        inputs.skyColor = inputs.fogColor;
        inputs.skyIrradiance = { 0.2f, 0.2f, 0.2f };
        inputs.groundIrradiance = inputs.skyIrradiance;
        inputs.solarRadiance = { 1.f, 1.f, 1.f };
        inputs.sunDirection = { 0.f, 0.f, 1.f, 0.f };
        inputs.viewDirection = { 1.f, 0.f, 0.3f };
        inputs.fogNear = 0.f;
        inputs.fogFar = 1000.f;
        inputs.farPlane = 300000.f;
        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(clouds.pixels.empty());
        std::size_t occupiedPixels = 0;
        double meanChromaticDifference = 0.0;
        for (const osg::Vec4f& pixel : clouds.pixels)
        {
            const float opacity = 1.f - pixel.w();
            if (opacity <= 0.2f)
                continue;
            meanChromaticDifference += std::abs(pixel.x() - pixel.z()) / opacity;
            ++occupiedPixels;
        }
        ASSERT_GT(occupiedPixels, clouds.pixels.size() / 20);
        EXPECT_LT(meanChromaticDifference / static_cast<double>(occupiedPixels), 0.01)
            << "Neutral ground-level atmosphere must not acquire the blue clear-air color from above the clouds";
    }

    /// Verify that local cloud extinction can remain dark before aerial perspective blends it into the sky.
    TEST(AtmosphereShaderTest, nearbyDenseCloudsRetainDarkSelfShadowedRadiance)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 96;
        inputs.cloudParameters = { 0.82f, 1.8f, 16000.f, 42000.f };
        inputs.mistParameters = {};
        inputs.solarRadiance = {};
        inputs.skyIrradiance = { 0.08f, 0.1f, 0.14f };
        inputs.groundIrradiance = { 0.025f, 0.03f, 0.04f };
        inputs.viewDirection = { 1.f, 0.f, 0.24f };
        inputs.farPlane = 300000.f;
        inputs.fogNear = -9830.4f;
        inputs.fogFar = 88473.6f;
        inputs.weatherWeight = 0.f;
        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);

        double cloudLuminance = 0.0;
        std::size_t occupiedPixelCount = 0;
        for (const osg::Vec4f& pixel : clouds.pixels)
        {
            const float opacity = 1.f - pixel.w();
            if (opacity <= 0.2f)
                continue;
            const osg::Vec3f radiance = osg::Vec3f(pixel.x(), pixel.y(), pixel.z()) / opacity;
            cloudLuminance += radiance.x() * 0.2126 + radiance.y() * 0.7152 + radiance.z() * 0.0722;
            ++occupiedPixelCount;
        }

        ASSERT_GT(occupiedPixelCount, clouds.pixels.size() / 20);
        const double meanCloudLuminance = cloudLuminance / static_cast<double>(occupiedPixelCount);
        const double atmosphericTargetLuminance
            = inputs.fogColor.x() * 0.2126 + inputs.fogColor.y() * 0.7152 + inputs.fogColor.z() * 0.0722;
        EXPECT_LT(meanCloudLuminance, atmosphericTargetLuminance * 0.85)
            << "Dense nearby clouds must remain visibly darker than the surrounding distance atmosphere";
    }

    /// Convert the canonical unit-Lambert surface response to irradiance before applying a normalized cloud phase.
    TEST(AtmosphereShaderTest, cloudSolarIrradianceMatchesCanonicalLambertLightingUnits)
    {
        NativePassInputs inputs;
        inputs.width = 4;
        inputs.height = 4;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 9;
        inputs.cloudDensitySliceAxis = 0;
        inputs.sunDirection = { 0.f, 0.f, 1.f, 0.f };
        inputs.weatherWeight = 0.f;
        for (float intensity : { 0.f, 0.2f, 1.f, 2.f })
        {
            inputs.solarRadiance = osg::Vec3f(1.f, 0.6f, 0.2f) * intensity;
            const PassImage light = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_FALSE(light.pixels.empty());
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(light.pixels.front()[channel], inputs.solarRadiance[channel] * std::numbers::pi, 0.004);
        }
    }

    /// Integrate an upper hemisphere's isotropic sky fill without leaking direct sun around cloud shadows.
    TEST(AtmosphereShaderTest, cloudSkyFillUsesHemisphereSolidAngleWithoutDirectSunLeak)
    {
        NativePassInputs inputs;
        inputs.width = 4;
        inputs.height = 4;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 9;
        inputs.cloudDensitySliceAxis = 1;
        inputs.sunDirection = { 0.f, 0.f, 1.f, 0.f };
        inputs.weatherWeight = 0.f;
        for (const osg::Vec3f sky : { osg::Vec3f(), osg::Vec3f(0.1f, 0.3f, 0.6f), osg::Vec3f(1.f, 1.f, 1.f) })
        {
            inputs.skyIrradiance = sky;
            for (float solar : { 0.f, 1.f, 2.f })
            {
                inputs.solarRadiance = { solar, solar, solar };
                const PassImage light = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(light.pixels.empty());
                // The upper hemisphere subtends 2*pi steradians under an isotropic 1/(4*pi) phase.
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(light.pixels.front()[channel], sky[channel] * 0.5f, 0.001);
            }
        }
    }

    /// Preserve the forward-scattering peak toward the sun under either phase-function sign convention.
    TEST(AtmosphereShaderTest, opticallyThinCloudsScatterMoreTowardTheSunThanAway)
    {
        NativePassInputs inputs;
        inputs.cloudParameters = { 0.72f, 0.005f, 48000.f, 22000.f };
        inputs.mistParameters = {};
        inputs.skyIrradiance = {};
        inputs.groundIrradiance = {};
        inputs.solarRadiance = { 1.f, 1.f, 1.f };
        inputs.weatherWeight = 0.f;
        inputs.fogNear = 10000000.f;
        inputs.fogFar = 20000000.f;
        inputs.farPlane = 500000.f;
        inputs.viewDirection = { 1.f, 0.f, 0.25f };
        inputs.verticalFov = 10.f;
        inputs.sunDirection = { 1.f, 0.f, 0.25f, 0.f };
        const PassImage towardSun = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.sunDirection = { -1.f, 0.f, 0.25f, 0.f };
        const PassImage awayFromSun = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(towardSun.pixels.empty());
        ASSERT_EQ(towardSun.pixels.size(), awayFromSun.pixels.size());

        double towardRadiance = 0.0;
        double awayRadiance = 0.0;
        double opacitySum = 0.0;
        for (std::size_t index = 0; index < towardSun.pixels.size(); ++index)
        {
            towardRadiance += towardSun.pixels[index].x();
            awayRadiance += awayFromSun.pixels[index].x();
            opacitySum += 1.0 - towardSun.pixels[index].w();
            EXPECT_NEAR(towardSun.pixels[index].w(), awayFromSun.pixels[index].w(), 1e-6);
        }
        ASSERT_GT(opacitySum, 0.01);
        EXPECT_GT(towardRadiance, awayRadiance * 1.5)
            << "Droplet forward scattering must brighten backlit clouds without changing cloud occupancy";
    }

    TEST(AtmosphereShaderTest, lowSunWarmsCloudLightingIndependentlyOfDiscVisibility)
    {
        NativePassInputs lowSunInputs;
        lowSunInputs.cloudParameters = { 0.72f, 1.f, 48000.f, 22000.f };
        lowSunInputs.mistParameters = {};
        lowSunInputs.sunColor = { 1.f, 1.f, 1.f, 1.f };
        lowSunInputs.solarRadiance = { 1.f, 1.f, 1.f };
        lowSunInputs.sunDiscColor = { 1.f, 1.f, 1.f, 1.f };
        lowSunInputs.sunDirection = { 0.995f, 0.f, 0.1f, 0.f };
        lowSunInputs.fogNear = 1000000.f;
        lowSunInputs.fogFar = 2000000.f;
        const PassImage lowSun = renderNativeAtmospherePass("integrateAtmosphereRayGrid", lowSunInputs);
        ASSERT_FALSE(lowSun.pixels.empty());

        NativePassInputs highSunInputs = lowSunInputs;
        highSunInputs.sunDirection = { 0.6f, 0.f, 0.8f, 0.f };
        const PassImage highSun = renderNativeAtmospherePass("integrateAtmosphereRayGrid", highSunInputs);
        NativePassInputs hiddenSunInputs = lowSunInputs;
        hiddenSunInputs.sunVisibility = 0.f;
        const PassImage hiddenSun = renderNativeAtmospherePass("integrateAtmosphereRayGrid", hiddenSunInputs);
        ASSERT_EQ(highSun.pixels.size(), lowSun.pixels.size());
        ASSERT_EQ(hiddenSun.pixels.size(), lowSun.pixels.size());

        double lowSunRed = 0.0;
        double lowSunBlue = 0.0;
        double highSunRed = 0.0;
        double highSunBlue = 0.0;
        double hiddenSunLuminance = 0.0;
        double lowSunLuminance = 0.0;
        for (std::size_t index = 0; index < lowSun.pixels.size(); ++index)
        {
            lowSunRed += lowSun.pixels[index].x();
            lowSunBlue += lowSun.pixels[index].z();
            highSunRed += highSun.pixels[index].x();
            highSunBlue += highSun.pixels[index].z();
            lowSunLuminance += lowSun.pixels[index].x() * 0.2126 + lowSun.pixels[index].y() * 0.7152
                + lowSun.pixels[index].z() * 0.0722;
            hiddenSunLuminance += hiddenSun.pixels[index].x() * 0.2126 + hiddenSun.pixels[index].y() * 0.7152
                + hiddenSun.pixels[index].z() * 0.0722;
        }
        EXPECT_GT(lowSunRed / std::max(lowSunBlue, 1e-6), highSunRed / std::max(highSunBlue, 1e-6));
        EXPECT_NEAR(hiddenSunLuminance, lowSunLuminance, 1e-5);
    }

    /// Preserve weather-lit cloud brightness and warm sunset sunlight independently of diffuse shadow color.
    TEST(AtmosphereShaderTest, productionWeatherCloudsTrackSharedEnvironmentLighting)
    {
        /// Identify one canonical weather and time-of-day fixture combination.
        struct Scenario
        {
            std::string_view mWeather;
            std::string_view mTimeOfDay;
        };
        constexpr std::array scenarios{ Scenario{ "Clear", "Day" }, Scenario{ "Clear", "Sunrise" },
            Scenario{ "Clear", "Sunset" }, Scenario{ "Cloudy", "Day" }, Scenario{ "Cloudy", "Sunset" },
            Scenario{ "Cloudy", "Sunrise" }, Scenario{ "Foggy", "Day" }, Scenario{ "Overcast", "Day" } };
        constexpr std::array<std::string_view, 8> presets{ "Solstheim-Necro", "BC-Necro", "WG-Necro", "AC-Necro",
            "SG-Necro", "AL-Necro", "AI-Necro", "default" };

        for (std::string_view preset : presets)
        {
            for (const Scenario& scenario : scenarios)
            {
                if (preset != "default" && scenario.mWeather != "Clear")
                    continue;
                NativePassInputs inputs = makePackagedAtmosphereInputs(scenario.mWeather, scenario.mTimeOfDay, preset);
                inputs.width = 16;
                inputs.height = 16;
                inputs.mistParameters = {};
                inputs.fogNear = 12000.f;
                inputs.fogFar = 180000.f;
                const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(clouds.pixels.empty()) << preset << ' ' << scenario.mWeather << ' ' << scenario.mTimeOfDay;

                osg::Vec3d radianceSum;
                double opacitySum = 0.0;
                for (const osg::Vec4f& pixel : clouds.pixels)
                {
                    EXPECT_TRUE(std::isfinite(pixel.x()));
                    EXPECT_TRUE(std::isfinite(pixel.y()));
                    EXPECT_TRUE(std::isfinite(pixel.z()));
                    EXPECT_TRUE(std::isfinite(pixel.w()));
                    radianceSum += osg::Vec3d(pixel.x(), pixel.y(), pixel.z());
                    opacitySum += 1.0 - pixel.w();
                }
                ASSERT_GT(opacitySum, 1e-3) << preset << ' ' << scenario.mWeather << ' ' << scenario.mTimeOfDay;
                const osg::Vec3d meanCloudRadiance = radianceSum / opacitySum;
                const double meanLuminance = meanCloudRadiance * osg::Vec3d(0.2126, 0.7152, 0.0722);
                EXPECT_GT(meanLuminance, 0.05) << preset << ' ' << scenario.mWeather << ' ' << scenario.mTimeOfDay;

                if (scenario.mWeather == "Clear")
                {
                    const osg::Vec3d clearSky(inputs.skyColor.x(), inputs.skyColor.y(), inputs.skyColor.z());
                    const double clearSkyLuminance = clearSky * osg::Vec3d(0.2126, 0.7152, 0.0722);
                    const double minimumRelativeLuminance = scenario.mTimeOfDay == "Day" ? 0.72 : 0.62;
                    EXPECT_GT(meanLuminance, clearSkyLuminance * minimumRelativeLuminance)
                        << preset << ' ' << scenario.mTimeOfDay
                        << " clear-weather clouds must receive enough diffuse sky and multiply scattered sunlight "
                        << "to avoid reading as dark opacity against the rendered sky";
                }

                if (scenario.mWeather == "Clear" && scenario.mTimeOfDay == "Sunset")
                {
                    inputs.solarRadiance = {};
                    const PassImage diffuseClouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_EQ(diffuseClouds.pixels.size(), clouds.pixels.size());
                    osg::Vec3d diffuseRadianceSum;
                    for (std::size_t index = 0; index < clouds.pixels.size(); ++index)
                    {
                        const osg::Vec4f& diffuse = diffuseClouds.pixels[index];
                        EXPECT_NEAR(diffuse.w(), clouds.pixels[index].w(), 1e-5f);
                        diffuseRadianceSum += osg::Vec3d(diffuse.x(), diffuse.y(), diffuse.z());
                    }
                    const osg::Vec3d solarContribution = (radianceSum - diffuseRadianceSum) / opacitySum;
                    EXPECT_GT(solarContribution * osg::Vec3d(0.2126, 0.7152, 0.0722), 0.0001) << preset;
                    EXPECT_GT(solarContribution.x(), solarContribution.z() * 1.15) << preset;
                }
            }
        }
    }

    TEST(AtmosphereShaderTest, mistConvergesTowardTheWeatherAtmosphereColor)
    {
        NativePassInputs warmInputs;
        warmInputs.cloudParameters = {};
        warmInputs.mistParameters = { 2.5f, 800.f, 4500.f, 1.f };
        warmInputs.fogColor = { 0.82f, 0.38f, 0.22f, 1.f };
        warmInputs.skyColor = { 0.34f, 0.2f, 0.18f, 1.f };
        warmInputs.skyIrradiance = { 0.34f, 0.2f, 0.18f };
        warmInputs.groundIrradiance = { 0.508f, 0.263f, 0.194f };
        warmInputs.sunDirection = { 0.995f, 0.f, 0.1f, 0.f };
        const PassImage warm = renderNativeAtmospherePass("integrateAtmosphereRayGrid", warmInputs);
        ASSERT_FALSE(warm.pixels.empty());

        NativePassInputs coolInputs = warmInputs;
        coolInputs.fogColor = { 0.22f, 0.38f, 0.82f, 1.f };
        coolInputs.skyColor = { 0.18f, 0.2f, 0.34f, 1.f };
        coolInputs.skyIrradiance = { 0.18f, 0.2f, 0.34f };
        coolInputs.groundIrradiance = { 0.194f, 0.263f, 0.508f };
        const PassImage cool = renderNativeAtmospherePass("integrateAtmosphereRayGrid", coolInputs);
        ASSERT_EQ(cool.pixels.size(), warm.pixels.size());

        double warmRedBlue = 0.0;
        double coolRedBlue = 0.0;
        for (std::size_t index = 0; index < warm.pixels.size(); ++index)
        {
            warmRedBlue += warm.pixels[index].x() - warm.pixels[index].z();
            coolRedBlue += cool.pixels[index].x() - cool.pixels[index].z();
        }
        EXPECT_GT(warmRedBlue, coolRedBlue + 0.01);
    }

    TEST(AtmosphereShaderTest, executesVolumeDepthTerminationWithReverseZ)
    {
        NativePassInputs openSkyInputs;
        openSkyInputs.reverseZ = true;
        openSkyInputs.depth = [](int, int) { return 0.f; };
        openSkyInputs.previousDepth = [](int, int) { return 0.f; };
        openSkyInputs.cloudParameters = { 0.72f, 1.f, 48000.f, 22000.f };
        openSkyInputs.mistParameters = {};
        const PassImage openSky = renderNativeAtmospherePass("integrateAtmosphereRayGrid", openSkyInputs);
        ASSERT_FALSE(openSky.pixels.empty());

        const double openSkyExtinction = std::accumulate(openSky.pixels.begin(), openSky.pixels.end(), 0.0,
            [](double total, const osg::Vec4f& pixel) { return total + 1.0 - pixel.w(); });
        EXPECT_GT(openSkyExtinction / static_cast<double>(openSky.pixels.size()), 0.005);

        NativePassInputs occludedInputs = openSkyInputs;
        occludedInputs.depth = [](int, int) { return 1.f; };
        const PassImage occluded = renderNativeAtmospherePass("integrateAtmosphereRayGrid", occludedInputs);
        ASSERT_EQ(occluded.pixels.size(), openSky.pixels.size());
        for (const osg::Vec4f& pixel : occluded.pixels)
        {
            EXPECT_NEAR(pixel.x(), 0.f, 1e-6f);
            EXPECT_NEAR(pixel.y(), 0.f, 1e-6f);
            EXPECT_NEAR(pixel.z(), 0.f, 1e-6f);
            EXPECT_NEAR(pixel.w(), 1.f, 1e-6f);
        }
    }

    TEST(AtmosphereShaderTest, distantGeometryAtTheFarPlaneBoundaryOccludesCloudVolume)
    {
        constexpr float distantGeometryViewDistance = 19950.f;

        for (const bool reverseZ : { false, true })
            for (const bool useUniformBuffer : { false, true })
            {
                NativePassInputs inputs;
                inputs.reverseZ = reverseZ;
                inputs.useUniformBuffer = useUniformBuffer;
                inputs.depth
                    = [=](int, int) { return normalizedDepthForViewDistance(distantGeometryViewDistance, reverseZ); };
                inputs.previousDepth = inputs.depth;
                inputs.cloudParameters = { 0.72f, 1.f, 48000.f, 22000.f };
                inputs.mistParameters = {};

                const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(volume.pixels.empty());
                float maximumRadiance = 0.f;
                float minimumTransmittance = 1.f;
                for (const osg::Vec4f& pixel : volume.pixels)
                {
                    maximumRadiance = std::max({ maximumRadiance, pixel.x(), pixel.y(), pixel.z() });
                    minimumTransmittance = std::min(minimumTransmittance, pixel.w());
                }
                EXPECT_LT(maximumRadiance, 1e-6f)
                    << "reverseZ=" << reverseZ << ", useUniformBuffer=" << useUniformBuffer;
                EXPECT_NEAR(minimumTransmittance, 1.f, 1e-6f)
                    << "reverseZ=" << reverseZ << ", useUniformBuffer=" << useUniformBuffer;
            }
    }

    TEST(AtmosphereShaderTest, executesCloudVolumeWithRuntimeUniformBufferAndReverseZ)
    {
        NativePassInputs inputs;
        inputs.useUniformBuffer = true;
        inputs.reverseZ = true;
        inputs.depth = [](int, int) { return 0.f; };
        inputs.previousDepth = [](int, int) { return 0.f; };
        inputs.cloudParameters = { 0.72f, 1.f, 48000.f, 22000.f };
        inputs.mistParameters = {};

        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(clouds.pixels.empty());
        const double extinction = std::accumulate(clouds.pixels.begin(), clouds.pixels.end(), 0.0,
            [](double total, const osg::Vec4f& pixel) { return total + 1.0 - pixel.w(); });
        EXPECT_GT(extinction / static_cast<double>(clouds.pixels.size()), 0.005);
    }

    TEST(AtmosphereShaderTest, inheritsRuntimeUniformBufferForCloudVolume)
    {
        NativePassInputs inputs;
        inputs.useUniformBuffer = true;
        inputs.bindUniformBufferOnAncestor = true;
        inputs.reverseZ = true;
        inputs.depth = [](int, int) { return 0.f; };
        inputs.previousDepth = [](int, int) { return 0.f; };
        inputs.cloudParameters = { 0.72f, 1.f, 48000.f, 22000.f };
        inputs.mistParameters = {};

        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(clouds.pixels.empty());
        const double extinction = std::accumulate(clouds.pixels.begin(), clouds.pixels.end(), 0.0,
            [](double total, const osg::Vec4f& pixel) { return total + 1.0 - pixel.w(); });
        EXPECT_GT(extinction / static_cast<double>(clouds.pixels.size()), 0.005);
    }

    /// Verify solar occlusion through production-sized volume samples and their owning depth metadata.
    TEST(AtmosphereShaderTest, cloudVolumeAttenuatesAnalyticHdrSunSignal)
    {
        NativePassInputs cloudInputs;
        cloudInputs.cloudParameters = { 0.82f, 1.15f, 48000.f, 22000.f };
        cloudInputs.mistParameters = {};
        cloudInputs.sceneWidth = cloudInputs.width * 2;
        cloudInputs.sceneHeight = cloudInputs.height * 2;
        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", cloudInputs);
        ASSERT_FALSE(clouds.pixels.empty());
        const auto densestCloud = std::min_element(clouds.pixels.begin(), clouds.pixels.end(),
            [](const osg::Vec4f& left, const osg::Vec4f& right) { return left.w() < right.w(); });
        const std::size_t cloudIndex = static_cast<std::size_t>(std::distance(clouds.pixels.begin(), densestCloud));
        const int sunX = static_cast<int>(cloudIndex % static_cast<std::size_t>(clouds.width)) * 2 + 1;
        const int sunY = static_cast<int>(cloudIndex / static_cast<std::size_t>(clouds.width)) * 2 + 1;
        ASSERT_LT(densestCloud->w(), 0.98f);

        NativePassInputs metadataInputs = cloudInputs;
        const PassImage metadata = renderNativeAtmosphereMetadata(metadataInputs);
        ASSERT_EQ(metadata.pixels.size(), clouds.pixels.size());

        NativePassInputs clearCompositeInputs;
        clearCompositeInputs.width = cloudInputs.sceneWidth;
        clearCompositeInputs.height = cloudInputs.sceneHeight;
        clearCompositeInputs.lastShader = [sunX, sunY](int x, int y) {
            const bool sunCore = std::abs(x - sunX) <= 3 && std::abs(y - sunY) <= 3;
            return sunCore ? osg::Vec4f(8.f, 6.8f, 5.2f, 1.f) : osg::Vec4f(0.3f, 0.4f, 0.55f, 1.f);
        };
        clearCompositeInputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        clearCompositeInputs.shaftInput = [](int, int) { return osg::Vec4f(); };
        const PassImage clearComposite = renderNativeAtmospherePass("composite", clearCompositeInputs);

        NativePassInputs cloudyCompositeInputs = clearCompositeInputs;
        cloudyCompositeInputs.volumeHistoryInput = [&clouds](int x, int y) { return clouds.at(x, y); };
        cloudyCompositeInputs.volumeHistoryWidth = clouds.width;
        cloudyCompositeInputs.volumeHistoryHeight = clouds.height;
        cloudyCompositeInputs.volumeMetadataInput = [&metadata](int x, int y) { return metadata.at(x, y); };
        cloudyCompositeInputs.volumeMetadataWidth = metadata.width;
        cloudyCompositeInputs.volumeMetadataHeight = metadata.height;
        const PassImage cloudyComposite = renderNativeAtmospherePass("composite", cloudyCompositeInputs);
        ASSERT_EQ(cloudyComposite.pixels.size(), clearComposite.pixels.size());

        const auto luminance
            = [](const osg::Vec4f& color) { return color.x() * 0.2126f + color.y() * 0.7152f + color.z() * 0.0722f; };
        const osg::Vec4f& clearSun = clearComposite.at(sunX, sunY);
        const osg::Vec4f& cloudySun = cloudyComposite.at(sunX, sunY);
        EXPECT_LT(luminance(cloudySun), luminance(clearSun) * 0.98f);
    }

    TEST(AtmosphereShaderTest, mistUsesCollectedPointLightsOnlyAtSupportedQuality)
    {
        NativePassInputs unlitInputs;
        unlitInputs.mistParameters = { 2.5f, 800.f, 4500.f, 1.f };
        const PassImage unlit = renderNativeAtmospherePass("integrateAtmosphereRayGrid", unlitInputs);
        ASSERT_FALSE(unlit.pixels.empty());

        NativePassInputs litInputs = unlitInputs;
        litInputs.pointLightCount = 1;
        litInputs.pointLights[0] = { 1200.f, 0.f, 1200.f, 0.f };
        litInputs.pointLights[1] = { 8.f, 2.f, 1.f, 1.f };
        litInputs.pointLights[2] = { 1.f, 0.f, 0.f, 5000.f };
        const PassImage lit = renderNativeAtmospherePass("integrateAtmosphereRayGrid", litInputs);
        ASSERT_EQ(lit.pixels.size(), unlit.pixels.size());

        NativePassInputs lowQualityUnlitInputs = unlitInputs;
        lowQualityUnlitInputs.quality = 1;
        const PassImage lowQualityUnlit
            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", lowQualityUnlitInputs);
        NativePassInputs lowQualityLitInputs = litInputs;
        lowQualityLitInputs.quality = 1;
        const PassImage lowQualityLit = renderNativeAtmospherePass("integrateAtmosphereRayGrid", lowQualityLitInputs);
        ASSERT_EQ(lowQualityLit.pixels.size(), lowQualityUnlit.pixels.size());

        const auto averageRed = [](const PassImage& image) {
            double red = 0.0;
            for (const osg::Vec4f& pixel : image.pixels)
                red += pixel.x();
            return red / static_cast<double>(image.pixels.size());
        };
        EXPECT_GT(averageRed(lit), averageRed(unlit) + 0.01);
        EXPECT_NEAR(averageRed(lowQualityLit), averageRed(lowQualityUnlit), 1e-6);
    }

    /// Keep a cloud edge's reconstructed color independent of changing coverage and stale illumination.
    TEST(AtmosphereShaderTest, sunsetCloudEdgesPreserveRadianceAcrossCoverageChanges)
    {
        const osg::Vec3f cloudRadiance(0.1f, 0.12f, 0.2f);
        for (const float historicalOpacity : { 0.05f, 0.5f, 0.95f })
            for (const float historicalBrightness : { 0.25f, 1.f, 2.f })
            {
                SCOPED_TRACE(historicalOpacity);
                SCOPED_TRACE(historicalBrightness);
                NativePassInputs inputs;
                inputs.width = 8;
                inputs.height = 8;
                inputs.volumeResolutionScale = 1.f;
                inputs.quality = 3;
                inputs.historyValid = true;
                inputs.passInput = [cloudRadiance](int x, int) {
                    const float opacity = x % 2 == 0 ? 0.8f : 0.2f;
                    return osg::Vec4f(cloudRadiance * opacity, 1.f - opacity);
                };
                inputs.volumeMetadataInput = [](int, int) {
                    const float distance = encodeAtmosphereDistance(20000.f);
                    return osg::Vec4f(distance, 0.5f, distance, 2.f);
                };
                inputs.atmosphereMetadataHistory = inputs.volumeMetadataInput;
                inputs.atmosphereHistory = [=](int, int) {
                    return osg::Vec4f(
                        cloudRadiance * (historicalBrightness * historicalOpacity), 1.f - historicalOpacity);
                };
                const PassImage result = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
                ASSERT_FALSE(result.pixels.empty());
                const osg::Vec4f pixel = result.at(4, 4);
                const float opacity = 1.f - pixel.a();
                ASSERT_GT(opacity, 0.1f);
                ASSERT_LT(opacity, 0.9f);
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(pixel[channel] / opacity, cloudRadiance[channel], 0.002f);
            }
    }

    /// Resolve a vanished cloud to transparent sky without retaining color or producing invalid values.
    TEST(AtmosphereShaderTest, vanishedCloudHistoryConvergesToTransparentSky)
    {
        NativePassInputs inputs;
        inputs.width = 8;
        inputs.height = 8;
        inputs.volumeResolutionScale = 1.f;
        inputs.quality = 3;
        inputs.historyValid = true;
        inputs.passInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        inputs.volumeMetadataInput = [](int, int) {
            const float distance = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(distance, 0.f, distance, 2.f);
        };
        inputs.atmosphereMetadataHistory = inputs.volumeMetadataInput;
        inputs.atmosphereHistory = [](int, int) { return osg::Vec4f(0.1f, 0.12f, 0.2f, 0.5f); };
        const PassImage result = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(result.pixels.empty());
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(result.at(4, 4)[channel], channel == 3 ? 1.f : 0.f, 0.001f);
    }

    TEST(AtmosphereShaderTest, temporalHistoryPreservesStableSpatialExtrema)
    {
        NativePassInputs inputs;
        inputs.width = 16;
        inputs.height = 16;
        inputs.sceneWidth = 32;
        inputs.sceneHeight = 32;
        inputs.historyValid = true;
        inputs.passInput = [](int x, int y) {
            const float value = (x < 8 && y < 8) || (x >= 8 && y >= 8) ? 0.9f : 0.1f;
            return osg::Vec4f(value, value * 2.f, value * 0.5f, 1.f - value);
        };
        inputs.atmosphereHistory = inputs.passInput;
        inputs.volumeMetadataInput = [](int, int) {
            const float distance = encodeAtmosphereDistance(50000.f);
            return osg::Vec4f(distance, 1.f, encodeAtmosphereDistance(20000.f), 2.f);
        };
        inputs.atmosphereMetadataHistory = inputs.volumeMetadataInput;
        const PassImage result = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(result.pixels.empty());
        for (int y = 0; y < inputs.height; ++y)
            for (int x = 0; x < inputs.width; ++x)
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_NEAR(result.at(x, y)[channel], inputs.passInput(x, y)[channel], 0.002f);
    }

    /// Match history motion to the distinct formation-field advection of mist, fog, and clouds.
    TEST(AtmosphereShaderTest, atmosphericMediaUseTheirFormationAdvectionScales)
    {
        const auto scaleFor = [](float mistWeight, float fogWeight, float mediumType) {
            NativePassInputs inputs;
            inputs.width = 32;
            inputs.height = 24;
            inputs.sceneWidth = 64;
            inputs.sceneHeight = 48;
            inputs.historyValid = true;
            inputs.renderReprojectionCoordinates = true;
            inputs.cloudAdvectionDeltaWorldUnits = { 0.f, 96.f };
            inputs.mistParameters.x() = mistWeight;
            inputs.fogVolumeParameters.x() = fogWeight;
            inputs.volumeMetadataInput = [mediumType](int, int) {
                return osg::Vec4f(
                    encodeAtmosphereDistance(5000.f), 0.5f, encodeAtmosphereDistance(20000.f), mediumType);
            };
            const PassImage result = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
            EXPECT_FALSE(result.pixels.empty());
            return result.at(inputs.width / 2, inputs.height / 2).b();
        };

        EXPECT_FLOAT_EQ(scaleFor(0.f, 0.f, 0.f), 0.f);
        EXPECT_FLOAT_EQ(scaleFor(1.f, 0.f, 1.f), 1.f);
        EXPECT_NEAR(scaleFor(0.f, 1.f, 1.f), 0.32f, 0.001f);
        EXPECT_FLOAT_EQ(scaleFor(0.f, 0.f, 2.f), 0.f);
    }

    /// Keep copied radiance paired with its nearest categorical medium metadata during subpixel reprojection.
    TEST(AtmosphereShaderTest, untracedHistoryDoesNotBlendAcrossMediumBoundaries)
    {
        const osg::Vec4f faintMist(0.1f, 0.08f, 0.06f, 0.95f);
        const osg::Vec4f brightCloud(0.9f, 0.8f, 0.7f, 0.2f);
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.previousEyeOffset = { 0.f, 10.f, 0.f };
        inputs.depth = inputs.previousDepth = [](int, int) { return normalizedDepthForViewDistance(5000.f, false); };
        inputs.passInput = [faintMist](int, int) { return faintMist; };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(1000.f), 0.05f, encodeAtmosphereDistance(5000.f), 1.f);
        };
        inputs.atmosphereHistory = [faintMist, brightCloud](int x, int) { return x < 32 ? faintMist : brightCloud; };
        inputs.atmosphereMetadataHistory = [](int x, int) {
            return osg::Vec4f(encodeAtmosphereDistance(x < 32 ? 1000.f : 18000.f), x < 32 ? 0.05f : 0.8f,
                encodeAtmosphereDistance(5000.f), x < 32 ? 1.f : 2.f);
        };

        const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojected.pixels.empty());
        std::size_t copiedPixels = 0;
        std::size_t blendedPixels = 0;
        for (int y = 2; y < inputs.height - 2; ++y)
            for (int x = 2; x < inputs.width - 2; ++x)
            {
                const int phase = (inputs.frameNumber + ((x / 2) + (y / 2)) % 2) % 4;
                const bool traced = x % 2 == phase % 2 && y % 2 == phase / 2;
                if (traced)
                    continue;
                const float red = reprojected.at(x, y).r();
                if (std::min(std::abs(red - faintMist.r()), std::abs(red - brightCloud.r())) >= 0.002f)
                    ++blendedPixels;
                ++copiedPixels;
            }
        EXPECT_GT(copiedPixels, 0u);
        EXPECT_EQ(blendedPixels, 0u);
    }

    /// Reject filtered clear-sky history that is unsupported by the current medium neighborhood.
    TEST(AtmosphereShaderTest, clearSkyHistoryRejectsUnsupportedMediumBoundaryMixtures)
    {
        const osg::Vec4f clearAir(0.08f, 0.1f, 0.14f, 0.98f);
        const osg::Vec4f denseCloud(0.9f, 0.8f, 0.7f, 0.2f);
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.previousEyeOffset = { 0.f, 10.f, 0.f };
        inputs.depth = inputs.previousDepth = [](int, int) { return 1.f; };
        inputs.passInput = [clearAir](int, int) { return clearAir; };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(60000.f), 0.02f, encodeAtmosphereDistance(60000.f), 0.f);
        };
        inputs.atmosphereHistory = [clearAir, denseCloud](int x, int) { return x < 32 ? clearAir : denseCloud; };
        inputs.atmosphereMetadataHistory = [](int x, int) {
            return osg::Vec4f(encodeAtmosphereDistance(x < 32 ? 60000.f : 18000.f), x < 32 ? 0.02f : 0.8f,
                encodeAtmosphereDistance(20000.f), x < 32 ? 0.f : 2.f);
        };

        const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojected.pixels.empty());
        std::size_t copiedPixels = 0;
        std::size_t blendedPixels = 0;
        for (int y = 2; y < inputs.height - 2; ++y)
            for (int x = 2; x < inputs.width - 2; ++x)
            {
                const int phase = (inputs.frameNumber + ((x / 2) + (y / 2)) % 2) % 4;
                const bool traced = x % 2 == phase % 2 && y % 2 == phase / 2;
                if (traced)
                    continue;
                const float red = reprojected.at(x, y).r();
                if (std::min(std::abs(red - clearAir.r()), std::abs(red - denseCloud.r())) >= 0.002f)
                    ++blendedPixels;
                ++copiedPixels;
            }
        EXPECT_GT(copiedPixels, 0u);
        EXPECT_EQ(blendedPixels, 0u);
    }

    /// Preserve validated untraced cloud history until its own ray refreshes.
    TEST(AtmosphereShaderTest, untracedCloudHistoryDoesNotClampAgainstOtherCurrentRays)
    {
        const osg::Vec4f currentCloud(0.2f, 0.24f, 0.28f, 0.8f);
        const osg::Vec4f previousCloud(0.9f, 0.85f, 0.8f, 0.1f);
        NativePassInputs inputs;
        inputs.width = 32;
        inputs.height = 24;
        inputs.sceneWidth = 64;
        inputs.sceneHeight = 48;
        inputs.historyValid = true;
        inputs.depth = inputs.previousDepth = [](int, int) { return 1.f; };
        inputs.passInput = [currentCloud](int, int) { return currentCloud; };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.2f, encodeAtmosphereDistance(60000.f), 2.f);
        };
        inputs.atmosphereHistory = [previousCloud](int, int) { return previousCloud; };
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.2f, encodeAtmosphereDistance(20000.f), 2.f);
        };

        const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojected.pixels.empty());
        std::size_t copiedPixels = 0;
        for (int y = 2; y < inputs.height - 2; ++y)
            for (int x = 2; x < inputs.width - 2; ++x)
            {
                const int phase = (inputs.frameNumber + ((x / 2) + (y / 2)) % 2) % 4;
                const bool traced = x % 2 == phase % 2 && y % 2 == phase / 2;
                if (traced)
                    continue;
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_NEAR(reprojected.at(x, y)[channel], previousCloud[channel], 0.002f);
                ++copiedPixels;
            }
        EXPECT_GT(copiedPixels, 0u);
    }

    /// Mark a traced checkerboard ray for exact repair when its raw endpoint disagrees with current geometry.
    TEST(AtmosphereShaderTest, tracedRayRepairsRawSceneTerminationMismatch)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.depth = inputs.previousDepth = [](int, int) { return normalizedDepthForViewDistance(5000.f, false); };
        inputs.passInput = [](int, int) { return osg::Vec4f(0.8f, 0.75f, 0.7f, 0.1f); };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.9f, encodeAtmosphereDistance(20000.f), 2.f);
        };

        PassImage metadata;
        renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs, &metadata);
        ASSERT_FALSE(metadata.pixels.empty());
        // Frame four selects the even/even member of block zero as its traced sample.
        EXPECT_LT(metadata.at(0, 0).a(), 0.f);
    }

    /// Reject history whose paired scene termination came from clear sky rather than current opaque geometry.
    TEST(AtmosphereShaderTest, geometryRejectsHistoryFromDifferentSceneTermination)
    {
        const osg::Vec4f currentMist(0.01f, 0.012f, 0.014f, 0.9995f);
        const osg::Vec4f staleCloud(0.8f, 0.75f, 0.7f, 0.1f);
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.depth = inputs.previousDepth = [](int, int) { return normalizedDepthForViewDistance(5000.f, false); };
        inputs.passInput = [currentMist](int, int) { return currentMist; };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(1000.f), 0.0005f, encodeAtmosphereDistance(5000.f), 1.f);
        };
        inputs.atmosphereHistory = [staleCloud](int, int) { return staleCloud; };
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.9f, encodeAtmosphereDistance(20000.f), 2.f);
        };

        const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojected.pixels.empty());
        for (const osg::Vec4f& pixel : reprojected.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(pixel[channel], currentMist[channel], 0.002f);
    }

    /// Reject history terminated by old geometry when the current ray has become clear sky.
    TEST(AtmosphereShaderTest, clearSkyRejectsHistoryFromOpaqueSceneTermination)
    {
        const osg::Vec4f currentCloud(0.1f, 0.12f, 0.14f, 0.8f);
        const osg::Vec4f staleGeometryVolume(0.8f, 0.75f, 0.7f, 0.1f);
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.depth = inputs.previousDepth = [](int, int) { return 1.f; };
        inputs.passInput = [currentCloud](int, int) { return currentCloud; };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.2f, encodeAtmosphereDistance(20000.f), 2.f);
        };
        inputs.atmosphereHistory = [staleGeometryVolume](int, int) { return staleGeometryVolume; };
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(1000.f), 0.9f, encodeAtmosphereDistance(5000.f), 1.f);
        };

        const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojected.pixels.empty());
        for (const osg::Vec4f& pixel : reprojected.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(pixel[channel], currentCloud[channel], 0.002f);
    }

    /// Reject a history sample whose participating medium lies behind its nominal opaque endpoint.
    TEST(AtmosphereShaderTest, geometryRejectsHistoryMediumBehindSceneTermination)
    {
        const osg::Vec4f currentMist(0.01f, 0.012f, 0.014f, 0.9995f);
        const osg::Vec4f staleCloud(0.8f, 0.75f, 0.7f, 0.1f);
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.depth = inputs.previousDepth = [](int, int) { return normalizedDepthForViewDistance(5000.f, false); };
        inputs.passInput = [currentMist](int, int) { return currentMist; };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(1000.f), 0.0005f, encodeAtmosphereDistance(5000.f), 1.f);
        };
        inputs.atmosphereHistory = [staleCloud](int, int) { return staleCloud; };
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.9f, encodeAtmosphereDistance(5000.f), 2.f);
        };

        const PassImage reprojected = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojected.pixels.empty());
        for (const osg::Vec4f& pixel : reprojected.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(pixel[channel], currentMist[channel], 0.002f);
    }

    /// Preserve a copied history sample's validated endpoint instead of relabeling it as current geometry.
    TEST(AtmosphereShaderTest, copiedHistoryRetainsItsValidatedSceneTermination)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 48;
        inputs.sceneWidth = 128;
        inputs.sceneHeight = 96;
        inputs.historyValid = true;
        inputs.depth = inputs.previousDepth = [](int, int) { return normalizedDepthForViewDistance(5100.f, false); };
        inputs.passInput = [](int, int) { return osg::Vec4f(0.1f, 0.08f, 0.06f, 0.95f); };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(1000.f), 0.05f, encodeAtmosphereDistance(5100.f), 1.f);
        };
        inputs.atmosphereHistory = inputs.passInput;
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(1000.f), 0.05f, encodeAtmosphereDistance(5000.f), 1.f);
        };

        PassImage metadata;
        renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs, &metadata);
        ASSERT_FALSE(metadata.pixels.empty());
        // This center-adjacent pixel is untraced at frame zero and its perspective-correct current
        // termination remains within the production compatibility tolerance of the 5,000-unit history.
        EXPECT_NEAR(metadata.at(33, 24).b(), encodeAtmosphereDistance(5000.f), 0.001f);
    }

    /// Reproject an analytic world plane while preserving independently marked current checkerboard samples.
    TEST(AtmosphereShaderTest, translatedCloudHistoryPreservesIndependentRefreshes)
    {
        for (const bool reverseZ : { false, true })
            for (const bool uniformBuffer : { false, true })
            {
                NativePassInputs inputs;
                inputs.width = 64;
                inputs.height = 48;
                inputs.sceneWidth = 128;
                inputs.sceneHeight = 96;
                inputs.reverseZ = reverseZ;
                inputs.useUniformBuffer = uniformBuffer;
                inputs.historyValid = true;
                inputs.previousEyeOffset = { 0.f, -1800.f, 0.f };
                inputs.depth = inputs.previousDepth = [reverseZ](int, int) { return reverseZ ? 0.f : 1.f; };
                const double slope = std::tan(osg::DegreesToRadians(double(inputs.verticalFov)) * 0.5);
                const auto ray = [&](int x, int y) {
                    const float horizontal
                        = float((2.0 * (x + 0.5) / inputs.width - 1.0) * slope * inputs.width / inputs.height);
                    const float vertical = float((2.0 * (y + 0.5) / inputs.height - 1.0) * slope);
                    return osg::Vec3f(1.f, -horizontal, vertical);
                };
                const auto color = [&](int x, int y, float eyeY) {
                    const float value = 0.5f + (ray(x, y).y() * 50000.f + eyeY) / 200000.f;
                    return osg::Vec4f(value, value * 0.5f, value * 0.25f, 1.f - value);
                };
                inputs.passInput = [&](int x, int y) {
                    const float refreshSignal = (x + y) % 2 == 0 ? 0.003f : -0.003f;
                    return color(x, y, 0.f) + osg::Vec4f(refreshSignal, refreshSignal, refreshSignal, refreshSignal);
                };
                inputs.atmosphereHistory = [&](int x, int y) { return color(x, y, -1800.f); };
                inputs.volumeMetadataInput = [&](int x, int y) {
                    return osg::Vec4f(encodeAtmosphereDistance(50000.f * ray(x, y).length()), 0.5f,
                        encodeAtmosphereDistance(20000.f), 2.f);
                };
                inputs.atmosphereMetadataHistory = inputs.volumeMetadataInput;
                const PassImage result = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
                ASSERT_FALSE(result.pixels.empty());
                for (int y = 2; y < inputs.height - 2; ++y)
                    for (int x = 2; x < inputs.width - 2; ++x)
                    {
                        const int phase = (inputs.frameNumber + ((x / 2) + (y / 2)) % 2) % 4;
                        const bool traced = x % 2 == phase % 2 && y % 2 == phase / 2;
                        const osg::Vec4f expected = traced ? inputs.passInput(x, y) : color(x, y, 0.f);
                        for (int channel = 0; channel < 4; ++channel)
                            EXPECT_NEAR(result.at(x, y)[channel], expected[channel], 0.001f);
                    }
            }
    }

    /// Accumulate valid mist history while refreshed rays replace former clouds and repair disocclusions.
    TEST(AtmosphereShaderTest, temporalMistRejectsDisocclusionAndConvergesOnStableDepth)
    {
        NativePassInputs currentInputs;
        currentInputs.passInput = [](int x, int y) {
            const float value = static_cast<float>(x) / 63.f;
            return osg::Vec4f(value, value, value, value);
        };
        currentInputs.volumeMetadataInput = [](int, int) {
            const float encodedFar = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(encodedFar, 1.f, encodedFar, 1.f);
        };
        const PassImage current = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", currentInputs);
        ASSERT_FALSE(current.pixels.empty());

        NativePassInputs stableInputs = currentInputs;
        stableInputs.historyValid = true;
        stableInputs.atmosphereHistory = [](int, int) { return osg::Vec4f(0.55f, 0.55f, 0.55f, 0.55f); };
        stableInputs.atmosphereMetadataHistory = currentInputs.volumeMetadataInput;
        const PassImage stable = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", stableInputs);
        ASSERT_EQ(stable.pixels.size(), current.pixels.size());

        NativePassInputs disoccludedInputs = stableInputs;
        disoccludedInputs.previousDepth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 1.f : 0.f; };
        const PassImage disoccluded = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", disoccludedInputs);
        ASSERT_EQ(disoccluded.pixels.size(), current.pixels.size());
        NativePassInputs cameraCutInputs = stableInputs;
        cameraCutInputs.previousEyeOffset = { 0.f, 50000.f, 0.f };
        const PassImage cameraCut = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", cameraCutInputs);
        ASSERT_EQ(cameraCut.pixels.size(), current.pixels.size());
        NativePassInputs translatedInputs = stableInputs;
        translatedInputs.previousEyeOffset = { 0.f, 100.f, 0.f };
        const PassImage translated = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", translatedInputs);
        ASSERT_EQ(translated.pixels.size(), current.pixels.size());
        NativePassInputs fovCutInputs = stableInputs;
        fovCutInputs.previousVerticalFov = 20.0;
        const PassImage fovCut = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", fovCutInputs);
        ASSERT_EQ(fovCut.pixels.size(), current.pixels.size());
        NativePassInputs largeAdvectionInputs = stableInputs;
        largeAdvectionInputs.cloudAdvectionDeltaWorldUnits = { 2500.f, 0.f };
        const PassImage largeAdvection
            = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", largeAdvectionInputs);
        ASSERT_EQ(largeAdvection.pixels.size(), current.pixels.size());
        NativePassInputs changedMediumInputs = stableInputs;
        changedMediumInputs.atmosphereMetadataHistory = [](int, int) {
            const float encodedFar = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(encodedFar, 1.f, encodedFar, 2.f);
        };
        const PassImage changedMedium
            = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", changedMediumInputs);
        ASSERT_EQ(changedMedium.pixels.size(), current.pixels.size());

        const osg::Vec4f& rawCenter = current.at(current.width / 2, current.height / 2);
        const osg::Vec4f& stableCenter = stable.at(stable.width / 2, stable.height / 2);
        const osg::Vec4f& disoccludedCenter = disoccluded.at(disoccluded.width / 2, disoccluded.height / 2);
        const osg::Vec4f& cameraCutCenter = cameraCut.at(cameraCut.width / 2, cameraCut.height / 2);
        const osg::Vec4f& translatedCenter = translated.at(translated.width / 2, translated.height / 2);
        const osg::Vec4f& changedMediumCenter = changedMedium.at(changedMedium.width / 2, changedMedium.height / 2);
        const osg::Vec4f& largeAdvectionCenter = largeAdvection.at(largeAdvection.width / 2, largeAdvection.height / 2);
        EXPECT_LT(std::abs(stableCenter.x() - 0.55f), std::abs(rawCenter.x() - 0.55f));
        EXPECT_NEAR(disoccludedCenter.x(), rawCenter.x(), 0.002f);
        EXPECT_NEAR(disoccludedCenter.w(), rawCenter.w(), 0.002f);
        EXPECT_NEAR(cameraCutCenter.x(), rawCenter.x(), 0.002f);
        EXPECT_LT(std::abs(translatedCenter.x() - 0.55f), std::abs(rawCenter.x() - 0.55f));
        EXPECT_LT(std::abs(translatedCenter.w() - 0.55f), std::abs(rawCenter.w() - 0.55f));
        EXPECT_NEAR(largeAdvectionCenter.x(), rawCenter.x(), 0.002f);
        EXPECT_NEAR(changedMediumCenter.x(), rawCenter.x(), 0.002f);
        EXPECT_NEAR(changedMediumCenter.w(), rawCenter.w(), 0.002f);
        const osg::Vec4f& rawEdge = current.at(56, current.height / 2);
        const osg::Vec4f& stableEdge = stable.at(56, stable.height / 2);
        const osg::Vec4f& fovCutEdge = fovCut.at(56, fovCut.height / 2);
        EXPECT_GT(std::abs(stableEdge.x() - rawEdge.x()), 0.005f);
        EXPECT_NEAR(fovCutEdge.x(), rawEdge.x(), 0.002f);
    }

    TEST(AtmosphereShaderTest, temporalHistoryRejectsRapidLightingChangesAtSilhouettes)
    {
        const osg::Vec4f currentLitVolume(0.82f, 0.48f, 0.2f, 0.42f);
        const osg::Vec4f previousDarkVolume(0.025f, 0.018f, 0.014f, 0.42f);
        for (const bool reverseZ : { false, true })
            for (const bool useUniformBuffer : { false, true })
            {
                NativePassInputs inputs;
                inputs.reverseZ = reverseZ;
                inputs.useUniformBuffer = useUniformBuffer;
                inputs.historyValid = true;
                inputs.historyLightingWeight = 0.f;
                inputs.depth = [=](int, int) { return reverseZ ? 0.f : 1.f; };
                inputs.previousDepth = inputs.depth;
                inputs.passInput = [=](int x, int y) {
                    const bool silhouettePixel = std::abs(x - 32) <= 1 && std::abs(y - 32) <= 1;
                    return silhouettePixel ? currentLitVolume : previousDarkVolume;
                };
                inputs.atmosphereHistory = [=](int, int) { return previousDarkVolume; };
                inputs.volumeMetadataInput = [](int, int) {
                    const float encodedCloudDistance = encodeAtmosphereDistance(15000.f);
                    const float encodedSceneDistance = encodeAtmosphereDistance(20000.f);
                    return osg::Vec4f(encodedCloudDistance, 0.58f, encodedSceneDistance, 2.f);
                };
                inputs.atmosphereMetadataHistory = inputs.volumeMetadataInput;

                const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
                ASSERT_FALSE(resolved.pixels.empty());
                const osg::Vec4f& center = resolved.at(32, 32);
                EXPECT_GT(center.x(), currentLitVolume.x() * 0.9f)
                    << "reverseZ=" << reverseZ << " useUniformBuffer=" << useUniformBuffer;
                EXPECT_GT(center.y(), currentLitVolume.y() * 0.9f)
                    << "reverseZ=" << reverseZ << " useUniformBuffer=" << useUniformBuffer;
                EXPECT_GT(center.z(), currentLitVolume.z() * 0.9f)
                    << "reverseZ=" << reverseZ << " useUniformBuffer=" << useUniformBuffer;
                EXPECT_NEAR(center.w(), currentLitVolume.w(), 0.002f)
                    << "reverseZ=" << reverseZ << " useUniformBuffer=" << useUniformBuffer;
            }
    }

    TEST(AtmosphereShaderTest, highTimescaleDawnMovementDoesNotLeaveDarkAtmosphereTraces)
    {
        constexpr int width = 48;
        constexpr int height = 27;
        const auto currentTerrain = [](int x, int y) {
            const int ridgeHeight = 13 + std::max(0, 12 - std::abs(x - width / 2) / 2);
            return y < ridgeHeight;
        };
        const auto previousTerrain = [](int x, int y) {
            const int ridgeHeight = 13 + std::max(0, 12 - std::abs(x - (width / 2 - 3)) / 2);
            return y < ridgeHeight;
        };
        const auto currentDepth
            = [=](int x, int y) { return currentTerrain(x, y) ? normalizedDepthForViewDistance(14000.f, false) : 1.f; };
        const auto previousDepth = [=](int x, int y) {
            return previousTerrain(x, y) ? normalizedDepthForViewDistance(14000.f, false) : 1.f;
        };

        NativePassInputs previousInputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        previousInputs.width = width;
        previousInputs.height = height;
        previousInputs.depth = previousDepth;
        previousInputs.eyePosition = { 0.f, -180.f, 1200.f };
        previousInputs.frameNumber = 40;
        const PassImage previousRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", previousInputs);
        NativePassInputs previousMetadataInputs = previousInputs;
        const PassImage previousMetadata = renderNativeAtmosphereMetadata(previousMetadataInputs);

        MWRender::CloudAdvectionController advectionController;
        advectionController.update(1.f / 60.f, false, 1.25f, 10000.f);
        const MWRender::CloudAdvectionState& advection = advectionController.getState();
        NativePassInputs currentInputs = makePackagedAtmosphereInputs("Cloudy", "Day");
        currentInputs.width = width;
        currentInputs.height = height;
        currentInputs.depth = currentDepth;
        currentInputs.previousDepth = previousDepth;
        currentInputs.previousEyeOffset = { 0.f, -180.f, 0.f };
        currentInputs.cloudAdvectionOffsetWorldUnits = advection.mOffsetWorldUnits;
        currentInputs.cloudAdvectionDeltaWorldUnits = advection.mFrameDeltaWorldUnits;
        currentInputs.frameNumber = 41;
        currentInputs.historyValid = true;
        MWRender::AtmosphereHistoryLightingController lightingController;
        const auto lightingState = [](const NativePassInputs& inputs) {
            return MWRender::AtmosphereHistoryLightingState{ inputs.solarRadiance, inputs.skyIrradiance,
                inputs.groundIrradiance, inputs.inScattering, inputs.outScattering,
                osg::Vec3f(inputs.skyColor.x(), inputs.skyColor.y(), inputs.skyColor.z()),
                osg::Vec3f(inputs.fogColor.x(), inputs.fogColor.y(), inputs.fogColor.z()),
                osg::Vec3f(inputs.sunDirection.x(), inputs.sunDirection.y(), inputs.sunDirection.z()) };
        };
        EXPECT_FLOAT_EQ(lightingController.update(lightingState(previousInputs), true), 0.f);
        currentInputs.historyLightingWeight = lightingController.update(lightingState(currentInputs), true);
        EXPECT_LT(currentInputs.historyLightingWeight, 0.05f);
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", currentInputs);
        NativePassInputs currentMetadataInputs = currentInputs;
        const PassImage currentMetadata = renderNativeAtmosphereMetadata(currentMetadataInputs);

        NativePassInputs resolveInputs = currentInputs;
        resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        resolveInputs.atmosphereHistory = [&previousRaw](int x, int y) { return previousRaw.at(x, y); };
        resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
        resolveInputs.atmosphereMetadataHistory
            = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        ASSERT_EQ(resolved.pixels.size(), currentRaw.pixels.size());

        const auto luminance
            = [](const osg::Vec4f& color) { return color.x() * 0.2126f + color.y() * 0.7152f + color.z() * 0.0722f; };
        float maximumRelativeDarkening = 0.f;
        std::size_t comparedPixelCount = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const float currentLuminance = luminance(currentRaw.at(x, y));
                if (currentLuminance < 0.025f || currentTerrain(x, y))
                    continue;
                const float resolvedLuminance = luminance(resolved.at(x, y));
                maximumRelativeDarkening = std::max(maximumRelativeDarkening,
                    (currentLuminance - resolvedLuminance) / std::max(currentLuminance, 0.025f));
                ++comparedPixelCount;
            }
        EXPECT_GT(comparedPixelCount, 64u);
        EXPECT_LT(maximumRelativeDarkening, 0.15f);
    }

    /// Retain useful cloud history during continuous density changes and reject it at scene disocclusions.
    TEST(AtmosphereShaderTest, temporalCloudHistoryResolvesChangingDensity)
    {
        NativePassInputs firstFrameInputs;
        firstFrameInputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        firstFrameInputs.mistParameters = {};
        firstFrameInputs.frameNumber = 4;
        const PassImage firstFrame = renderNativeAtmospherePass("integrateAtmosphereRayGrid", firstFrameInputs);
        const PassImage repeatedFirstFrame = renderNativeAtmospherePass("integrateAtmosphereRayGrid", firstFrameInputs);
        ASSERT_EQ(firstFrame.pixels.size(), repeatedFirstFrame.pixels.size());

        NativePassInputs stableNonTemporalInputs = firstFrameInputs;
        stableNonTemporalInputs.frameNumber = 5;
        const PassImage stableNonTemporalFrame
            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", stableNonTemporalInputs);
        ASSERT_EQ(firstFrame.pixels.size(), stableNonTemporalFrame.pixels.size());

        NativePassInputs secondFrameInputs = firstFrameInputs;
        secondFrameInputs.frameNumber = 5;
        secondFrameInputs.historyValid = true;
        secondFrameInputs.cloudParameters.y() *= 1.02f;
        const PassImage secondFrame = renderNativeAtmospherePass("integrateAtmosphereRayGrid", secondFrameInputs);
        ASSERT_EQ(firstFrame.pixels.size(), secondFrame.pixels.size());

        double deterministicDifference = 0.0;
        double stableNonTemporalDifference = 0.0;
        double densityChangeDifference = 0.0;
        for (std::size_t index = 0; index < firstFrame.pixels.size(); ++index)
            for (int channel = 0; channel < 4; ++channel)
            {
                deterministicDifference
                    += std::abs(firstFrame.pixels[index][channel] - repeatedFirstFrame.pixels[index][channel]);
                stableNonTemporalDifference
                    += std::abs(firstFrame.pixels[index][channel] - stableNonTemporalFrame.pixels[index][channel]);
                densityChangeDifference
                    += std::abs(firstFrame.pixels[index][channel] - secondFrame.pixels[index][channel]);
            }
        EXPECT_LT(deterministicDifference, 1e-6);
        EXPECT_LT(stableNonTemporalDifference, 1e-6);
        EXPECT_GT(densityChangeDifference, 0.01);

        NativePassInputs resolveInputs = secondFrameInputs;
        resolveInputs.historyValid = true;
        resolveInputs.passInput = [&secondFrame](int x, int y) { return secondFrame.at(x, y); };
        resolveInputs.atmosphereHistory = [&firstFrame](int x, int y) { return firstFrame.at(x, y); };
        NativePassInputs currentMetadataInputs = secondFrameInputs;
        const PassImage currentMetadata = renderNativeAtmosphereMetadata(currentMetadataInputs);
        NativePassInputs previousMetadataInputs = firstFrameInputs;
        const PassImage previousMetadata = renderNativeAtmosphereMetadata(previousMetadataInputs);
        resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
        resolveInputs.atmosphereMetadataHistory
            = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        ASSERT_EQ(resolved.pixels.size(), secondFrame.pixels.size());

        double historyContribution = 0.0;
        for (std::size_t index = 0; index < resolved.pixels.size(); ++index)
            for (int channel = 0; channel < 4; ++channel)
                historyContribution += std::abs(resolved.pixels[index][channel] - secondFrame.pixels[index][channel]);
        EXPECT_GT(historyContribution, 0.001);

        NativePassInputs disoccludedInputs = resolveInputs;
        disoccludedInputs.previousDepth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 1.f : 0.f; };
        const PassImage disoccluded = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", disoccludedInputs);
        ASSERT_EQ(disoccluded.pixels.size(), secondFrame.pixels.size());
        for (std::size_t index = 0; index < disoccluded.pixels.size(); ++index)
        {
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(disoccluded.pixels[index][channel], secondFrame.pixels[index][channel], 0.002f);
        }
    }

    TEST(AtmosphereShaderTest, lightingRejectedHistoryUsesStableVolumeSampling)
    {
        const auto meanAbsoluteDifference = [](const PassImage& first, const PassImage& second) {
            EXPECT_EQ(first.pixels.size(), second.pixels.size());
            double difference = 0.0;
            for (std::size_t pixelIndex = 0; pixelIndex < first.pixels.size(); ++pixelIndex)
                for (int channel = 0; channel < 4; ++channel)
                    difference += std::abs(first.pixels[pixelIndex][channel] - second.pixels[pixelIndex][channel]);
            return difference / static_cast<double>(first.pixels.size() * 4);
        };

        for (const bool cloudsEnabled : { false, true })
            for (const bool excessiveAdvection : { false, true })
            {
                NativePassInputs firstInputs
                    = makePackagedAtmosphereInputs(cloudsEnabled ? "Cloudy" : "Rain", "Sunset");
                firstInputs.width = 96;
                firstInputs.height = 54;
                firstInputs.cloudParameters = cloudsEnabled ? firstInputs.cloudParameters : osg::Vec4f();
                firstInputs.mistParameters = cloudsEnabled ? osg::Vec4f() : firstInputs.mistParameters;
                firstInputs.historyValid = true;
                firstInputs.historyLightingWeight = excessiveAdvection ? 1.f : 0.f;
                firstInputs.cloudAdvectionDeltaWorldUnits = excessiveAdvection ? osg::Vec2f(2500.f, 0.f) : osg::Vec2f();
                firstInputs.frameNumber = 120;
                const PassImage first = renderNativeAtmospherePass("integrateAtmosphereRayGrid", firstInputs);
                ASSERT_FALSE(first.pixels.empty());

                NativePassInputs secondInputs = firstInputs;
                secondInputs.frameNumber = 121;
                const PassImage second = renderNativeAtmospherePass("integrateAtmosphereRayGrid", secondInputs);
                ASSERT_EQ(first.pixels.size(), second.pixels.size());

                EXPECT_LT(meanAbsoluteDifference(first, second), 1e-6)
                    << "cloudsEnabled=" << cloudsEnabled << " excessiveAdvection=" << excessiveAdvection;
            }
    }

    TEST(AtmosphereShaderTest, productionScaledRainHistoryConvergesAtTheHorizon)
    {
        constexpr int width = 96;
        constexpr int height = 54;
        NativePassInputs inputs = makePackagedAtmosphereInputs("Rain", "Sunset");
        inputs.width = width;
        inputs.height = height;
        inputs.eyePosition = { 0.f, 0.f, 900.f };
        inputs.lastShader = [](int x, int y) {
            const float heightWeight = static_cast<float>(y) / static_cast<float>(height - 1);
            const float horizonVariation = 0.015f * std::sin(static_cast<float>(x) * 0.17f);
            return osg::Vec4f(0.42f + horizonVariation, 0.46f + horizonVariation, 0.51f + heightWeight * 0.08f, 1.f);
        };
        inputs.depth = [](int x, int y) {
            const int distantRidge = 20 + static_cast<int>(3.f * std::sin(static_cast<float>(x) * 0.14f));
            return y <= distantRidge ? normalizedDepthForViewDistance(17500.f, false) : 1.f;
        };
        inputs.previousDepth = inputs.depth;

        ProductionAtmosphereFrame previousFrame;
        double rawVariation = 0.0;
        double resolvedVariation = 0.0;
        std::size_t comparedValues = 0;
        for (int frameNumber = 0; frameNumber < 8; ++frameNumber)
        {
            inputs.frameNumber = frameNumber;
            inputs.historyValid = frameNumber > 0;
            ProductionAtmosphereFrame frame
                = renderProductionAtmosphereVolumeFrame(inputs, frameNumber > 0 ? &previousFrame : nullptr);
            ASSERT_FALSE(frame.mResolvedVolume.pixels.empty());
            if (frameNumber >= 4)
            {
                for (std::size_t pixelIndex = 0; pixelIndex < frame.mRawVolume.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        rawVariation += std::abs(frame.mRawVolume.pixels[pixelIndex][channel]
                            - previousFrame.mRawVolume.pixels[pixelIndex][channel]);
                        resolvedVariation += std::abs(frame.mResolvedVolume.pixels[pixelIndex][channel]
                            - previousFrame.mResolvedVolume.pixels[pixelIndex][channel]);
                        ++comparedValues;
                    }
            }
            previousFrame = std::move(frame);
        }

        ASSERT_GT(comparedValues, 0u);
        const double meanRawVariation = rawVariation / static_cast<double>(comparedValues);
        const double meanResolvedVariation = resolvedVariation / static_cast<double>(comparedValues);
        EXPECT_GT(meanRawVariation, 1e-5);
        EXPECT_LT(meanResolvedVariation, meanRawVariation * 0.45);
        EXPECT_LT(meanResolvedVariation, 0.003);
    }

    /// Read the immutable published frame instead of the updater's next pending frame in either uniform path.
    TEST(AtmosphereShaderTest, diagnosticFrameIdentityMatchesPublishedUniformSnapshot)
    {
        for (const bool useUniformBuffer : { false, true })
        {
            Fx::StateUpdater updater(useUniformBuffer);
            osg::ref_ptr<osg::StateSet> snapshot = new osg::StateSet;
            EXPECT_FALSE(Fx::StateUpdater::getPublishedFrameNumber(*snapshot).has_value());
            updater.setFrameNumber(17);
            updater.setDefaults(snapshot);
            updater.apply(snapshot, nullptr);
            updater.setFrameNumber(18);
            EXPECT_EQ(Fx::StateUpdater::getPublishedFrameNumber(*snapshot), 17);
            updater.apply(snapshot, nullptr);
            EXPECT_EQ(Fx::StateUpdater::getPublishedFrameNumber(*snapshot), 18);
        }
    }

    /// Capture every cloud control from the published binding, independently of later pending weather changes.
    TEST(AtmosphereShaderTest, diagnosticCloudModelsMatchPublishedUniformSnapshot)
    {
        SceneUtil::CloudLayerUniforms expected;
        for (unsigned int index = 0; index < expected.size(); ++index)
            expected[index] = osg::Vec4f(static_cast<float>(index), .25f, .5f, .75f);
        for (const bool useUniformBuffer : { false, true })
        {
            Fx::StateUpdater updater(useUniformBuffer);
            osg::ref_ptr<osg::StateSet> snapshot = new osg::StateSet;
            EXPECT_FALSE(Fx::StateUpdater::getPublishedCloudLayers(*snapshot).has_value());
            updater.setCloudLayers(expected);
            updater.setDefaults(snapshot);
            updater.apply(snapshot, nullptr);
            updater.setCloudLayers({});
            EXPECT_EQ(Fx::StateUpdater::getPublishedCloudLayers(*snapshot), expected);
            updater.apply(snapshot, nullptr);
            EXPECT_EQ(Fx::StateUpdater::getPublishedCloudLayers(*snapshot), SceneUtil::CloudLayerUniforms{});
        }
    }

    /// Read fixed world terrain from the rendered binding after the weather owner has advanced its cache.
    TEST(AtmosphereShaderTest, diagnosticTerrainMatchesPublishedUniformSnapshot)
    {
        SceneUtil::AtmosphereTerrainGrid expected;
        expected.update({ 100000.f, -50000.f }, [](const osg::Vec2f& p) { return p.x() * 0.02f + p.y() * 0.01f; });
        for (const bool useUniformBuffer : { false, true })
        {
            Fx::StateUpdater updater(useUniformBuffer);
            osg::ref_ptr<osg::StateSet> snapshot = new osg::StateSet;
            EXPECT_FALSE(Fx::StateUpdater::getPublishedAtmosphereTerrain(*snapshot).has_value());
            updater.setAtmosphereTerrain(expected);
            updater.setDefaults(snapshot);
            updater.apply(snapshot, nullptr);
            updater.setAtmosphereTerrain({});
            const auto terrain = Fx::StateUpdater::getPublishedAtmosphereTerrain(*snapshot);
            ASSERT_TRUE(terrain.has_value());
            EXPECT_EQ(terrain->mHeights, expected.mHeights);
            EXPECT_EQ(terrain->mOriginSpacing, expected.mOriginSpacing);
            EXPECT_EQ(terrain->mHeightBounds, expected.mHeightBounds);
        }
    }

    /// Verify the real sparse trace visits each dense ray once, without moving its dither or optical model.
    TEST(AtmosphereShaderTest, checkerboardFourPhasesMatchDenseRayGrid)
    {
        NativePassInputs denseInputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        denseInputs.width = 32;
        denseInputs.height = 24;
        denseInputs.sceneWidth = 64;
        denseInputs.sceneHeight = 48;
        denseInputs.viewDirection = { 1.f, 0.f, 0.35f };
        std::array<int, 32 * 24> visits{};
        for (int phase = 0; phase < 4; ++phase)
        {
            denseInputs.frameNumber = phase;
            PassImage denseMetadata;
            const PassImage dense
                = renderNativeAtmospherePass("integrateAtmosphereRayGrid", denseInputs, &denseMetadata);
            NativePassInputs sparseInputs = denseInputs;
            sparseInputs.width /= 2;
            sparseInputs.height /= 2;
            PassImage sparseMetadata;
            const PassImage sparse
                = renderNativeAtmospherePass("integrateAtmosphereVolume", sparseInputs, &sparseMetadata);
            ASSERT_EQ(sparse.pixels.size() * 4, dense.pixels.size());
            for (int y = 0; y < sparse.height; ++y)
                for (int x = 0; x < sparse.width; ++x)
                {
                    const int index = (phase + (x + y) % 2) % 4;
                    const int denseX = x * 2 + index % 2;
                    const int denseY = y * 2 + index / 2;
                    ++visits[denseY * dense.width + denseX];
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        EXPECT_NEAR(sparse.at(x, y)[channel], dense.at(denseX, denseY)[channel], 0.002f);
                        EXPECT_NEAR(
                            sparseMetadata.at(x, y)[channel], denseMetadata.at(denseX, denseY)[channel], 0.002f);
                    }
                }
        }
        for (const int count : visits)
            EXPECT_EQ(count, 1);
    }

    /// Mark only untraced reset pixels for repair and never expose the intermediate sentinel as final metadata.
    TEST(AtmosphereShaderTest, checkerboardRepairMaskPreservesTracedPixelsAndOddEdges)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        inputs.width = 17;
        inputs.height = 13;
        inputs.sceneWidth = 34;
        inputs.sceneHeight = 26;
        inputs.historyValid = false;
        for (int phase = 0; phase < 4; ++phase)
        {
            inputs.frameNumber = phase;
            PassImage intermediateMetadata;
            const PassImage intermediate
                = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs, &intermediateMetadata);
            NativePassInputs repairInputs = inputs;
            repairInputs.resolveReprojectedVolume = true;
            repairInputs.passInput = [&intermediate](int x, int y) { return intermediate.at(x, y); };
            repairInputs.volumeMetadataInput
                = [&intermediateMetadata](int x, int y) { return intermediateMetadata.at(x, y); };
            repairInputs.volumeMetadataWidth = inputs.width;
            repairInputs.volumeMetadataHeight = inputs.height;
            PassImage finalMetadata;
            const PassImage repaired
                = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", repairInputs, &finalMetadata);
            ASSERT_EQ(intermediate.pixels.size(), repaired.pixels.size());
            for (int y = 0; y < inputs.height; ++y)
                for (int x = 0; x < inputs.width; ++x)
                {
                    const int member = (phase + (x / 2 + y / 2) % 2) % 4;
                    const bool traced = x < inputs.width / 2 * 2 && y < inputs.height / 2 * 2 && x % 2 == member % 2
                        && y % 2 == member / 2;
                    EXPECT_EQ(intermediateMetadata.at(x, y).a() < 0.f, !traced);
                    EXPECT_GE(finalMetadata.at(x, y).a(), 0.f);
                    EXPECT_LE(finalMetadata.at(x, y).a(), 2.f);
                    if (traced)
                    {
                        EXPECT_EQ(repaired.at(x, y), intermediate.at(x, y));
                        EXPECT_EQ(finalMetadata.at(x, y), intermediateMetadata.at(x, y));
                    }
                }
        }
    }

    /// Require fresh reconstruction to match dense integration even at incomplete checkerboard edge blocks.
    TEST(AtmosphereShaderTest, checkerboardResetReconstructsEveryPixelIncludingOddEdges)
    {
        for (const bool reverseZ : { false, true })
            for (const bool uniformBuffer : { false, true })
            {
                NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
                inputs.width = 66;
                inputs.height = 50;
                inputs.reverseZ = reverseZ;
                inputs.useUniformBuffer = uniformBuffer;
                inputs.depth = inputs.previousDepth = [reverseZ](int, int) { return reverseZ ? 0.f : 1.f; };
                inputs.viewDirection = { 1.f, 0.f, 0.35f };
                const ProductionAtmosphereFrame frame = renderProductionAtmosphereFrame(inputs);
                NativePassInputs denseInputs = inputs;
                denseInputs.sceneWidth = inputs.width;
                denseInputs.sceneHeight = inputs.height;
                denseInputs.width /= 2;
                denseInputs.height /= 2;
                PassImage denseMetadata;
                const PassImage dense
                    = renderNativeAtmospherePass("integrateAtmosphereRayGrid", denseInputs, &denseMetadata);
                ASSERT_EQ(frame.mResolvedVolume.pixels.size(), dense.pixels.size());
                for (std::size_t index = 0; index < dense.pixels.size(); ++index)
                {
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        EXPECT_NEAR(frame.mResolvedVolume.pixels[index][channel], dense.pixels[index][channel], 0.002f);
                        EXPECT_NEAR(
                            frame.mMetadata.pixels[index][channel], denseMetadata.pixels[index][channel], 0.002f);
                    }
                    EXPECT_NEAR(
                        frame.mMetadata.pixels[index].g(), 1.f - frame.mResolvedVolume.pixels[index].a(), 0.001f);
                }
            }
    }

    TEST(AtmosphereShaderTest, productionPipelineUsesConfiguredQualityRenderScales)
    {
        constexpr int sceneWidth = 80;
        constexpr int sceneHeight = 40;
        for (const int quality : { 0, 1, 2 })
        {
            NativePassInputs inputs;
            inputs.width = sceneWidth;
            inputs.height = sceneHeight;
            inputs.quality = quality;
            inputs.atmosphereEnabled = false;
            const ProductionAtmosphereFrame frame = renderProductionAtmosphereFrame(inputs);
            const float volumeScale = MWRender::getAtmosphereVolumeResolutionScale(quality);
            EXPECT_EQ(frame.mRawVolume.width, static_cast<int>(sceneWidth * volumeScale * 0.5f));
            EXPECT_EQ(frame.mRawVolume.height, static_cast<int>(sceneHeight * volumeScale * 0.5f));
            EXPECT_EQ(frame.mRawMetadata.width, frame.mRawVolume.width);
            EXPECT_EQ(frame.mRawMetadata.height, frame.mRawVolume.height);
            EXPECT_EQ(frame.mResolvedVolume.width, static_cast<int>(sceneWidth * volumeScale));
            EXPECT_EQ(frame.mResolvedVolume.height, static_cast<int>(sceneHeight * volumeScale));
            EXPECT_EQ(frame.mMetadata.width, frame.mResolvedVolume.width);
            EXPECT_EQ(frame.mMetadata.height, frame.mResolvedVolume.height);
            EXPECT_EQ(frame.mShafts.width, static_cast<int>(sceneWidth * volumeScale * 0.5f));
            EXPECT_EQ(frame.mShafts.height, static_cast<int>(sceneHeight * volumeScale * 0.5f));
            EXPECT_EQ(frame.mComposite.width, sceneWidth);
            EXPECT_EQ(frame.mComposite.height, sceneHeight);
        }
    }

    TEST(AtmosphereShaderTest, productionCompositePreservesRadianceAtSubpixelFoliageDepth)
    {
        constexpr int width = 192;
        constexpr int height = 96;
        const PassImage foliageMask = renderDistantFoliageCard(true);
        ASSERT_EQ(foliageMask.width, width);
        ASSERT_EQ(foliageMask.height, height);

        NativePassInputs inputs = makePackagedAtmosphereInputs("Rain", "Sunset");
        inputs.width = width;
        inputs.height = height;
        inputs.quality = 2;
        inputs.cloudParameters = {};
        inputs.eyePosition = { 0.f, 0.f, 900.f };
        inputs.lastShader = [](int, int) { return osg::Vec4f(0.08f, 0.075f, 0.07f, 1.f); };
        inputs.depth = [&foliageMask](int x, int y) {
            const bool foliageSurface = foliageMask.at(x, y).w() > 0.5f;
            return normalizedDepthForViewDistance(foliageSurface ? 12000.f : 18000.f, false);
        };
        inputs.previousDepth = inputs.depth;
        const auto luminance
            = [](const osg::Vec4f& color) { return color.x() * 0.2126f + color.y() * 0.7152f + color.z() * 0.0722f; };

        const ProductionAtmosphereFrame frame = renderProductionAtmosphereFrame(inputs);
        ASSERT_EQ(frame.mComposite.width, width);
        ASSERT_EQ(frame.mComposite.height, height);
        const float sourceLuminance = luminance(inputs.lastShader(0, 0));
        double accumulatedSurfaceIncrease = 0.0;
        float minimumSurfaceLuminance = std::numeric_limits<float>::max();
        std::size_t surfacePixelCount = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                if (foliageMask.at(x, y).w() <= 0.5f)
                    continue;
                const osg::Vec4f& composite = frame.mComposite.at(x, y);
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_TRUE(std::isfinite(composite[channel]));
                const float compositeLuminance = luminance(composite);
                minimumSurfaceLuminance = std::min(minimumSurfaceLuminance, compositeLuminance);
                accumulatedSurfaceIncrease += compositeLuminance - sourceLuminance;
                ++surfacePixelCount;
            }
        ASSERT_GT(surfacePixelCount, 100u);
        EXPECT_GE(minimumSurfaceLuminance, sourceLuminance - 1e-4f);
        EXPECT_GT(accumulatedSurfaceIncrease / static_cast<double>(surfacePixelCount), 0.002);
    }

    TEST(AtmosphereShaderTest, rapidProductionSunsetCloudsRemainLitAndStableWhenHistoryIsRejected)
    {
        constexpr int width = 64;
        constexpr int height = 36;
        NativePassInputs day = makePackagedAtmosphereInputs("Cloudy", "Day");
        NativePassInputs sunset = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        day.cloudParameters.x() = 0.92f;
        sunset.cloudParameters.x() = 0.92f;
        const auto interpolate = [](const osg::Vec3f& first, const osg::Vec3f& second, float factor) {
            return first * (1.f - factor) + second * factor;
        };
        const auto interpolate4 = [](const osg::Vec4f& first, const osg::Vec4f& second, float factor) {
            return first * (1.f - factor) + second * factor;
        };
        const auto lightingState = [](const NativePassInputs& inputs) {
            return MWRender::AtmosphereHistoryLightingState{ inputs.solarRadiance, inputs.skyIrradiance,
                inputs.groundIrradiance, inputs.inScattering, inputs.outScattering,
                osg::Vec3f(inputs.skyColor.x(), inputs.skyColor.y(), inputs.skyColor.z()),
                osg::Vec3f(inputs.fogColor.x(), inputs.fogColor.y(), inputs.fogColor.z()),
                osg::Vec3f(inputs.sunDirection.x(), inputs.sunDirection.y(), inputs.sunDirection.z()) };
        };
        const auto meanAbsoluteDifference = [](const PassImage& first, const PassImage& second) {
            double difference = 0.0;
            for (std::size_t pixelIndex = 0; pixelIndex < first.pixels.size(); ++pixelIndex)
                for (int channel = 0; channel < 4; ++channel)
                    difference += std::abs(first.pixels[pixelIndex][channel] - second.pixels[pixelIndex][channel]);
            return difference / static_cast<double>(first.pixels.size() * 4);
        };

        MWRender::AtmosphereHistoryLightingController lightingController;
        ProductionAtmosphereFrame previousFrame;
        std::size_t rejectedFrameCount = 0;
        bool verifiedRejectedFrame = false;
        for (int step = 0; step <= 5; ++step)
        {
            const float factor = static_cast<float>(step) / 5.f;
            NativePassInputs inputs = day;
            inputs.width = width;
            inputs.height = height;
            inputs.eyePosition = { 0.f, 0.f, 900.f };
            inputs.mistParameters = {};
            inputs.skyColor = interpolate4(day.skyColor, sunset.skyColor, factor);
            inputs.fogColor = interpolate4(day.fogColor, sunset.fogColor, factor);
            inputs.sunColor = interpolate4(day.sunColor, sunset.sunColor, factor);
            inputs.sunDiscColor = interpolate4(day.sunDiscColor, sunset.sunDiscColor, factor);
            inputs.solarRadiance = interpolate(day.solarRadiance, sunset.solarRadiance, factor);
            inputs.skyIrradiance = interpolate(day.skyIrradiance, sunset.skyIrradiance, factor);
            inputs.groundIrradiance = interpolate(day.groundIrradiance, sunset.groundIrradiance, factor);
            inputs.inScattering = interpolate(day.inScattering, sunset.inScattering, factor);
            inputs.outScattering = interpolate(day.outScattering, sunset.outScattering, factor);
            inputs.sunDirection = interpolate4(day.sunDirection, sunset.sunDirection, factor);
            osg::Vec3f normalizedSunDirection(
                inputs.sunDirection.x(), inputs.sunDirection.y(), inputs.sunDirection.z());
            normalizedSunDirection.normalize();
            inputs.sunDirection = osg::Vec4f(normalizedSunDirection, 0.f);
            inputs.historyValid = step > 0;
            inputs.historyLightingWeight = lightingController.update(lightingState(inputs), true);
            inputs.frameNumber = 300 + step;

            const ProductionAtmosphereFrame frame
                = renderProductionAtmosphereVolumeFrame(inputs, step > 0 ? &previousFrame : nullptr);
            ASSERT_FALSE(frame.mRawVolume.pixels.empty());
            std::size_t cloudPixelCount = 0;
            for (const osg::Vec4f& pixel : frame.mRawVolume.pixels)
            {
                for (int channel = 0; channel < 4; ++channel)
                {
                    EXPECT_TRUE(std::isfinite(pixel[channel]));
                    EXPECT_GE(pixel[channel], -1e-6f);
                }
                const float opacity = 1.f - pixel.w();
                if (opacity <= 0.03f)
                    continue;
                ++cloudPixelCount;
                const float meanRadiance = (pixel.x() * 0.2126f + pixel.y() * 0.7152f + pixel.z() * 0.0722f) / opacity;
                EXPECT_GT(meanRadiance, 0.005f);
            }
            EXPECT_GT(cloudPixelCount, 12u);

            if (inputs.historyValid && inputs.historyLightingWeight < 0.5f)
            {
                ++rejectedFrameCount;
                if (!verifiedRejectedFrame)
                {
                    NativePassInputs repeatedInputs = inputs;
                    repeatedInputs.frameNumber += 41;
                    const ProductionAtmosphereFrame repeated
                        = renderProductionAtmosphereVolumeFrame(repeatedInputs, &previousFrame);
                    EXPECT_LT(meanAbsoluteDifference(frame.mResolvedVolume, repeated.mResolvedVolume), 1e-6)
                        << "step=" << step << " historyLightingWeight=" << inputs.historyLightingWeight;
                    verifiedRejectedFrame = true;
                }
            }
            previousFrame = frame;
        }
        EXPECT_GE(rejectedFrameCount, 3u);
        EXPECT_TRUE(verifiedRejectedFrame);
    }

    TEST(AtmosphereShaderTest, classifiesOpenSkyCloudsAtTheirPhysicalDistanceBeyondTheFarPlane)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 64;
        inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        inputs.mistParameters = {};
        inputs.frameNumber = 0;
        const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(raw.pixels.empty());

        NativePassInputs classifyInputs = inputs;
        const PassImage metadata = renderNativeAtmosphereMetadata(classifyInputs);
        ASSERT_EQ(metadata.pixels.size(), raw.pixels.size());

        std::size_t classifiedCloudPixelCount = 0;
        for (std::size_t index = 0; index < raw.pixels.size(); ++index)
        {
            if (1.f - raw.pixels[index].w() < 0.01f)
                continue;
            ++classifiedCloudPixelCount;
            EXPECT_GT(metadata.pixels[index].w(), 1.5f);
            EXPECT_GT(metadata.pixels[index].x(), encodeAtmosphereDistance(20000.f));
        }
        EXPECT_GT(classifiedCloudPixelCount, 16u);
    }

    /// Verify analytic foreground composition below the opaque-ray early-termination threshold.
    TEST(AtmosphereShaderTest, integratedMetadataPreservesOrderedMistCloudMoments)
    {
        NativePassInputs inputs;
        inputs.width = 32;
        inputs.height = 24;
        inputs.cloudParameters = { 1.f, 0.12f, 55000.f, 18000.f };
        inputs.mistParameters.x() = 0.2f;
        inputs.viewDirection = { 1.f, 0.f, 0.3f };
        inputs.historyValid = false;
        for (const bool uniformBuffer : { false, true })
        {
            inputs.useUniformBuffer = uniformBuffer;
            PassImage combinedMetadata;
            const PassImage combined
                = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &combinedMetadata);
            NativePassInputs cloudInputs = inputs;
            cloudInputs.mistParameters = {};
            PassImage cloudMetadata;
            const PassImage clouds
                = renderNativeAtmospherePass("integrateAtmosphereRayGrid", cloudInputs, &cloudMetadata);
            NativePassInputs mistInputs = inputs;
            mistInputs.cloudParameters = {};
            PassImage mistMetadata;
            const PassImage mist = renderNativeAtmospherePass("integrateAtmosphereRayGrid", mistInputs, &mistMetadata);
            ASSERT_FALSE(combined.pixels.empty());
            ASSERT_EQ(cloudMetadata.pixels.size(), combined.pixels.size());
            ASSERT_EQ(mistMetadata.pixels.size(), combined.pixels.size());
            ASSERT_EQ(combinedMetadata.pixels.size(), combined.pixels.size());
            std::size_t mixedPixels = 0;
            for (std::size_t index = 0; index < combined.pixels.size(); ++index)
            {
                const float foregroundOpacity = mistMetadata.pixels[index].y();
                const float backgroundOpacity = (1.f - foregroundOpacity) * cloudMetadata.pixels[index].y();
                if (foregroundOpacity < 0.01f || backgroundOpacity < 0.01f)
                    continue;
                ++mixedPixels;
                const float mistDistance = std::exp2(mistMetadata.pixels[index].x() * 20.f) - 1.f;
                const float cloudDistance = std::exp2(cloudMetadata.pixels[index].x() * 20.f) - 1.f;
                const float expectedDistance = (foregroundOpacity * mistDistance + backgroundOpacity * cloudDistance)
                    / (foregroundOpacity + backgroundOpacity);
                EXPECT_GT(combined.pixels[index].w(), 0.01f);
                EXPECT_NEAR(combinedMetadata.pixels[index].x(), encodeAtmosphereDistance(expectedDistance), 0.0015f);
                EXPECT_NEAR(combinedMetadata.pixels[index].y(), 1.f - combined.pixels[index].w(), 0.001f);
                EXPECT_NEAR(combined.pixels[index].w(), mist.pixels[index].w() * clouds.pixels[index].w(), 0.001f);
                EXPECT_LT(combinedMetadata.pixels[index].x(), cloudMetadata.pixels[index].x());
            }
            EXPECT_GT(mixedPixels, 16u);
        }
    }

    TEST(AtmosphereShaderTest, integratedMetadataTracksCurrentDensityWithoutInventingHistorySupport)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 64;
        inputs.cloudParameters = { 0.3f, 0.48f, 48000.f, 22000.f };
        inputs.mistParameters = {};
        inputs.historyValid = true;
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(60000.f), 1.f, encodeAtmosphereDistance(20000.f), 2.f);
        };
        for (int frame = 4; frame < 8; ++frame)
        {
            inputs.frameNumber = frame;
            inputs.previousEyeOffset = frame > 5 ? osg::Vec3f(3000.f, -4000.f, 0.f) : osg::Vec3f();
            if (frame == 7)
                inputs.cloudParameters = {};
            PassImage metadata;
            const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
            ASSERT_FALSE(raw.pixels.empty());
            ASSERT_EQ(metadata.pixels.size(), raw.pixels.size());
            std::size_t contributingPixels = 0;
            for (std::size_t index = 0; index < raw.pixels.size(); ++index)
            {
                const float opacity = 1.f - raw.pixels[index].w();
                const auto& sample = metadata.pixels[index];
                EXPECT_NEAR(sample.y(), opacity, 0.001f);
                if (opacity > 0.001f)
                {
                    ++contributingPixels;
                    EXPECT_EQ(sample.w(), 2.f);
                    EXPECT_GT(sample.x(), encodeAtmosphereDistance(20000.f));
                }
                if (sample.y() == 0.f)
                    EXPECT_EQ(sample.w(), 0.f);
                if (frame == 7)
                {
                    EXPECT_EQ(sample.y(), 0.f);
                    EXPECT_EQ(sample.w(), 0.f);
                }
            }
            if (frame < 7)
                EXPECT_GT(contributingPixels, 16u);
            else
                EXPECT_EQ(contributingPixels, 0u);
        }
    }

    /// Publish the current cloud integral when a previously mist-dominated ray reveals a cloud.
    TEST(AtmosphereShaderTest, cloudRefreshReplacesFormerlyDominantMist)
    {
        const osg::Vec4f brightCloud(0.8f, 0.85f, 0.9f, 0.1f);
        const osg::Vec4f faintMist(0.1f, 0.13f, 0.15f, 0.9f);
        const osg::Vec4f historyVolume(0.35f, 0.4f, 0.45f, 0.6f);
        const float distance = encodeAtmosphereDistance(18000.f);
        const float termination = encodeAtmosphereDistance(20000.f);
        NativePassInputs inputs;
        inputs.historyValid = true;
        inputs.passInput = [brightCloud, faintMist](int x, int y) {
            return x >= 31 && x <= 32 && y >= 31 && y <= 32 ? brightCloud : faintMist;
        };
        inputs.volumeMetadataInput
            = [distance, termination](int, int) { return osg::Vec4f(distance, 0.9f, termination, 2.f); };
        inputs.atmosphereHistory = [historyVolume](int, int) { return historyVolume; };
        inputs.atmosphereMetadataHistory
            = [distance, termination](int, int) { return osg::Vec4f(distance, 0.4f, termination, 1.f); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(resolved.pixels.empty());
        const osg::Vec4f& center = resolved.at(resolved.width / 2, resolved.height / 2);
        for (int channel = 0; channel < 4; ++channel)
        {
            EXPECT_NEAR(center[channel], brightCloud[channel], 0.002f);
        }
    }

    TEST(AtmosphereShaderTest, newlyVisibleCloudRejectsEmptyHistory)
    {
        const osg::Vec4f currentVolume(0.1875f, 0.21875f, 0.28125f, 0.625f);
        const osg::Vec4f emptyHistory(0.f, 0.f, 0.f, 1.f);
        const osg::Vec4f cloudMetadata(
            encodeAtmosphereDistance(18000.f), 0.36f, encodeAtmosphereDistance(20000.f), 2.f);
        const osg::Vec4f emptyMetadata(encodeAtmosphereDistance(20000.f), 0.f, encodeAtmosphereDistance(20000.f), 0.f);

        NativePassInputs inputs;
        inputs.historyValid = true;
        inputs.passInput = [currentVolume](int, int) { return currentVolume; };
        inputs.volumeMetadataInput = [cloudMetadata](int, int) { return cloudMetadata; };
        inputs.atmosphereHistory = [emptyHistory](int, int) { return emptyHistory; };
        inputs.atmosphereMetadataHistory = [emptyMetadata](int, int) { return emptyMetadata; };

        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(resolved.pixels.empty());
        const osg::Vec4f& center = resolved.at(resolved.width / 2, resolved.height / 2);
        constexpr float halfFloatTolerance = 0.0005f;
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(center[channel], currentVolume[channel], halfFloatTolerance);
    }

    /// Replace old cloud radiance at refreshed rays as cloud coverage grows, disperses, or reveals mist.
    TEST(AtmosphereShaderTest, cloudRefreshReplacesThePreviousIntegral)
    {
        const osg::Vec4f previousCloud(0.3f, 0.32f, 0.34f, 0.7f);
        const osg::Vec4f cloudMetadata(encodeAtmosphereDistance(18000.f), 0.3f, encodeAtmosphereDistance(20000.f), 2.f);
        const std::array<osg::Vec4f, 3> currentIntegrals{ osg::Vec4f(0.f, 0.f, 0.f, 1.f),
            osg::Vec4f(0.05f, 0.07f, 0.09f, 0.9f), osg::Vec4f(0.9f, 0.92f, 0.94f, 0.1f) };
        for (int medium = 0; medium < 3; ++medium)
        {
            SCOPED_TRACE(medium);
            const osg::Vec4f currentIntegral = currentIntegrals[medium];
            NativePassInputs inputs;
            inputs.historyValid = true;
            inputs.passInput = [previousCloud, currentIntegral](int x, int y) {
                return x >= 31 && x <= 32 && y >= 31 && y <= 32 ? currentIntegral : previousCloud;
            };
            inputs.volumeMetadataInput = [cloudMetadata, currentIntegral, medium](int x, int y) {
                osg::Vec4f metadata = cloudMetadata;
                if (x >= 31 && x <= 32 && y >= 31 && y <= 32)
                {
                    metadata.g() = 1.f - currentIntegral.a();
                    metadata.a() = static_cast<float>(medium);
                }
                return metadata;
            };
            inputs.atmosphereHistory = [previousCloud](int, int) { return previousCloud; };
            inputs.atmosphereMetadataHistory = [cloudMetadata](int, int) { return cloudMetadata; };

            const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
            ASSERT_FALSE(resolved.pixels.empty());
            const osg::Vec4f& center = resolved.at(resolved.width / 2, resolved.height / 2);
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(center[channel], currentIntegral[channel], 0.002f);
        }
    }

    /// Keep shallow mist and fog integration stable when the medium field and camera have not changed.
    TEST(AtmosphereShaderTest, stationaryLowMediaDoNotInjectTemporalRayNoise)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        inputs.width = 64;
        inputs.height = 40;
        inputs.eyePosition = { 19962.6f, -97318.7f, 1659.62f };
        inputs.viewDirection = { 1.f, 0.f, 0.08f };
        inputs.farPlane = 229376.f;
        inputs.cloudParameters = {};
        inputs.lowCloudParameters = {};
        inputs.mistParameters = { 0.030467f, 794.045f, 4534.21f, 0.f };
        inputs.fogVolumeParameters = {};
        inputs.historyValid = true;
        inputs.frameNumber = 320;
        const PassImage first = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.frameNumber = 321;
        const PassImage second = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(first.pixels.size(), second.pixels.size());

        float maximumDifference = 0.f;
        std::size_t occupiedPixels = 0;
        for (std::size_t index = 0; index < first.pixels.size(); ++index)
        {
            if (1.f - std::min(first.pixels[index].w(), second.pixels[index].w()) > 0.01f)
                ++occupiedPixels;
            for (int channel = 0; channel < 4; ++channel)
                maximumDifference = std::max(
                    maximumDifference, std::abs(first.pixels[index][channel] - second.pixels[index][channel]));
        }
        ASSERT_GT(occupiedPixels, 0u);
        EXPECT_LT(maximumDifference, 0.0001f);
    }

    /// Reject ground/above-deck radiance history when the observer crosses the clear-air recovery interval in one
    /// frame.
    TEST(AtmosphereShaderTest, altitudeDiscontinuityReconstructsCurrentAtmosphere)
    {
        for (bool ascending : { false, true })
        {
            NativePassInputs previous = makePackagedAtmosphereInputs("Cloudy", "Day");
            previous.width = 32;
            previous.height = 20;
            previous.eyePosition = { 23227.6f, 69902.8f, ascending ? 22000.f : 450000.f };
            previous.viewDirection = { 1.f, 0.f, ascending ? 0.18f : -0.32f };
            previous.farPlane = 500000.f;
            previous.fogNear = 22118.4f;
            previous.fogFar = 199066.f;
            previous.cloudAdvectionOffsetWorldUnits = { 180571.f, 296323.f };
            previous.mistParameters = {};
            previous.lowCloudParameters = {};
            previous.frameNumber = 3;
            const ProductionAtmosphereFrame previousFrame = renderProductionAtmosphereVolumeFrame(previous);

            NativePassInputs current = previous;
            current.eyePosition.z() = ascending ? 450000.f : 22000.f;
            current.viewDirection.z() = ascending ? -0.32f : 0.18f;
            current.previousEyeOffset = previous.eyePosition - current.eyePosition;
            current.previousViewDirection = previous.viewDirection;
            current.historyValid = true;
            current.frameNumber = 4;
            const ProductionAtmosphereFrame reprojected
                = renderProductionAtmosphereVolumeFrame(current, &previousFrame);
            current.historyValid = false;
            const ProductionAtmosphereFrame fresh = renderProductionAtmosphereVolumeFrame(current);
            ASSERT_EQ(reprojected.mResolvedVolume.pixels.size(), fresh.mResolvedVolume.pixels.size());
            EXPECT_LT(meanRgbDifference(reprojected.mResolvedVolume, fresh.mResolvedVolume), 0.0001)
                << "ascending=" << ascending;
            EXPECT_GT(std::count_if(fresh.mResolvedVolume.pixels.begin(), fresh.mResolvedVolume.pixels.end(),
                          [](const osg::Vec4f& sample) { return sample.w() < 0.9f; }),
                4);
        }
    }

    /// Preserve beyond-far cloud history on untraced rays while every checkerboard phase refreshes its current
    /// integral.
    TEST(AtmosphereShaderTest, movingGroundCameraReprojectsUnoccludedCloudsBeyondTheFarPlane)
    {
        const float encodedCloudDistance = encodeAtmosphereDistance(60000.f);
        const float encodedSceneDistance = encodeAtmosphereDistance(20000.f);
        const osg::Vec4f currentVolume(0.08f, 0.1f, 0.14f, 0.72f);
        const osg::Vec4f historyVolume(0.42f, 0.46f, 0.5f, 0.58f);
        const osg::Vec4f cloudMetadata(encodedCloudDistance, 0.42f, encodedSceneDistance, 2.f);

        NativePassInputs inputs;
        inputs.historyValid = true;
        inputs.previousEyeOffset = { 0.f, -180.f, 0.f };
        inputs.passInput = [currentVolume, historyVolume](int x, int y) {
            return x >= 32 && x < 34 && y >= 32 && y < 34 ? currentVolume : historyVolume;
        };
        inputs.volumeMetadataInput = [cloudMetadata](int, int) { return cloudMetadata; };
        inputs.atmosphereHistory = [historyVolume](int, int) { return historyVolume; };
        inputs.atmosphereMetadataHistory = [cloudMetadata](int, int) { return cloudMetadata; };
        inputs.previousDepth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f; };

        for (int frame = 4; frame < 8; ++frame)
        {
            inputs.frameNumber = frame;
            const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
            ASSERT_FALSE(resolved.pixels.empty());
            for (int y = 32; y < 34; ++y)
                for (int x = 32; x < 34; ++x)
                {
                    const int phase = (frame + (x / 2 + y / 2) % 2) % 4;
                    const bool traced = x % 2 == phase % 2 && y % 2 == phase / 2;
                    const osg::Vec4f& expected = traced ? currentVolume : historyVolume;
                    for (int channel = 0; channel < 4; ++channel)
                        EXPECT_NEAR(resolved.at(x, y)[channel], expected[channel], 0.001f)
                            << "frame=" << frame << " pixel=" << x << ',' << y << " traced=" << traced;
                }
        }
    }

    TEST(AtmosphereShaderTest, movingCameraReusesRealCloudHistoryFromGroundAndCloudLayerAltitudes)
    {
        constexpr int width = 48;
        constexpr int height = 32;
        const osg::Vec3f cameraTranslation(0.f, -180.f, 0.f);

        for (const float cameraAltitude : { 1200.f, 54000.f })
        {
            NativePassInputs currentInputs;
            currentInputs.width = width;
            currentInputs.height = height;
            currentInputs.eyePosition = { 0.f, 0.f, cameraAltitude };
            currentInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
            currentInputs.mistParameters = {};
            currentInputs.frameNumber = 4;
            currentInputs.historyValid = true;
            currentInputs.previousEyeOffset = cameraTranslation;

            NativePassInputs previousInputs = currentInputs;
            previousInputs.eyePosition += cameraTranslation;
            previousInputs.frameNumber = 3;
            previousInputs.historyValid = false;
            previousInputs.previousEyeOffset = {};
            const PassImage previousRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", previousInputs);
            ASSERT_FALSE(previousRaw.pixels.empty());
            NativePassInputs previousMetadataInputs = previousInputs;
            const PassImage previousMetadata = renderNativeAtmosphereMetadata(previousMetadataInputs);
            ASSERT_EQ(previousMetadata.pixels.size(), previousRaw.pixels.size());

            const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", currentInputs);
            ASSERT_FALSE(currentRaw.pixels.empty());
            NativePassInputs currentMetadataInputs = currentInputs;
            const PassImage currentMetadata = renderNativeAtmosphereMetadata(currentMetadataInputs);
            ASSERT_EQ(currentMetadata.pixels.size(), currentRaw.pixels.size());

            NativePassInputs resolveInputs = currentInputs;
            resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
            resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
            resolveInputs.atmosphereHistory = [&previousRaw](int x, int y) { return previousRaw.at(x, y); };
            resolveInputs.atmosphereMetadataHistory
                = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
            const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
            ASSERT_EQ(resolved.pixels.size(), currentRaw.pixels.size());

            std::size_t cloudPixelCount = 0;
            std::size_t historyContributingPixelCount = 0;
            for (std::size_t index = 0; index < currentRaw.pixels.size(); ++index)
            {
                if (currentMetadata.pixels[index].w() < 1.5f || 1.f - currentRaw.pixels[index].w() < 0.01f)
                    continue;
                ++cloudPixelCount;
                float difference = 0.f;
                for (int channel = 0; channel < 4; ++channel)
                    difference += std::abs(resolved.pixels[index][channel] - currentRaw.pixels[index][channel]);
                if (difference > 0.001f)
                    ++historyContributingPixelCount;
            }
            ASSERT_GT(cloudPixelCount, 16u) << "cameraAltitude=" << cameraAltitude;
            EXPECT_GT(historyContributingPixelCount, cloudPixelCount / 10u) << "cameraAltitude=" << cameraAltitude;
        }
    }

    TEST(AtmosphereShaderTest, movingCameraOutputMatchesTheCurrentWorldSpaceCloudField)
    {
        constexpr int width = 32;
        constexpr int height = 20;
        constexpr int referenceFrameCount = 4;
        const osg::Vec3f currentEyePosition(0.f, 0.f, 1200.f);
        const osg::Vec3f previousEyeOffset(0.f, -180.f, 0.f);

        const auto renderCloudReference = [&](const osg::Vec3f& eyePosition) {
            NativePassInputs inputs;
            inputs.width = width;
            inputs.height = height;
            inputs.sceneWidth = width * 2;
            inputs.sceneHeight = height * 2;
            inputs.eyePosition = eyePosition;
            inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
            inputs.mistParameters = {};
            return renderCloudFootprintReference(inputs, referenceFrameCount);
        };

        const PassImage previousReference = renderCloudReference(currentEyePosition + previousEyeOffset);
        const PassImage currentReference = renderCloudReference(currentEyePosition);
        ASSERT_EQ(previousReference.pixels.size(), currentReference.pixels.size());

        NativePassInputs currentInputs;
        currentInputs.width = width;
        currentInputs.height = height;
        currentInputs.sceneWidth = width * 2;
        currentInputs.sceneHeight = height * 2;
        currentInputs.eyePosition = currentEyePosition;
        currentInputs.previousEyeOffset = previousEyeOffset;
        currentInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        currentInputs.mistParameters = {};
        currentInputs.frameNumber = referenceFrameCount + 1;
        currentInputs.historyValid = true;
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", currentInputs);

        NativePassInputs currentMetadataInputs = currentInputs;
        const PassImage currentMetadata = renderNativeAtmosphereMetadata(currentMetadataInputs);

        NativePassInputs previousMetadataInputs = currentInputs;
        previousMetadataInputs.eyePosition += previousEyeOffset;
        previousMetadataInputs.previousEyeOffset = {};
        const PassImage previousMetadata = renderNativeAtmosphereMetadata(previousMetadataInputs);

        NativePassInputs resolveInputs = currentInputs;
        resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
        resolveInputs.atmosphereHistory = [&previousReference](int x, int y) { return previousReference.at(x, y); };
        resolveInputs.atmosphereMetadataHistory
            = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        ASSERT_EQ(resolved.pixels.size(), currentReference.pixels.size());

        double rawOpacityError = 0.0;
        double resolvedOpacityError = 0.0;
        double historyContribution = 0.0;
        for (std::size_t index = 0; index < resolved.pixels.size(); ++index)
        {
            rawOpacityError += std::abs(currentRaw.pixels[index].w() - currentReference.pixels[index].w());
            resolvedOpacityError += std::abs(resolved.pixels[index].w() - currentReference.pixels[index].w());
            historyContribution += std::abs(resolved.pixels[index].w() - currentRaw.pixels[index].w());
        }

        EXPECT_GT(historyContribution, 0.001);
        RecordProperty("raw_opacity_error", std::to_string(rawOpacityError));
        RecordProperty("resolved_opacity_error", std::to_string(resolvedOpacityError));
        RecordProperty("history_contribution", std::to_string(historyContribution));
        EXPECT_LT(resolvedOpacityError, rawOpacityError)
            << "rawOpacityError=" << rawOpacityError << " resolvedOpacityError=" << resolvedOpacityError
            << " historyContribution=" << historyContribution;
    }

    TEST(AtmosphereShaderTest, sustainedCameraMotionDoesNotDragCloudHistoryBehindTheWorldField)
    {
        constexpr int width = 32;
        constexpr int height = 20;
        constexpr int referenceFrameCount = 4;
        constexpr int motionFrameCount = 4;
        const osg::Vec3f frameTranslation(0.f, 180.f, 0.f);

        const auto makeInputs = [&](const osg::Vec3f& eyePosition, int frameNumber) {
            NativePassInputs inputs;
            inputs.width = width;
            inputs.height = height;
            inputs.sceneWidth = width * 2;
            inputs.sceneHeight = height * 2;
            inputs.eyePosition = eyePosition;
            inputs.previousEyeOffset = -frameTranslation;
            inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
            inputs.mistParameters = {};
            inputs.frameNumber = frameNumber;
            inputs.historyValid = true;
            return inputs;
        };
        const auto renderCloudReference = [&](const osg::Vec3f& eyePosition) {
            return renderCloudFootprintReference(makeInputs(eyePosition, 0), referenceFrameCount);
        };
        osg::Vec3f previousEyePosition;
        PassImage previousResolved = renderCloudReference(previousEyePosition);
        NativePassInputs previousMetadataInputs = makeInputs(previousEyePosition, 0);
        previousMetadataInputs.previousEyeOffset = {};
        PassImage previousMetadata = renderNativeAtmosphereMetadata(previousMetadataInputs);

        double cumulativeRawOpacityError = 0.0;
        double cumulativeResolvedOpacityError = 0.0;
        for (int motionFrame = 1; motionFrame <= motionFrameCount; ++motionFrame)
        {
            const osg::Vec3f eyePosition = frameTranslation * static_cast<float>(motionFrame);
            NativePassInputs currentInputs = makeInputs(eyePosition, referenceFrameCount + motionFrame);
            const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", currentInputs);

            NativePassInputs currentMetadataInputs = currentInputs;
            currentMetadataInputs.atmosphereMetadataHistory
                = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
            const PassImage currentMetadata = renderNativeAtmosphereMetadata(currentMetadataInputs);

            NativePassInputs resolveInputs = currentInputs;
            resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
            resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
            resolveInputs.atmosphereHistory = [&previousResolved](int x, int y) { return previousResolved.at(x, y); };
            resolveInputs.atmosphereMetadataHistory
                = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
            PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
            const PassImage reference = renderCloudReference(eyePosition);
            ASSERT_EQ(resolved.pixels.size(), reference.pixels.size());

            for (std::size_t index = 0; index < resolved.pixels.size(); ++index)
            {
                cumulativeRawOpacityError += std::abs(currentRaw.pixels[index].w() - reference.pixels[index].w());
                cumulativeResolvedOpacityError += std::abs(resolved.pixels[index].w() - reference.pixels[index].w());
            }
            previousResolved = std::move(resolved);
            previousMetadata = currentMetadata;
        }

        RecordProperty("raw_opacity_error", std::to_string(cumulativeRawOpacityError));
        RecordProperty("resolved_opacity_error", std::to_string(cumulativeResolvedOpacityError));
        EXPECT_LT(cumulativeResolvedOpacityError, cumulativeRawOpacityError)
            << "cumulativeRawOpacityError=" << cumulativeRawOpacityError
            << " cumulativeResolvedOpacityError=" << cumulativeResolvedOpacityError;
    }

    /// Keep cloud history aligned with the current world-space field through sustained camera rotation.
    TEST(AtmosphereShaderTest, sustainedCameraRotationDoesNotSmearCloudHistory)
    {
        constexpr int width = 32;
        constexpr int height = 20;
        constexpr int referenceFrameCount = 4;
        constexpr int motionFrameCount = 4;
        constexpr float yawStep = 0.05f;

        const auto viewDirectionAt = [](float yaw) {
            osg::Vec3f direction(std::cos(yaw), std::sin(yaw), 0.18f);
            direction.normalize();
            return direction;
        };
        const auto makeInputs = [&](const osg::Vec3f& viewDirection,
                                    const std::optional<osg::Vec3f>& previousViewDirection, int frameNumber) {
            NativePassInputs inputs;
            inputs.width = width;
            inputs.height = height;
            inputs.sceneWidth = width * 2;
            inputs.sceneHeight = height * 2;
            inputs.eyePosition = { 0.f, 0.f, 1200.f };
            inputs.viewDirection = viewDirection;
            inputs.previousViewDirection = previousViewDirection;
            inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
            inputs.mistParameters = {};
            inputs.frameNumber = frameNumber;
            inputs.historyValid = true;
            return inputs;
        };
        const auto renderCloudReference = [&](const osg::Vec3f& viewDirection) {
            NativePassInputs inputs = makeInputs(viewDirection, std::nullopt, 0);
            inputs.width /= 2;
            inputs.height /= 2;
            return renderCloudFootprintReference(inputs, referenceFrameCount);
        };
        osg::Vec3f previousViewDirection = viewDirectionAt(0.f);
        NativePassInputs initialInputs = makeInputs(previousViewDirection, std::nullopt, 0);
        initialInputs.historyValid = false;
        ProductionAtmosphereFrame previousFrame = renderProductionAtmosphereVolumeFrame(initialInputs);

        double cumulativeFreshOpacityError = 0.0;
        double cumulativeStaleOpacityError = 0.0;
        double cumulativeResolvedOpacityError = 0.0;
        for (int motionFrame = 1; motionFrame <= motionFrameCount; ++motionFrame)
        {
            const osg::Vec3f viewDirection = viewDirectionAt(yawStep * static_cast<float>(motionFrame));
            NativePassInputs currentInputs
                = makeInputs(viewDirection, previousViewDirection, referenceFrameCount + motionFrame);
            ProductionAtmosphereFrame resolved = renderProductionAtmosphereVolumeFrame(currentInputs, &previousFrame);
            NativePassInputs freshInputs = currentInputs;
            freshInputs.historyValid = false;
            const ProductionAtmosphereFrame fresh = renderProductionAtmosphereVolumeFrame(freshInputs);
            const PassImage reference = renderCloudReference(viewDirection);
            ASSERT_EQ(resolved.mResolvedVolume.pixels.size(), reference.pixels.size());
            ASSERT_EQ(fresh.mResolvedVolume.pixels.size(), reference.pixels.size());

            for (std::size_t index = 0; index < reference.pixels.size(); ++index)
            {
                cumulativeFreshOpacityError
                    += std::abs(fresh.mResolvedVolume.pixels[index].w() - reference.pixels[index].w());
                cumulativeStaleOpacityError
                    += std::abs(previousFrame.mResolvedVolume.pixels[index].w() - reference.pixels[index].w());
                cumulativeResolvedOpacityError
                    += std::abs(resolved.mResolvedVolume.pixels[index].w() - reference.pixels[index].w());
            }
            previousFrame = std::move(resolved);
            previousViewDirection = viewDirection;
        }

        RecordProperty("fresh_opacity_error", std::to_string(cumulativeFreshOpacityError));
        RecordProperty("stale_opacity_error", std::to_string(cumulativeStaleOpacityError));
        RecordProperty("resolved_opacity_error", std::to_string(cumulativeResolvedOpacityError));
        EXPECT_LT(cumulativeResolvedOpacityError, cumulativeStaleOpacityError)
            << "cumulativeFreshOpacityError=" << cumulativeFreshOpacityError
            << " cumulativeStaleOpacityError=" << cumulativeStaleOpacityError
            << " cumulativeResolvedOpacityError=" << cumulativeResolvedOpacityError;
        EXPECT_LT(cumulativeResolvedOpacityError,
            cumulativeFreshOpacityError + (cumulativeStaleOpacityError - cumulativeFreshOpacityError) * 0.6)
            << "Temporal stability must remain substantially closer to the current field than stale history"
            << " cumulativeFreshOpacityError=" << cumulativeFreshOpacityError
            << " cumulativeStaleOpacityError=" << cumulativeStaleOpacityError
            << " cumulativeResolvedOpacityError=" << cumulativeResolvedOpacityError;
    }

    TEST(AtmosphereShaderTest, currentCloudIntegrationDoesNotDependOnPreviousCameraDirection)
    {
        constexpr int width = 32;
        constexpr int height = 20;
        const osg::Vec3f currentEyePosition(0.f, 0.f, 1200.f);

        NativePassInputs currentInputs;
        currentInputs.width = width;
        currentInputs.height = height;
        currentInputs.eyePosition = currentEyePosition;
        currentInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        currentInputs.mistParameters = {};
        currentInputs.frameNumber = 5;
        currentInputs.historyValid = true;

        const auto integrateFromPreviousCamera = [&](const osg::Vec3f& previousEyeOffset) {
            NativePassInputs inputs = currentInputs;
            inputs.previousEyeOffset = previousEyeOffset;
            return renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        };

        const PassImage arrivedFromSouth = integrateFromPreviousCamera({ 0.f, -180.f, 0.f });
        const PassImage arrivedFromWest = integrateFromPreviousCamera({ -180.f, 0.f, 0.f });
        ASSERT_EQ(arrivedFromSouth.pixels.size(), arrivedFromWest.pixels.size());
        double directionDependentDifference = 0.0;
        for (std::size_t index = 0; index < arrivedFromSouth.pixels.size(); ++index)
            for (int channel = 0; channel < 4; ++channel)
                directionDependentDifference
                    += std::abs(arrivedFromSouth.pixels[index][channel] - arrivedFromWest.pixels[index][channel]);
        directionDependentDifference /= static_cast<double>(arrivedFromSouth.pixels.size() * 4);
        EXPECT_LT(directionDependentDifference, 1e-5);
    }

    /// Keep a current ray independent of frame phase while the camera has a translation history.
    TEST(AtmosphereShaderTest, translatingCameraRetainsStableCloudIntegration)
    {
        NativePassInputs firstInputs;
        firstInputs.width = 32;
        firstInputs.height = 20;
        firstInputs.eyePosition = { 0.f, 0.f, 1200.f };
        firstInputs.previousEyeOffset = { 0.f, -180.f, 0.f };
        firstInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        firstInputs.mistParameters = {};
        firstInputs.frameNumber = 5;
        firstInputs.historyValid = true;
        const PassImage first = renderNativeAtmospherePass("integrateAtmosphereRayGrid", firstInputs);

        NativePassInputs secondInputs = firstInputs;
        secondInputs.frameNumber = 6;
        const PassImage second = renderNativeAtmospherePass("integrateAtmosphereRayGrid", secondInputs);
        ASSERT_EQ(first.pixels.size(), second.pixels.size());
        EXPECT_LT(meanRgbDifference(first, second), 1e-6);
    }

    /// Reduce spatial checkerboard variation in a stationary cloud field.
    TEST(AtmosphereShaderTest, stationaryCloudHistoryConvergesCheckerboardSamples)
    {
        NativePassInputs volumeInputs;
        volumeInputs.width = 32;
        volumeInputs.height = 20;
        volumeInputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        volumeInputs.mistParameters = {};

        ProductionAtmosphereFrame previousFrame;
        double rawVariation = 0.0;
        double resolvedVariation = 0.0;
        std::size_t comparedRawValues = 0;
        std::size_t comparedResolvedValues = 0;

        for (int frameNumber = 0; frameNumber < 8; ++frameNumber)
        {
            volumeInputs.frameNumber = frameNumber;
            volumeInputs.historyValid = frameNumber > 0;
            ProductionAtmosphereFrame frame
                = renderProductionAtmosphereVolumeFrame(volumeInputs, frameNumber > 0 ? &previousFrame : nullptr);
            const PassImage& raw = frame.mRawVolume;
            const PassImage& resolved = frame.mResolvedVolume;

            if (frameNumber >= 4)
            {
                for (std::size_t pixelIndex = 0; pixelIndex < raw.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        rawVariation += std::abs(
                            raw.pixels[pixelIndex][channel] - previousFrame.mRawVolume.pixels[pixelIndex][channel]);
                        ++comparedRawValues;
                    }
                for (std::size_t pixelIndex = 0; pixelIndex < resolved.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        resolvedVariation += std::abs(resolved.pixels[pixelIndex][channel]
                            - previousFrame.mResolvedVolume.pixels[pixelIndex][channel]);
                        ++comparedResolvedValues;
                    }
            }

            previousFrame = std::move(frame);
        }

        ASSERT_GT(comparedRawValues, 0u);
        ASSERT_GT(comparedResolvedValues, 0u);
        const double meanRawVariation = rawVariation / static_cast<double>(comparedRawValues);
        const double meanResolvedVariation = resolvedVariation / static_cast<double>(comparedResolvedValues);
        EXPECT_GT(meanRawVariation, 1e-5);
        EXPECT_LT(meanResolvedVariation, meanRawVariation * 0.35);
    }

    /// Retrace cloud history whose own radial depth cannot return to the current pixel during translation.
    TEST(AtmosphereShaderTest, translatedCloudHistoryMustReturnToItsCurrentPixel)
    {
        for (const bool reverseZ : { false, true })
            for (const float translation : { 0.f, 400.f })
                for (const float historicalDistance : { 2000.f, 18000.f })
                {
                    SCOPED_TRACE(reverseZ);
                    SCOPED_TRACE(translation);
                    SCOPED_TRACE(historicalDistance);
                    NativePassInputs inputs;
                    inputs.reverseZ = reverseZ;
                    inputs.depth = [reverseZ](int, int) { return reverseZ ? 0.f : 1.f; };
                    inputs.previousDepth = inputs.depth;
                    inputs.width = 64;
                    inputs.height = 40;
                    inputs.historyValid = true;
                    inputs.previousEyeOffset = { 0.f, translation, 0.f };
                    inputs.passInput = [](int, int) { return osg::Vec4f(0.2f, 0.2f, 0.2f, 0.7f); };
                    inputs.volumeMetadataInput = [](int, int) {
                        return osg::Vec4f(
                            encodeAtmosphereDistance(18000.f), 0.3f, encodeAtmosphereDistance(20000.f), 2.f);
                    };
                    inputs.atmosphereHistory = [](int, int) { return osg::Vec4f(0.4f, 0.4f, 0.4f, 0.4f); };
                    inputs.atmosphereMetadataHistory = [historicalDistance](int, int) {
                        return osg::Vec4f(
                            encodeAtmosphereDistance(historicalDistance), 0.6f, encodeAtmosphereDistance(20000.f), 2.f);
                    };
                    PassImage metadata;
                    const PassImage reprojected
                        = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs, &metadata);
                    ASSERT_FALSE(reprojected.pixels.empty());
                    std::size_t untracedPixels = 0;
                    std::size_t repairedPixels = 0;
                    double retainedOpacityError = 0.0;
                    for (int y = 8; y < inputs.height - 8; ++y)
                        for (int x = 8; x < inputs.width - 8; ++x)
                        {
                            const int phase = (inputs.frameNumber + ((x / 2 + y / 2) % 2)) % 4;
                            if (x % 2 == phase % 2 && y % 2 == phase / 2)
                                continue;
                            ++untracedPixels;
                            repairedPixels += metadata.at(x, y).a() < 0.f;
                            retainedOpacityError += std::abs(reprojected.at(x, y).a() - 0.4f);
                        }
                    EXPECT_GT(untracedPixels, 0u);
                    if (translation > 0.f && historicalDistance < 10000.f)
                        EXPECT_EQ(repairedPixels, untracedPixels)
                            << "Stale near-cloud radiance must be repaired, not carried at the far-cloud motion";
                    else
                    {
                        EXPECT_EQ(repairedPixels, 0u);
                        EXPECT_LT(retainedOpacityError / static_cast<double>(untracedPixels), 0.002);
                    }
                }
    }

    /// Keep stationary shallow condensate stable without relying on temporal history to hide its sampling noise.
    TEST(AtmosphereShaderTest, shallowCloudCondensateDoesNotBoilAtRest)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Rain", "Day");
        inputs.width = 32;
        inputs.height = 24;
        inputs.cloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.lowCloudParameters = { 0.37f, 0.96f, 2573.f, 11583.f };
        inputs.eyePosition = { 22995.f, -38405.f, 19856.f };
        inputs.viewDirection = { 0.34f, 0.6f, -0.73f };
        inputs.farPlane = 229376.f;
        inputs.historyValid = true;
        inputs.frameNumber = 7100;
        const PassImage first = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ++inputs.frameNumber;
        const PassImage second = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        double opacity = 0.0;
        for (const osg::Vec4f& pixel : first.pixels)
            opacity += 1.f - pixel.a();
        EXPECT_GT(opacity / first.pixels.size(), 0.01);
        EXPECT_LT(meanRgbDifference(first, second), 1e-6)
            << "A fixed shallow density field must not change with the blue-noise frame phase";
    }

    /// Preserve stationary low-cloud optical depth while moving through empty foreground along the same ray.
    TEST(AtmosphereShaderTest, forwardMotionThroughEmptyAirPreservesLowCloudOpticalDepth)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Thunderstorm", "Day");
        inputs.width = inputs.height = 1;
        inputs.cloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.lowCloudParameters = { 0.21f, 1.066f, 3147.f, 14199.f };
        inputs.lowCloudEnvironmentInfluence = 0.f;
        inputs.eyePosition = { 35276.5f, -74563.6f, 8308.86f };
        inputs.viewDirection = { 0.97545f, 0.2174f, -0.03563f };
        inputs.viewDirection.normalize();
        inputs.farPlane = 229376.f;
        inputs.historyValid = true;
        const osg::Vec3f origin = inputs.eyePosition;
        const PassImage first = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(first.pixels.size(), 1u);
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 28;
        inputs.cloudDensitySliceAxis = 0;
        const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_LT(reference.pixels[0].a(), 0.999f);
        inputs.cloudDensitySliceAxis = 1;
        const PassImage finerReference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        EXPECT_NEAR(reference.pixels[0].a(), finerReference.pixels[0].a(), 0.0005f);
        EXPECT_NEAR(first.pixels[0].a(), reference.pixels[0].a(), 0.002f) << "Dense optical-depth reference";
        inputs.renderCloudDensitySlice = false;
        const auto sceneDepth = inputs.depth;
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(400.f, false); };
        const PassImage foreground = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_GT(foreground.pixels[0].a(), 0.999f);
        inputs.depth = sceneDepth;
        for (const float distance : { 50.f, 100.f, 200.f, 300.f })
        {
            inputs.eyePosition = origin + inputs.viewDirection * distance;
            const PassImage moved = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            EXPECT_NEAR(moved.pixels[0].a(), reference.pixels[0].a(), 0.002f) << "distance=" << distance;
            inputs.renderCloudDensitySlice = true;
            const PassImage movedReference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            EXPECT_NEAR(movedReference.pixels[0].a(), finerReference.pixels[0].a(), 0.0005f);
            inputs.renderCloudDensitySlice = false;
        }
    }

    /// Preserve local-medium radiance and depth when lighting is reused below, across, and above cloud recovery.
    TEST(AtmosphereShaderTest, localLightReusePreservesPerSampleIllumination)
    {
        for (const float sunHeight : { -0.12f, 0.03f, 0.65f })
            for (const float layerAltitude : { 500.f, 10000.f, 25000.f })
                for (const bool reverseZ : { false, true })
                    for (const bool lowClouds : { false, true })
                    {
                        SCOPED_TRACE(::testing::Message() << "sun=" << sunHeight << " altitude=" << layerAltitude
                                                          << " reverseZ=" << reverseZ << " lowClouds=" << lowClouds);
                        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
                        inputs.width = 12;
                        inputs.height = 8;
                        inputs.quality = 2;
                        inputs.reverseZ = reverseZ;
                        inputs.useUniformBuffer = reverseZ;
                        inputs.depth = [reverseZ](int, int) { return reverseZ ? 0.f : 1.f; };
                        inputs.sunDirection = { 1.f, 0.2f, sunHeight, 0.f };
                        inputs.cloudParameters = { 0.9f, 0.1f, 5000.f, 5000.f };
                        inputs.incomingCloudParameters = {};
                        inputs.lowCloudParameters
                            = lowClouds ? osg::Vec4f(0.4f, 0.9f, layerAltitude, 10000.f) : osg::Vec4f();
                        inputs.environmentalCloudModel = SceneUtil::CloudLayer{};
                        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
                        inputs.environmentalCloudModel.mTypeAmount = 0.f;
                        inputs.environmentalCloudModel.mTypeMinimum = 0.5f;
                        inputs.lowCloudEnvironmentInfluence = 0.f;
                        inputs.mistParameters = {};
                        inputs.fogVolumeParameters = { 0.025f, layerAltitude, 14000.f, 0.35f };
                        inputs.eyePosition = { 0.f, 0.f, layerAltitude + 2000.f };
                        inputs.viewDirection = { 1.f, 0.f, 0.5f };
                        inputs.viewDirection.normalize();
                        inputs.farPlane = 40000.f;
                        PassImage cachedMetadata;
                        const PassImage cached
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &cachedMetadata);
                        inputs.useUncachedLocalLighting = true;
                        PassImage referenceMetadata;
                        const PassImage reference
                            = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &referenceMetadata);
                        ASSERT_EQ(cached.pixels.size(), reference.pixels.size());
                        ASSERT_FALSE(cached.pixels.empty());
                        float maximumOpacity = 0.f;
                        for (std::size_t pixel = 0; pixel < cached.pixels.size(); ++pixel)
                        {
                            maximumOpacity = std::max(maximumOpacity, 1.f - reference.pixels[pixel].a());
                            for (int channel = 0; channel < 4; ++channel)
                            {
                                EXPECT_NEAR(cached.pixels[pixel][channel], reference.pixels[pixel][channel], 1e-6f);
                                EXPECT_NEAR(cachedMetadata.pixels[pixel][channel],
                                    referenceMetadata.pixels[pixel][channel], 1e-6f);
                            }
                        }
                        EXPECT_GT(maximumOpacity, 0.01f);
                    }
    }

    /// Keep every occupied low-cloud sample inside conservative ray bounds across terrain slopes and lattice edges.
    TEST(AtmosphereShaderTest, lowCloudRayBoundsNeverRejectOccupiedDensity)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = 16;
        inputs.height = 8;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 33;
        inputs.lowCloudParameters = { 0.8f, 1.f, 4000.f, 14000.f };
        inputs.lowCloudEnvironmentInfluence = 1.f;
        inputs.mediumReferenceParameters = { 1500.f, 0.6f, 5000.f, 0.f };
        inputs.viewDirection = { 0.96f, 0.25f, -0.12f };
        inputs.viewDirection.normalize();
        inputs.farPlane = 229376.f;
        float maximumDensity = 0.f;
        float maximumRejectedFraction = 0.f;
        for (const float slope : { -0.15f, 0.f, 0.15f })
        {
            inputs.orographicParameters = { 0.f, 0.f, slope, -slope * 0.5f };
            for (const float x : { -163850.f, 163830.f, 1300000.f })
            {
                inputs.eyePosition = { x, -12000.f, 12000.f };
                const PassImage result = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                for (const osg::Vec4f& pixel : result.pixels)
                {
                    EXPECT_EQ(pixel.r(), 0.f) << "x=" << x << ", slope=" << slope;
                    maximumDensity = std::max(maximumDensity, pixel.g());
                    maximumRejectedFraction = std::max(maximumRejectedFraction, pixel.b());
                }
            }
        }
        EXPECT_GT(maximumDensity, 0.01f);
        EXPECT_GT(maximumRejectedFraction, 0.1f);
    }

    /// Match an unculled production integral while traversing low cloud from below, within, and above its terrain
    /// layer.
    TEST(AtmosphereShaderTest, lowCloudRayBoundsPreserveUnculledTransport)
    {
        for (const auto& [weather, phase] :
            { std::pair{ "Overcast", "Day" }, std::pair{ "Clear", "Sunset" }, std::pair{ "Foggy", "Day" } })
        {
            for (const int quality : { 0, 2, 3 })
            {
                NativePassInputs inputs = makePackagedAtmosphereInputs(weather, phase);
                inputs.width = 16;
                inputs.height = 8;
                inputs.quality = quality;
                inputs.lowCloudParameters = { 0.35f, 0.9f, 4000.f, 12000.f };
                inputs.lowCloudEnvironmentInfluence = 1.f;
                inputs.mediumReferenceParameters = { 1200.f, 0.5f, 6000.f, 0.f };
                inputs.orographicParameters = { 0.f, 0.f, 0.08f, -0.03f };
                inputs.eyePosition = { 150000.f, -70000.f, quality == 0 ? 900.f : quality == 2 ? 8000.f : 60000.f };
                inputs.viewDirection = { 0.92f, 0.25f, quality == 0 ? 0.2f : -0.2f };
                inputs.viewDirection.normalize();
                inputs.farPlane = 229376.f;
                const PassImage culled = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                inputs.useUnculledLowCloudReference = true;
                const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(culled.pixels.size(), reference.pixels.size());
                ASSERT_FALSE(culled.pixels.empty());
                for (std::size_t pixel = 0; pixel < culled.pixels.size(); ++pixel)
                    for (int channel = 0; channel < 4; ++channel)
                        EXPECT_NEAR(culled.pixels[pixel][channel], reference.pixels[pixel][channel], 1e-6f)
                            << weather << ", quality=" << quality << ", pixel=" << pixel << ", channel=" << channel;
            }
        }
    }

    /// Retain terrain-following density beyond the low march limit when the main interval continues through it.
    TEST(AtmosphereShaderTest, lowCloudRayBoundsPreserveDensityBeyondLowMarchLimit)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = 16;
        inputs.height = 8;
        inputs.verticalFov = 2.f;
        inputs.cloudParameters = { 1.f, 0.002f, 30000.f, 30000.f };
        inputs.cloudModel.mCoverageMinimum = 1.f;
        inputs.cloudModel.mPhysicalScale = 1.f;
        inputs.cloudModel.mGradientSmall = { 0.f, 0.1f, 0.9f, 1.f };
        inputs.cloudModel.mGradientMedium = inputs.cloudModel.mGradientSmall;
        inputs.cloudModel.mGradientLarge = inputs.cloudModel.mGradientSmall;
        inputs.lowCloudParameters = { 1.f, 1.f, 0.f, 8000.f };
        inputs.environmentalCloudModel.mPhysicalScale = 0.04f;
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        inputs.environmentalCloudModel.mGradientSmall = { 0.f, 0.1f, 0.9f, 1.f };
        inputs.environmentalCloudModel.mGradientMedium = inputs.environmentalCloudModel.mGradientSmall;
        inputs.environmentalCloudModel.mGradientLarge = inputs.environmentalCloudModel.mGradientSmall;
        inputs.lowCloudEnvironmentInfluence = 1.f;
        inputs.mediumReferenceParameters = { 0.f, 1.f, 0.f, 0.f };
        inputs.orographicParameters = { 0.f, 0.f, 0.15f, 0.f };
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.eyePosition = { 0.f, 0.f, 40000.f };
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.farPlane = 800000.f;
        const PassImage culled = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.useUnculledLowCloudReference = true;
        const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(culled.pixels.size(), reference.pixels.size());
        ASSERT_FALSE(culled.pixels.empty());
        inputs.lowCloudParameters.y() = 0.f;
        const PassImage withoutLowCloud = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(withoutLowCloud.pixels.size(), reference.pixels.size());
        double lowCloudDifference = 0.0;
        for (std::size_t pixel = 0; pixel < reference.pixels.size(); ++pixel)
            for (int channel = 0; channel < 4; ++channel)
                lowCloudDifference = std::max(lowCloudDifference,
                    static_cast<double>(
                        std::abs(reference.pixels[pixel][channel] - withoutLowCloud.pixels[pixel][channel])));
        EXPECT_GT(lowCloudDifference, 0.01);
        for (std::size_t pixel = 0; pixel < culled.pixels.size(); ++pixel)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(culled.pixels[pixel][channel], reference.pixels[pixel][channel], 1e-6f)
                    << "pixel=" << pixel << ", channel=" << channel;
    }

    /// Resolve nearby sky-cloud edges against a converged world-field integral during camera approach.
    TEST(AtmosphereShaderTest, nearbySkyCloudEdgesMatchDenseWorldField)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        inputs.width = 48;
        inputs.height = 24;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.farPlane = 229376.f;
        inputs.cloudParameters = { .7f, .35f, 42660.f, 153203.f };
        inputs.cloudModel.mCoverageMinimum = .6f;
        inputs.viewDirection = { -.8f, .55f, .12f };
        inputs.viewDirection.normalize();
        inputs.verticalFov = 35.f;
        inputs.previousVerticalFov = inputs.verticalFov;
        for (const osg::Vec3f eye : { osg::Vec3f(-30126.f, 72900.f, 41595.f), osg::Vec3f(-31726.f, 74000.f, 41835.f) })
        {
            SCOPED_TRACE(eye.x());
            inputs.eyePosition = eye;
            inputs.renderCloudDensitySlice = false;
            const PassImage actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.cloudTransportReferenceSubdivisions = 8;
            const PassImage fineTransport = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.cloudTransportReferenceSubdivisions = 16;
            const PassImage convergedTransport = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.cloudTransportReferenceSubdivisions = 1;
            double meanRadianceError = 0.0;
            float maximumRadianceError = 0.f;
            for (std::size_t index = 0; index < actual.pixels.size(); ++index)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    EXPECT_NEAR(fineTransport.pixels[index][channel], convergedTransport.pixels[index][channel], .01f);
                    const float error
                        = std::abs(actual.pixels[index][channel] - convergedTransport.pixels[index][channel]);
                    maximumRadianceError = std::max(maximumRadianceError, error);
                    meanRadianceError += error;
                }
            }
            EXPECT_LT(maximumRadianceError, .02f);
            EXPECT_LT(meanRadianceError / (actual.pixels.size() * 3), .0025);
            inputs.renderCloudDensitySlice = true;
            inputs.cloudDensitySliceQuantity = 34;
            inputs.cloudDensitySliceAxis = 0;
            const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.cloudDensitySliceAxis = 1;
            const PassImage converged = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            double meanError = 0.0;
            float maximumError = 0.f;
            float maximumOpacity = 0.f;
            for (std::size_t index = 0; index < actual.pixels.size(); ++index)
            {
                EXPECT_NEAR(reference.pixels[index].a(), converged.pixels[index].a(), .002f);
                const float error = std::abs(actual.pixels[index].a() - converged.pixels[index].a());
                maximumError = std::max(maximumError, error);
                meanError += error;
                maximumOpacity = std::max(maximumOpacity, 1.f - converged.pixels[index].a());
            }
            EXPECT_GT(maximumOpacity, .1f);
            EXPECT_LT(maximumError, .02f);
            EXPECT_LT(meanError / actual.pixels.size(), .0025);
        }
    }

    /// Verify approach and yaw against dense current-field transport and a freshly reconstructed frame.
    void expectCloudMotionMatchesDenseField(NativePassInputs inputs, const osg::Vec3f& translation, float yawPerFrame)
    {
        const osg::Vec3f origin = inputs.eyePosition;
        const osg::Vec3f direction = inputs.viewDirection;
        ProductionAtmosphereFrame previous;
        osg::Vec3f previousDirection;
        double cumulativeMotion = 0.0;
        for (int frame = 0; frame < 12; ++frame)
        {
            SCOPED_TRACE(frame);
            const float yaw = static_cast<float>(frame) * yawPerFrame;
            inputs.viewDirection = { direction.x() * std::cos(yaw) - direction.y() * std::sin(yaw),
                direction.x() * std::sin(yaw) + direction.y() * std::cos(yaw), direction.z() };
            inputs.viewDirection.normalize();
            inputs.eyePosition = origin + translation * static_cast<float>(frame);
            inputs.previousEyeOffset = frame > 0 ? -translation : osg::Vec3f{};
            inputs.previousViewDirection = frame > 0 ? std::optional(previousDirection) : std::nullopt;
            inputs.frameNumber = frame;
            inputs.historyValid = frame > 0;
            ProductionAtmosphereFrame current
                = renderProductionAtmosphereVolumeFrame(inputs, frame > 0 ? &previous : nullptr);
            ASSERT_FALSE(current.mResolvedVolume.pixels.empty());
            NativePassInputs referenceInputs = inputs;
            referenceInputs.width = current.mResolvedVolume.width;
            referenceInputs.height = current.mResolvedVolume.height;
            referenceInputs.sceneWidth = inputs.width;
            referenceInputs.sceneHeight = inputs.height;
            referenceInputs.cloudTransportReferenceSubdivisions = 8;
            const PassImage reference = renderCloudFootprintReference(referenceInputs, 1);
            NativePassInputs freshInputs = inputs;
            freshInputs.historyValid = false;
            const ProductionAtmosphereFrame fresh = renderProductionAtmosphereVolumeFrame(freshInputs);
            ASSERT_EQ(current.mResolvedVolume.pixels.size(), reference.pixels.size());
            ASSERT_EQ(fresh.mResolvedVolume.pixels.size(), reference.pixels.size());
            double resolvedError = 0.0;
            double freshError = 0.0;
            std::size_t resolvedOutliers = 0;
            std::size_t freshOutliers = 0;
            for (std::size_t pixel = 0; pixel < reference.pixels.size(); ++pixel)
            {
                float resolvedMaximum = 0.f;
                float freshMaximum = 0.f;
                for (int channel = 0; channel < 4; ++channel)
                {
                    const float resolvedDelta
                        = std::abs(current.mResolvedVolume.pixels[pixel][channel] - reference.pixels[pixel][channel]);
                    const float freshDelta
                        = std::abs(fresh.mResolvedVolume.pixels[pixel][channel] - reference.pixels[pixel][channel]);
                    resolvedError += resolvedDelta;
                    freshError += freshDelta;
                    resolvedMaximum = std::max(resolvedMaximum, resolvedDelta);
                    freshMaximum = std::max(freshMaximum, freshDelta);
                    if (frame > 0)
                        cumulativeMotion += std::abs(fresh.mResolvedVolume.pixels[pixel][channel]
                            - previous.mResolvedVolume.pixels[pixel][channel]);
                }
                resolvedOutliers += resolvedMaximum > .1f ? 1 : 0;
                freshOutliers += freshMaximum > .1f ? 1 : 0;
            }
            const double count = static_cast<double>(reference.pixels.size());
            EXPECT_LT(resolvedError / (count * 4), .02);
            EXPECT_LE(resolvedError / (count * 4), freshError / (count * 4) + .005);
            EXPECT_LE(static_cast<double>(resolvedOutliers) / count, static_cast<double>(freshOutliers) / count + .01);
            previous = std::move(current);
            previousDirection = inputs.viewDirection;
        }
        EXPECT_GT(cumulativeMotion, 1.0);
    }

    /// Keep nearby cloud history aligned with a dense current-field reference during combined approach and rotation.
    TEST(AtmosphereShaderTest, nearbySkyCloudMotionRetainsCurrentField)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        inputs.width = 96;
        inputs.height = 64;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.farPlane = 229376.f;
        inputs.cloudParameters = { .7f, .35f, 42660.f, 153203.f };
        inputs.cloudModel.mCoverageMinimum = .6f;
        inputs.verticalFov = 35.f;
        inputs.previousVerticalFov = inputs.verticalFov;
        inputs.eyePosition = { -30126.f, 72900.f, 41595.f };
        inputs.viewDirection = { -.8f, .55f, .12f };
        expectCloudMotionMatchesDenseField(inputs, { -480.f, 330.f, 40.f }, .006f);
    }

    /// Keep nearby environmental cloud history aligned during a descending, rotating approach.
    TEST(AtmosphereShaderTest, nearbyLowCloudMotionRetainsCurrentField)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Foggy", "Day");
        inputs.width = 96;
        inputs.height = 64;
        inputs.cloudParameters = {};
        inputs.incomingCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.farPlane = 229376.f;
        inputs.lowCloudParameters = { .253974f, .763042f, 3740.32f, 9954.81f };
        inputs.environmentalCloudModel.mCoverageAmount = .49f;
        inputs.mediumReferenceParameters = { 38.f, .45f, 6000.f, 0.f };
        inputs.cloudAdvectionOffsetWorldUnits = { 914.f, 421.9f };
        inputs.verticalFov = inputs.previousVerticalFov = 35.f;
        inputs.eyePosition = { 48659.f, -76196.f, 11449.f };
        inputs.viewDirection = { .919f, .303f, -.255f };
        expectCloudMotionMatchesDenseField(inputs, { 480.f, 160.f, -120.f }, .006f);
    }

    /// Keep a frozen nearby sky volume stable across refresh phases that trace the same camera ray.
    TEST(AtmosphereShaderTest, frozenNearbySkyCloudDoesNotBoilAcrossCheckerboardRefreshes)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Cloudy", "Sunset");
        inputs.width = inputs.height = 1;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.farPlane = 229376.f;
        inputs.historyValid = true;
        inputs.cloudParameters = { .7f, .35f, 42660.f, 153203.f };
        inputs.cloudModel.mCoverageMinimum = .6f;
        inputs.eyePosition = { -30126.f, 72900.f, 41595.f };
        inputs.verticalFov = .01f;
        float maximumOpacity = 0.f;
        for (const float elevation : { .08f, .2f, .45f })
        {
            inputs.viewDirection = { -.8f, .55f, elevation };
            inputs.viewDirection.normalize();
            inputs.frameNumber = 0;
            const auto first = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(first.pixels.size(), 1u);
            maximumOpacity = std::max(maximumOpacity, 1.f - first.pixels[0].a());
            for (int refresh = 1; refresh < 8; ++refresh)
            {
                inputs.frameNumber = refresh * 4;
                const auto current = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(current.pixels.size(), 1u);
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_NEAR(first.pixels[0][channel], current.pixels[0][channel], .005f)
                        << "elevation=" << elevation << ", refresh=" << refresh << ", channel=" << channel;
            }
        }
        EXPECT_GT(maximumOpacity, .1f);
    }

    /// Preserve a dense-reference condensate integral when the observer advances through empty air at every quality.
    TEST(AtmosphereShaderTest, approachingLowCloudPreservesFrozenVolumeRadiance)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Thunderstorm", "Day");
        inputs.width = inputs.height = 1;
        inputs.cloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.lowCloudParameters = { 0.210463f, 1.06606f, 3147.13f, 14198.7f };
        inputs.lowCloudEnvironmentInfluence = 0.8f;
        inputs.mediumReferenceParameters = { 86.4718f, 0.4f, 7000.f, 28.9601f };
        inputs.orographicParameters = { 54349.9f, -70313.1f, 0.00149427f, 0.0109434f };
        inputs.cloudAdvectionOffsetWorldUnits = { 2188.85f, 1010.24f };
        inputs.sunDirection = { 0.197705f, -0.216337f, 0.956092f, 0.f };
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.farPlane = 229376.f;
        inputs.historyValid = true;
        const osg::Vec3f origin{ 55451.7f, -70067.6f, 7571.85f };
        float maximumOpacity = 0.f;
        float maximumReferenceError = 0.f;
        float maximumMotionError = 0.f;
        for (const int quality : { 0, 1, 2, 3 })
            for (const float horizontal : { -0.34f, -0.64f, -0.96f })
            {
                SCOPED_TRACE(testing::Message() << "quality=" << quality << " horizontal=" << horizontal);
                inputs.quality = quality;
                inputs.viewDirection = { 0.97545f + horizontal * 0.2174f, 0.2174f - horizontal * 0.97545f, -0.12f };
                inputs.viewDirection.normalize();
                inputs.eyePosition = origin;
                const PassImage initial = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(initial.pixels.size(), 1u);
                NativePassInputs referenceInputs = inputs;
                referenceInputs.cloudTransportReferenceSubdivisions = 16;
                const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", referenceInputs);
                referenceInputs.cloudTransportReferenceSubdivisions = 32;
                const PassImage converged = renderNativeAtmospherePass("integrateAtmosphereRayGrid", referenceInputs);
                ASSERT_EQ(reference.pixels.size(), 1u);
                ASSERT_EQ(converged.pixels.size(), 1u);
                maximumOpacity = std::max(maximumOpacity, 1.f - converged.pixels[0].a());
                for (int channel = 0; channel < 4; ++channel)
                {
                    EXPECT_NEAR(reference.pixels[0][channel], converged.pixels[0][channel], .001f);
                    const float error = std::abs(initial.pixels[0][channel] - converged.pixels[0][channel]);
                    maximumReferenceError = std::max(maximumReferenceError, error);
                    EXPECT_LT(error, .002f);
                }
                const auto sceneDepth = inputs.depth;
                inputs.depth = [](int, int) { return normalizedDepthForViewDistance(1000.f, false); };
                const PassImage foreground = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_GT(foreground.pixels[0].a(), 0.999f);
                inputs.depth = sceneDepth;
                for (const float distance : { 100.f, 300.f, 600.f, 900.f })
                {
                    inputs.eyePosition = origin + inputs.viewDirection * distance;
                    const PassImage moved = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        maximumMotionError = std::max(
                            maximumMotionError, std::abs(moved.pixels[0][channel] - initial.pixels[0][channel]));
                        EXPECT_NEAR(moved.pixels[0][channel], initial.pixels[0][channel], 0.002f)
                            << "horizontal=" << horizontal << ", distance=" << distance << ", channel=" << channel;
                    }
                }
            }
        EXPECT_GT(maximumOpacity, .05f);
        RecordProperty("maximum_reference_error", std::to_string(maximumReferenceError));
        RecordProperty("maximum_motion_error", std::to_string(maximumMotionError));
    }

    /// Keep condensate at fixed world locations when the observer recenters an unchanged flat terrain sample.
    TEST(AtmosphereShaderTest, movingTerrainSampleDoesNotMoveLowClouds)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = inputs.height = 32;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 29;
        inputs.cloudDensitySliceExtent = { 32000.f, 32000.f };
        inputs.eyePosition = { 15000.f, 0.f, 6000.f };
        inputs.lowCloudParameters = { 0.25f, 0.76f, 3740.f, 9955.f };
        inputs.mediumReferenceParameters = { 38.f, 0.45f, 6000.f, 0.f };
        inputs.orographicParameters = {};
        const PassImage initial = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.orographicParameters.x() = 2048.f;
        const PassImage recentered = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        float maximumHeightChange = 0.f;
        float maximumDensityChange = 0.f;
        for (std::size_t index = 0; index < initial.pixels.size(); ++index)
        {
            maximumHeightChange = std::max(
                maximumHeightChange, std::abs(initial.pixels[index].g() - recentered.pixels[index].g()) * 10000.f);
            maximumDensityChange
                = std::max(maximumDensityChange, std::abs(initial.pixels[index].r() - recentered.pixels[index].r()));
        }
        EXPECT_LT(maximumHeightChange, 1.f);
        EXPECT_LT(maximumDensityChange, 0.001f);
    }

    /// Preserve diffuse illumination per visible droplet as environmental condensate becomes optically thin.
    TEST(AtmosphereShaderTest, thinLowCloudRetainsDiffuseIllumination)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = inputs.height = 16;
        inputs.cloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.lowCloudEnvironmentInfluence = 0.f;
        inputs.lowCloudParameters = { 1.f, 0.01f, 4000.f, 9800.f };
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        inputs.eyePosition = { 64002.f, -71132.f, 5400.f };
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.solarRadiance = {};
        inputs.skyIrradiance = inputs.groundIrradiance = { 1.f, 1.f, 1.f };
        inputs.inScattering = inputs.outScattering = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(20000.f, false); };
        const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        double radiance = 0.0;
        double opacity = 0.0;
        for (const osg::Vec4f& pixel : volume.pixels)
        {
            radiance += pixel.r();
            opacity += 1.f - pixel.a();
        }
        ASSERT_GT(opacity, 0.1);
        EXPECT_GT(radiance / opacity, 0.4)
            << "A white diffuse environment must not turn thin water condensate into an absorbing dark sheet";
    }

    /// Illuminate water droplets from the visible sky even when surface ambient lighting is dim.
    TEST(AtmosphereShaderTest, lowCloudDiffuseLightTracksVisibleSky)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = inputs.height = 8;
        inputs.cloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.lowCloudEnvironmentInfluence = 0.f;
        inputs.lowCloudParameters = { 1.f, .2f, 4000.f, 9800.f };
        inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
        inputs.environmentalCloudModel.mAlbedo = { .9f, .9f, .9f };
        inputs.environmentalCloudModel.mExtinctionPerMeter = { .02f, .02f, .02f };
        inputs.environmentalCloudModel.mDetailErosion = 0.f;
        inputs.environmentalCloudModel.mRainAmount = inputs.environmentalCloudModel.mRainMinimum = 0.f;
        inputs.eyePosition = { 64002.f, -71132.f, 5400.f };
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.solarRadiance = {};
        inputs.skyIrradiance = inputs.groundIrradiance = { .05f, .05f, .05f };
        inputs.weatherWeight = 0.f;
        inputs.inScattering = inputs.outScattering = {};
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(20000.f, false); };
        for (const float skyBrightness : { 1.f, 0.f })
        {
            inputs.skyColor = inputs.fogColor = { skyBrightness, skyBrightness, skyBrightness, 1.f };
            const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            double radiance = 0.0;
            double opacity = 0.0;
            for (const osg::Vec4f& pixel : volume.pixels)
            {
                radiance += pixel.r();
                opacity += 1.f - pixel.a();
            }
            ASSERT_GT(opacity, .1);
            // Isotropic scattering receives one half of each constant hemisphere, times droplet albedo.
            const double expected = .9 * .5 * (skyBrightness + .05);
            EXPECT_NEAR(radiance / opacity, expected, .015) << "sky brightness=" << skyBrightness;
        }
    }

    /// Follow the authored night sky instead of carrying daylight diffuse brightness into cloud bodies.
    TEST(AtmosphereShaderTest, lowCloudSkyLightingFollowsNightPalette)
    {
        double daylight = 0.0;
        for (const std::string_view phase : { "Day", "Night" })
        {
            NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", phase);
            inputs.width = inputs.height = 8;
            inputs.cloudParameters = {};
            inputs.mistParameters = {};
            inputs.fogVolumeParameters = {};
            inputs.lowCloudEnvironmentInfluence = 0.f;
            inputs.lowCloudParameters = { 1.f, .2f, 4000.f, 9800.f };
            inputs.environmentalCloudModel.mCoverageMinimum = 1.f;
            inputs.environmentalCloudModel.mDetailErosion = 0.f;
            inputs.eyePosition = { 64002.f, -71132.f, 5400.f };
            inputs.viewDirection = { 1.f, 0.f, 0.f };
            inputs.solarRadiance = {};
            if (phase == "Night")
                inputs.sunDirection = { 0.f, 0.f, -1.f, 0.f };
            inputs.fogNear = 1e8f;
            inputs.fogFar = 2e8f;
            inputs.depth = [](int, int) { return normalizedDepthForViewDistance(20000.f, false); };
            const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            double radiance = 0.0;
            for (const osg::Vec4f& pixel : volume.pixels)
                radiance += pixel.r() + pixel.g() + pixel.b();
            if (phase == "Day")
            {
                daylight = radiance;
                ASSERT_GT(daylight, .1);
            }
            else
                EXPECT_LT(radiance, daylight * .15);
        }
    }

    /// Shadow local mist with the same cloud cover that attenuates incident sunlight on low condensate.
    TEST(AtmosphereShaderTest, localMistReceivesCloudShadow)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 8;
        inputs.verticalFov = 10.f;
        inputs.eyePosition = { 0.f, 0.f, 2000.f };
        inputs.mistParameters = { .3f, 900.f, 5200.f, 0.f };
        inputs.solarRadiance = { 1.f, 1.f, 1.f };
        inputs.sunDirection = { 0.f, 0.f, 1.f, 0.f };
        inputs.skyIrradiance = inputs.groundIrradiance = {};
        inputs.inScattering = inputs.outScattering = {};
        inputs.skyColor = inputs.fogColor = { 0.f, 0.f, 0.f, 1.f };
        inputs.fogNear = 1e8f;
        inputs.fogFar = 2e8f;
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(10000.f, false); };
        const PassImage exposed = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.cloudParameters = { 1.f, 4.f, 15000.f, 30000.f };
        inputs.cloudModel.mPhysicalScale = .7f;
        inputs.cloudModel.mCoverageMinimum = 1.f;
        inputs.cloudModel.mDetailErosion = 0.f;
        const PassImage shaded = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        double exposedLight = 0.0;
        double shadedLight = 0.0;
        for (std::size_t index = 0; index < exposed.pixels.size(); ++index)
        {
            EXPECT_NEAR(exposed.pixels[index].a(), shaded.pixels[index].a(), .002f)
                << "The opaque surface terminates each view ray below the overhead cloud";
            exposedLight += exposed.pixels[index].r();
            shadedLight += shaded.pixels[index].r();
        }
        ASSERT_GT(exposedLight, .01);
        EXPECT_LT(shadedLight, exposedLight * .5);
    }

    /// Compare approach and head-on condensate opacity against a converged dense integral of the same world field.
    TEST(AtmosphereShaderTest, lowCloudApproachMatchesDenseWorldField)
    {
        NativePassInputs inputs = makePackagedAtmosphereInputs("Overcast", "Day");
        inputs.width = 32;
        inputs.height = 16;
        inputs.cloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        inputs.lowCloudParameters = { .253974f, .763042f, 3740.32f, 9954.81f };
        inputs.mediumReferenceParameters = { 38.f, .45f, 6000.f, 0.f };
        inputs.cloudAdvectionOffsetWorldUnits = { 914.f, 421.9f };
        inputs.farPlane = 229376.f;
        inputs.viewDirection = { .919f, .303f, -.255f };
        inputs.viewDirection.normalize();
        for (const osg::Vec3f eye : { osg::Vec3f(48659.f, -76196.f, 11449.f), osg::Vec3f(54300.f, -74317.f, 9740.f),
                 osg::Vec3f(64002.f, -71132.f, 7180.f), osg::Vec3f(65500.f, -70600.f, 6765.f) })
        {
            SCOPED_TRACE(eye.z());
            inputs.eyePosition = eye;
            inputs.renderCloudDensitySlice = false;
            const PassImage actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            inputs.renderCloudDensitySlice = true;
            inputs.cloudDensitySliceQuantity = 28;
            inputs.cloudDensitySliceAxis = 1;
            const PassImage reference = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            double meanError = 0.0;
            float maximumError = 0.f;
            float maximumOpacity = 0.f;
            for (std::size_t index = 0; index < actual.pixels.size(); ++index)
            {
                const float error = std::abs(actual.pixels[index].a() - reference.pixels[index].a());
                maximumError = std::max(maximumError, error);
                meanError += error;
                maximumOpacity = std::max(maximumOpacity, 1.f - reference.pixels[index].a());
            }
            EXPECT_GT(maximumOpacity, .1f);
            EXPECT_LT(maximumError, .02f);
            EXPECT_LT(meanError / actual.pixels.size(), .0025);
        }
    }

    /// Resolve low-cloud illumination against converged transport from both shallow and downward approach rays.
    TEST(AtmosphereShaderTest, lowCloudTransportMatchesDenseWorldField)
    {
        float maximumObservedError = 0.f;
        for (const char* phase : { "Day", "Sunset" })
            for (const int quality : { 0, 1, 2, 3 })
            {
                SCOPED_TRACE(phase);
                SCOPED_TRACE(quality);
                NativePassInputs inputs = makePackagedAtmosphereInputs("Foggy", phase);
                inputs.quality = quality;
                inputs.width = 32;
                inputs.height = 16;
                inputs.cloudParameters = {};
                inputs.incomingCloudParameters = {};
                inputs.mistParameters = {};
                inputs.fogVolumeParameters = {};
                inputs.fogNear = 1e8f;
                inputs.fogFar = 2e8f;
                inputs.farPlane = 229376.f;
                inputs.lowCloudParameters = { .253974f, .763042f, 3740.32f, 9954.81f };
                inputs.mediumReferenceParameters = { 38.f, .45f, 6000.f, 0.f };
                inputs.cloudAdvectionOffsetWorldUnits = { 914.f, 421.9f };
                inputs.verticalFov = inputs.previousVerticalFov = 35.f;
                for (const osg::Vec3f eye :
                    { osg::Vec3f(48659.f, -76196.f, 11449.f), osg::Vec3f(65500.f, -70600.f, 6765.f) })
                    for (const float elevation : { -.255f, .02f })
                    {
                        SCOPED_TRACE(testing::Message() << "height=" << eye.z() << " elevation=" << elevation);
                        inputs.eyePosition = eye;
                        inputs.viewDirection = { .919f, .303f, elevation };
                        inputs.viewDirection.normalize();
                        inputs.cloudTransportReferenceSubdivisions = 1;
                        const PassImage actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                        inputs.cloudTransportReferenceSubdivisions = 8;
                        const PassImage fine = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                        inputs.cloudTransportReferenceSubdivisions = 16;
                        const PassImage converged = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                        ASSERT_EQ(actual.pixels.size(), static_cast<std::size_t>(inputs.width * inputs.height));
                        ASSERT_EQ(fine.pixels.size(), actual.pixels.size());
                        ASSERT_EQ(converged.pixels.size(), actual.pixels.size());
                        double meanError = 0.0;
                        float maximumError = 0.f;
                        float maximumOpacity = 0.f;
                        for (std::size_t index = 0; index < actual.pixels.size(); ++index)
                        {
                            maximumOpacity = std::max(maximumOpacity, 1.f - converged.pixels[index].a());
                            for (int channel = 0; channel < 4; ++channel)
                            {
                                EXPECT_NEAR(fine.pixels[index][channel], converged.pixels[index][channel], .01f);
                                const float error
                                    = std::abs(actual.pixels[index][channel] - converged.pixels[index][channel]);
                                maximumError = std::max(maximumError, error);
                                meanError += error;
                            }
                        }
                        EXPECT_GT(maximumOpacity, .1f);
                        EXPECT_LT(maximumError, .02f);
                        EXPECT_LT(meanError / (actual.pixels.size() * 4), .0025);
                        maximumObservedError = std::max(maximumObservedError, maximumError);
                    }
            }
        RecordProperty("maximum_transport_error", std::to_string(maximumObservedError));
    }

    /// Preserve fractional cloud motion over continuous opaque terrain instead of snapping history texels.
    TEST(AtmosphereShaderTest, cloudHistoryOverTerrainPreservesSubpixelMotion)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 32;
        inputs.sceneWidth = inputs.sceneHeight = 64;
        inputs.verticalFov = inputs.previousVerticalFov = 90.f;
        inputs.historyValid = true;
        inputs.previousEyeOffset = { 0.f, 31.25f, 0.f };
        inputs.depth = inputs.previousDepth = [](int, int) { return normalizedDepthForViewDistance(10000.f, false); };
        inputs.passInput = [](int, int) { return osg::Vec4f(0.15f, 0.15f, 0.15f, 0.7f); };
        inputs.volumeMetadataInput = inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(2000.f), 0.3f, encodeAtmosphereDistance(10000.f), 2.f);
        };
        inputs.atmosphereHistory = [](int x, int) {
            const float value = x % 2 == 0 ? 0.1f : 0.3f;
            return osg::Vec4f(value, value, value, 0.7f);
        };
        PassImage metadata;
        const PassImage result = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs, &metadata);
        ASSERT_EQ(result.width, 32);
        ASSERT_GE(metadata.at(16, 17).a(), 0.f);
        // A 31.25-unit lateral translation at 2,000 units moves this near-axis ray by a quarter texel.
        EXPECT_NEAR(result.at(16, 17).r(), 0.15f, 0.003f);
        EXPECT_NEAR(result.at(16, 17).a(), 0.7f, 0.001f);

        inputs.atmosphereMetadataHistory = [](int x, int) {
            return osg::Vec4f(
                encodeAtmosphereDistance(2000.f), 0.3f, encodeAtmosphereDistance(x % 2 == 0 ? 10000.f : 20000.f), 2.f);
        };
        const PassImage silhouette = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        EXPECT_NEAR(silhouette.at(16, 17).r(), 0.1f, 0.003f)
            << "Filtering must not borrow a neighboring integral terminated at another surface";
    }

    /// Keep stationary cloud history centered on the same reconstruction texel.
    TEST(AtmosphereShaderTest, stationaryCloudReprojectionHasNoPixelMotion)
    {
        NativePassInputs inputs;
        inputs.width = 48;
        inputs.height = 30;
        inputs.renderReprojectionCoordinates = true;
        inputs.historyValid = true;
        const PassImage reprojection = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(reprojection.pixels.empty());

        float maximumMotion = 0.f;
        double meanMotion = 0.0;
        for (const osg::Vec4f& pixel : reprojection.pixels)
        {
            const float motion = std::hypot(pixel.x(), pixel.y());
            maximumMotion = std::max(maximumMotion, motion);
            meanMotion += motion;
        }
        meanMotion /= static_cast<double>(reprojection.pixels.size());
        RecordProperty("maximum_motion_pixels", std::to_string(maximumMotion));
        RecordProperty("mean_motion_pixels", std::to_string(meanMotion));
        EXPECT_LT(maximumMotion, 0.001f);
    }

    /// Preserve clear-sky cloud history when neighboring far-plane ray lengths differ across the frustum.
    TEST(AtmosphereShaderTest, clearSkyFarPlaneDistanceDoesNotRejectCloudHistory)
    {
        NativePassInputs inputs;
        inputs.width = 48;
        inputs.height = 30;
        inputs.sceneWidth = 96;
        inputs.sceneHeight = 60;
        inputs.historyValid = true;
        inputs.frameNumber = 9;
        inputs.passInput = [](int, int) { return osg::Vec4f(0.8f, 0.7f, 0.6f, 0.35f); };
        inputs.volumeMetadataInput = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.45f, encodeAtmosphereDistance(60000.f), 2.f);
        };
        inputs.atmosphereHistory = [](int, int) { return osg::Vec4f(0.8f, 0.7f, 0.6f, 0.35f); };
        inputs.atmosphereMetadataHistory = [](int, int) {
            return osg::Vec4f(encodeAtmosphereDistance(18000.f), 0.65f, encodeAtmosphereDistance(20000.f), 2.f);
        };
        inputs.depth = [](int, int) { return 1.f; };
        inputs.previousDepth = inputs.depth;

        const PassImage resolved = renderNativeAtmospherePass("reprojectAtmosphereVolume", inputs);
        ASSERT_FALSE(resolved.pixels.empty());
        std::size_t preservedPixels = 0;
        for (int y = 0; y < resolved.height; ++y)
            for (int x = 0; x < resolved.width; ++x)
            {
                const int blockPhase = ((x / 2) + (y / 2)) % 2;
                const int phase = (inputs.frameNumber + blockPhase) % 4;
                const bool traced = x % 2 == phase % 2 && y % 2 == phase / 2;
                if (traced)
                    continue;
                EXPECT_NEAR(resolved.at(x, y).r(), 0.8f, 0.002f);
                EXPECT_NEAR(resolved.at(x, y).a(), 0.35f, 0.002f);
                ++preservedPixels;
            }
        EXPECT_EQ(preservedPixels, static_cast<std::size_t>(resolved.width * resolved.height * 3 / 4));
    }

    /// Verify that temporal accumulation denoises overlapping low clouds, weather clouds, and mist without shell
    /// sampling.
    TEST(AtmosphereShaderTest, mixedAtmosphericStrataConvergeWithoutPersistentGrain)
    {
        NativePassInputs volumeInputs;
        volumeInputs.width = 32;
        volumeInputs.height = 20;
        volumeInputs.cloudParameters = { 0.6f, 0.95f, 26000.f, 18000.f };
        volumeInputs.lowCloudParameters = { 0.32f, 0.8f, 4200.f, 11000.f };
        volumeInputs.lowCloudEnvironmentInfluence = 1.f;
        volumeInputs.mistParameters = { 0.18f, 150.f, 2800.f, 0.12f };
        volumeInputs.eyePosition = { 0.f, 0.f, 700.f };
        volumeInputs.viewDirection = { 1.f, 0.f, 0.08f };
        volumeInputs.farPlane = 300000.f;

        ProductionAtmosphereFrame previousFrame;
        double rawVariation = 0.0;
        double resolvedVariation = 0.0;
        std::size_t comparedRawValues = 0;
        std::size_t comparedResolvedValues = 0;

        for (int frameNumber = 0; frameNumber < 8; ++frameNumber)
        {
            volumeInputs.frameNumber = frameNumber;
            volumeInputs.historyValid = frameNumber > 0;
            ProductionAtmosphereFrame frame
                = renderProductionAtmosphereVolumeFrame(volumeInputs, frameNumber > 0 ? &previousFrame : nullptr);
            ASSERT_FALSE(frame.mRawVolume.pixels.empty());
            ASSERT_FALSE(frame.mResolvedVolume.pixels.empty());

            if (frameNumber >= 4)
            {
                for (std::size_t pixelIndex = 0; pixelIndex < frame.mRawVolume.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        rawVariation += std::abs(frame.mRawVolume.pixels[pixelIndex][channel]
                            - previousFrame.mRawVolume.pixels[pixelIndex][channel]);
                        ++comparedRawValues;
                    }
                for (std::size_t pixelIndex = 0; pixelIndex < frame.mResolvedVolume.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        resolvedVariation += std::abs(frame.mResolvedVolume.pixels[pixelIndex][channel]
                            - previousFrame.mResolvedVolume.pixels[pixelIndex][channel]);
                        ++comparedResolvedValues;
                    }
            }

            previousFrame = std::move(frame);
        }

        ASSERT_GT(comparedRawValues, 0u);
        ASSERT_GT(comparedResolvedValues, 0u);
        const double meanRawVariation = rawVariation / static_cast<double>(comparedRawValues);
        const double meanResolvedVariation = resolvedVariation / static_cast<double>(comparedResolvedValues);
        EXPECT_GT(meanRawVariation, 1e-5);
        EXPECT_LT(meanResolvedVariation, meanRawVariation * 0.2);
    }

    TEST(AtmosphereShaderTest, largeCloudAdvectionRejectsHistoryWithoutMotionSmearing)
    {
        constexpr int referenceFrameCount = 4;
        const auto renderCloudReference = [](const osg::Vec2f& cloudAdvectionOffsetWorldUnits) {
            PassImage reference;
            for (int frameNumber = 0; frameNumber < referenceFrameCount; ++frameNumber)
            {
                NativePassInputs inputs;
                inputs.width = 32;
                inputs.height = 32;
                inputs.cloudParameters = { 0.62f, 0.9f, 48000.f, 22000.f };
                inputs.mistParameters = {};
                inputs.cloudAdvectionOffsetWorldUnits = cloudAdvectionOffsetWorldUnits;
                inputs.frameNumber = frameNumber;
                inputs.historyValid = true;
                const PassImage sample = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                if (reference.pixels.empty())
                {
                    reference.width = sample.width;
                    reference.height = sample.height;
                    reference.pixels.assign(sample.pixels.size(), osg::Vec4f());
                }
                for (std::size_t pixelIndex = 0; pixelIndex < sample.pixels.size(); ++pixelIndex)
                    reference.pixels[pixelIndex] += sample.pixels[pixelIndex] / referenceFrameCount;
            }
            return reference;
        };

        MWRender::CloudAdvectionController advectionController;
        advectionController.update(15.f, false, 1.25f, 30.f);
        const osg::Vec2f elapsedCloudAdvectionWorldUnits = advectionController.getState().mOffsetWorldUnits;
        const PassImage previousReference = renderCloudReference({});
        const PassImage currentReference = renderCloudReference(elapsedCloudAdvectionWorldUnits);
        ASSERT_EQ(previousReference.pixels.size(), currentReference.pixels.size());

        NativePassInputs currentInputs;
        currentInputs.width = 32;
        currentInputs.height = 32;
        currentInputs.cloudParameters = { 0.62f, 0.9f, 48000.f, 22000.f };
        currentInputs.mistParameters = {};
        currentInputs.cloudAdvectionOffsetWorldUnits = elapsedCloudAdvectionWorldUnits;
        currentInputs.cloudAdvectionDeltaWorldUnits = elapsedCloudAdvectionWorldUnits;
        currentInputs.frameNumber = referenceFrameCount + 1;
        currentInputs.historyValid = true;
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", currentInputs);
        ASSERT_EQ(currentRaw.pixels.size(), currentReference.pixels.size());

        NativePassInputs currentMetadataInputs = currentInputs;
        const PassImage currentMetadata = renderNativeAtmosphereMetadata(currentMetadataInputs);
        NativePassInputs previousMetadataInputs = currentInputs;
        previousMetadataInputs.cloudAdvectionOffsetWorldUnits = {};
        const PassImage previousMetadata = renderNativeAtmosphereMetadata(previousMetadataInputs);

        NativePassInputs resolveInputs = currentInputs;
        resolveInputs.cloudAdvectionDeltaWorldUnits = elapsedCloudAdvectionWorldUnits;
        resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        resolveInputs.atmosphereHistory = [&previousReference](int x, int y) { return previousReference.at(x, y); };
        resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
        resolveInputs.atmosphereMetadataHistory
            = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        ASSERT_EQ(resolved.pixels.size(), currentRaw.pixels.size());

        NativePassInputs unrelatedHistoryInputs = resolveInputs;
        unrelatedHistoryInputs.atmosphereHistory = [](int, int) { return osg::Vec4f(100.f, 0.f, 100.f, 0.f); };
        const PassImage unrelatedHistory
            = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", unrelatedHistoryInputs);
        ASSERT_EQ(unrelatedHistory.pixels.size(), resolved.pixels.size());
        double historyContribution = 0.0;
        for (std::size_t pixelIndex = 0; pixelIndex < resolved.pixels.size(); ++pixelIndex)
            for (int channel = 0; channel < 3; ++channel)
            {
                historyContribution
                    += std::abs(resolved.pixels[pixelIndex][channel] - unrelatedHistory.pixels[pixelIndex][channel]);
            }

        EXPECT_NEAR(historyContribution, 0.0, 1e-6)
            << "A large single-frame cloud displacement must reject stale history rather than smear it";
        EXPECT_LT(meanRgbDifference(resolved, currentRaw), 1e-6)
            << "Fresh reconstruction must retain the current field independently of rejected history";
    }

    /// Keep the virtual shadow projection stable across mainland cells without positioning visible clouds.
    TEST(AtmosphereShaderTest, texturedCloudShadowProjectionPreservesWorldPosition)
    {
        for (float worldX : { 0.f, 600000.f, 2000000.f })
            for (float altitude : { 1200.f, 11999.f, 12000.f, 12001.f, 22000.f })
            {
                NativePassInputs inputs;
                inputs.width = inputs.height = 1;
                inputs.fullPrecisionCapture = true;
                inputs.renderTexturedCloudProjection = true;
                inputs.eyePosition = { worldX, 0.f, altitude };
                inputs.viewDirection = { 0.2f, 0.3f, 1.f };
                inputs.viewDirection.normalize();
                const osg::Vec3d origin(inputs.eyePosition);
                osg::Vec3d direction(inputs.viewDirection);
                direction.normalize();
                const double distance = (12000.0 - origin.z()) / direction.z();
                const auto expected = origin + direction * distance;
                const auto initial = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(initial.pixels.size(), 1u);
                EXPECT_EQ(initial.pixels[0].w(), 1.f);
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(initial.pixels[0][channel] * 16000.f, expected[channel], 2.0);
                inputs.eyePosition += inputs.viewDirection * 1000.f;
                const auto approached = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(approached.pixels.size(), 1u);
                for (int channel = 0; channel < 4; ++channel)
                    EXPECT_NEAR(approached.pixels[0][channel], initial.pixels[0][channel], 0.000125f);
            }
    }

    /// Keep surface sunlight attenuation continuous as terrain crosses the shadow projection datum.
    TEST(AtmosphereShaderTest, texturedCloudShadowTransmissionHasNoAltitudeCutoff)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.fullPrecisionCapture = true;
        inputs.renderTexturedCloudTransmission = true;
        inputs.texturedClouds.mEnabled = true;
        inputs.texturedClouds.mWeights = { 1.f, 0.f };
        inputs.texturedClouds.mOpacity
            = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(0.5f, 0.f, 0.f, 1.f); });
        for (const osg::Vec3f direction : { osg::Vec3f(0.2f, 0.3f, 1.f), osg::Vec3f(1.f, 0.f, 0.011f) })
        {
            inputs.viewDirection = direction;
            for (const float altitude : { 0.f, 11999.f, 12000.f, 12001.f, 22000.f, 1000000.f })
            {
                SCOPED_TRACE(altitude);
                inputs.eyePosition = { 30000.f, 24000.f, altitude };
                const auto actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(actual.pixels.size(), 1u);
                EXPECT_NEAR(actual.pixels[0].x(), 0.0625f, 0.00001f);
            }
        }
    }

    /// The production textured cloud pass must preserve mesh UVs and vertex fade at ground and flying altitudes.
    TEST(AtmosphereShaderTest, texturedCloudsPreserveAuthoredUvAndVertexFade)
    {
        NativePassInputs inputs;
        inputs.renderAuthoredCloudPass = true;
        inputs.fullPrecisionCapture = true;
        inputs.lastShader = [](int x, int y) { return osg::Vec4f((x + 0.5f) / 64, (y + 0.5f) / 64, 0.4f, 0.8f); };
        for (float altitude : { 0.f, 8000.f, 12000.f, 100000.f })
        {
            inputs.eyePosition = { 600000.f, -300000.f, altitude };
            auto actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(actual.pixels.size(), 64u * 64u);
            for (int x : { 8, 32, 55 })
                for (int y : { 8, 32, 55 })
                {
                    const auto pixel = actual.at(x, y);
                    EXPECT_NEAR(pixel.x(), (0.2f + 0.5f * (y + 0.5f) / 64) * 0.7f, 0.0005f);
                    EXPECT_NEAR(pixel.y(), (0.1f + 0.5f * (x + 0.5f) / 64) * 0.8f, 0.0005f);
                    EXPECT_NEAR(pixel.z(), 0.4f * 0.9f, 0.0005f);
                    EXPECT_NEAR(pixel.w(), 0.8f * 0.25f * 0.6f, 0.0005f);
                }
        }
    }

    /// Sunlight follows authored dome UVs and weather animation at every observer position and altitude.
    TEST(AtmosphereShaderTest, texturedCloudDomeMappingIsCameraRelative)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.fullPrecisionCapture = true;
        inputs.renderTexturedCloudDome = true;
        inputs.texturedClouds.mEnabled = true;
        inputs.texturedClouds.mDomeMapping = makeColorTexture(
            64, 64, [](int x, int y) { return osg::Vec4f((x + 0.5f) / 64 * 0.5f, (y + 0.5f) / 64 * 0.5f, 0.5f, 1.f); });
        inputs.texturedClouds.mDomeMapping->setInternalFormat(GL_RGBA32F);
        inputs.texturedClouds.mOpacity = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(0.5f, 0, 0, 1); });
        inputs.viewDirection = { 0.6f, 0.f, 0.8f };
        std::optional<osg::Vec4f> reference;
        for (const osg::Vec3f position :
            { osg::Vec3f(0, 0, 0), osg::Vec3f(600000, -400000, 8000), osg::Vec3f(-800000, 2000000, 100000) })
        {
            inputs.eyePosition = position;
            auto actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(actual.pixels.size(), 1u);
            if (reference)
                EXPECT_EQ(actual.pixels[0], *reference);
            reference = actual.pixels[0];
            EXPECT_NEAR(actual.pixels[0].x(), 2.f / 3.f, 0.0001f);
            EXPECT_NEAR(actual.pixels[0].y(), 0.5f, 0.0001f);
            EXPECT_NEAR(actual.pixels[0].z(), 0.5f, 0.0001f);
            EXPECT_NEAR(actual.pixels[0].w(), std::pow(0.75f, 4.f), 0.0001f);
        }
        inputs.texturedClouds.mTransforms[0] = { 0.f, 1.f, 0.f, 0.2f };
        auto rotated = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(rotated.pixels.size(), 1u);
        EXPECT_NEAR(rotated.pixels[0].x(), 0.5f, 0.0001f);
        EXPECT_NEAR(rotated.pixels[0].y(), 1.f / 3.f + 0.2f, 0.0001f);
        inputs.viewDirection = { 0.f, 0.f, -1.f };
        auto below = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(below.pixels.size(), 1u);
        EXPECT_EQ(below.pixels[0].z(), 0.f);
        EXPECT_EQ(below.pixels[0].w(), 1.f);
    }

    /// Compare the selectable MGE response against the original parity equation from commit 15ee5e5d84.
    TEST(AtmosphereShaderTest, mgeAppearancePreservesOriginalParitySkyEquation)
    {
        const auto reference = [](const NativePassInputs& inputs) {
            osg::Vec3d direction(inputs.viewDirection);
            direction.normalize();
            osg::Vec3d sun(inputs.sunDirection.x(), inputs.sunDirection.y(), inputs.sunDirection.z());
            sun.normalize();
            const double cosine = direction * sun;
            const double altitude = std::pow(std::max(1.0 + sun.z(), 1e-4), 10.0);
            const double brightness = std::clamp(1.0 - std::exp2(-1.9 * altitude), 0.0, 1.0);
            const double horizon = std::clamp(std::exp(-4.0 * sun.z()), 0.0, 1.0) * std::clamp(altitude, 0.0, 1.0);
            const double forward = 1.58 / std::max(1.24 - cosine, 0.02) * horizon;
            const double depth = 1.33 * std::exp(-2.0 * std::clamp(direction.z(), 0.0, 1.0));
            const double skyAmount
                = 1.0 - std::pow(std::clamp(1.0 - 2.22 * std::clamp(direction.z() - 0.075, 0.0, 1.0), 0.0, 1.0), 1.15);
            const osg::Vec3d blueSky(0.371, 0.637, 1.108);
            osg::Vec3d result;
            for (int channel = 0; channel < 3; ++channel)
            {
                const double directional = std::lerp(static_cast<double>(inputs.inScattering[channel]),
                    static_cast<double>(inputs.outScattering[channel]), 0.5 * (1.0 + cosine));
                const double extinction = depth * directional * (2.8 + 4.3 / std::max(altitude, 1e-4) + 0.7 * forward);
                const double integral = -std::expm1(-extinction) / std::max(extinction, 1e-4);
                const double sky = std::lerp(static_cast<double>(inputs.skyColor[channel]), blueSky[channel], 0.44);
                const double scattering
                    = (0.125 * forward + sky * (1.0 - 0.09 * forward)) * integral * (1.17 * depth + 0.89) * brightness;
                const double background = std::lerp(static_cast<double>(inputs.fogColor[channel]),
                    static_cast<double>(inputs.skyColor[channel]), skyAmount);
                result[channel] = std::lerp(background, scattering, static_cast<double>(inputs.weatherWeight));
            }
            return result;
        };
        for (bool useBuffer : { false, true })
            for (float sunHeight : { -0.2f, -0.05f, 0.f, 0.2f, 0.8f })
            {
                NativePassInputs inputs;
                inputs.width = 1;
                inputs.height = 1;
                inputs.atmosphereAppearance = 1;
                inputs.useUniformBuffer = useBuffer;
                inputs.renderCloudSkyReference = true;
                inputs.weatherWeight = 0.65f;
                inputs.sunDirection = { std::sqrt(1.f - sunHeight * sunHeight), 0.f, sunHeight, 0.f };
                inputs.viewDirection = { 0.8f, 0.2f, 0.4f };
                const auto actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(actual.pixels.size(), 1u);
                const auto expected = reference(inputs);
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(actual.pixels[0][channel], expected[channel], 0.002);
            }
    }

    /// Match parity commit 15ee5e5d84's distance transfer at all observer heights, including linear fog.
    TEST(AtmosphereShaderTest, mgeAppearancePreservesOriginalFogTransport)
    {
        NativePassInputs inputs;
        inputs.width = inputs.height = 1;
        inputs.fullPrecisionCapture = true;
        inputs.renderFogTransport = true;
        inputs.atmosphereAppearance = 1;
        for (bool exponential : { false, true })
        {
            inputs.exponentialFog = exponential;
            const double scale = exponential ? 4.4 : 1.0;
            const double start = inputs.fogNear / scale;
            const double range = (inputs.fogFar - start) / scale;
            const double expected = (inputs.fogFar * 0.5 - start) / range;
            for (float height : { 0.f, 10000.f, 100000.f })
                for (float slope : { -0.5f, 0.f, 0.5f })
                {
                    inputs.eyePosition.z() = height;
                    inputs.viewDirection = { 1.f, 0.f, slope };
                    const auto actual = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_EQ(actual.pixels.size(), 1u);
                    EXPECT_NEAR(actual.pixels[0].x(), expected, 0.00001);
                    EXPECT_NEAR(actual.pixels[0].y(), exponential ? std::exp(-expected) : 1.0 - expected, 0.00001);
                }
        }
    }

    /// Compare MGE XE Sunshafts.fx v0.02a's twenty-sample ray response for a completely exposed solar source.
    TEST(AtmosphereShaderTest, sunShaftsMatchMgeRadialResponse)
    {
        NativePassInputs inputs;
        inputs.fullPrecisionCapture = true;
        // The filtered source is fully visible; foreground destination pixels may receive scattered light.
        inputs.passInput = [](int, int) { return osg::Vec4f(0, 0, 0, 1); };
        for (float weatherWeight : { 0.f, .25f, 1.f })
            for (float sunVisibility : { 0.3f, 1.f })
            {
                inputs.sunVisibility = sunVisibility;
                inputs.weatherWeight = weatherWeight;
                for (bool foreground : { false, true })
                {
                    inputs.depth = [foreground](int, int) {
                        return foreground ? normalizedDepthForViewDistance(3000.f, false)
                                          : (SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f);
                    };
                    const auto actual = renderNativeAtmospherePass("integrateShafts", inputs);
                    ASSERT_EQ(actual.pixels.size(), 64u * 64u);
                    for (const int x : { 3, 12, 24, 40, 60 })
                    {
                        const double dx = (x + 0.5) / 64.0 - 0.5;
                        const double dy = 0.5 / 64.0;
                        const double radius = std::hypot(dx, dy);
                        double integral = 0;
                        for (int sample = 1; sample <= 20; ++sample)
                        {
                            const double offset = sample / 20.0 * std::min(0.25, radius);
                            integral += std::exp(-1.10 * (radius - offset) / (0.125 + offset))
                                * std::pow(1.0 - offset / 0.25, 1.5);
                        }
                        // DistantLand::updatePostShader raises post-process sun visibility in fair weather.
                        const double postVisibility = std::lerp(double(sunVisibility), 1.0, .333 * weatherWeight);
                        const double strength = 1.7 * (1.0 - std::pow(1.0 - postVisibility, 2.0));
                        const double opacity = integral / 20.0 * strength * (radius / 0.25 * 0.7 + 0.3);
                        const double enhancement = 1.0 + 1.25 * std::pow(opacity, 3.0);
                        const auto pixel = actual.at(x, 32);
                        EXPECT_NEAR(pixel.a(), opacity, 0.0005);
                        EXPECT_NEAR(pixel.r(), inputs.sunColor.r() * enhancement, 0.001);
                        EXPECT_NEAR(pixel.g(), 0.8 * inputs.sunColor.g() * enhancement, 0.001);
                        EXPECT_NEAR(pixel.b(), 0.8 * inputs.sunColor.b() * enhancement, 0.001);
                    }
                }
            }
    }

    /// A finite foreground occluder shapes the complete source/filter/ray chain without extinguishing its surrounding
    /// gaps.
    TEST(AtmosphereShaderTest, sunShaftsSurvivePartialSolarOcclusion)
    {
        NativePassInputs inputs;
        inputs.depth = [](int x, int y) {
            return std::abs(x - 32) <= 5 && std::abs(y - 32) <= 5 ? normalizedDepthForViewDistance(3000.f, false)
                                                                  : (SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f);
        };
        const auto occluded = renderProductionShafts(inputs);
        ASSERT_FALSE(occluded.pixels.empty());
        inputs.depth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f; };
        const auto exposed = renderProductionShafts(inputs);
        ASSERT_EQ(exposed.pixels.size(), occluded.pixels.size());
        double remaining = 0.0;
        double removed = 0.0;
        for (size_t index = 0; index < exposed.pixels.size(); ++index)
        {
            remaining += occluded.pixels[index].a();
            removed += exposed.pixels[index].a() - occluded.pixels[index].a();
        }
        EXPECT_GT(remaining / exposed.pixels.size(), 0.01);
        EXPECT_GT(removed / exposed.pixels.size(), 0.01);
    }

    /// Attenuate the finite emitter through native volumes while retaining MGE's textured-sky depth source.
    TEST(AtmosphereShaderTest, sunShaftSourceRespectsVolumeTransmittanceAndCloudMode)
    {
        for (bool useBuffer : { false, true })
        {
            NativePassInputs inputs;
            inputs.useUniformBuffer = useBuffer;
            inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0, 0, 0, 1); };
            const auto exposed = renderProductionShafts(inputs);
            ASSERT_FALSE(exposed.pixels.empty());
            for (float transmittance : { 0.f, .2f, .75f })
            {
                inputs.volumeHistoryInput
                    = [transmittance](int, int) { return osg::Vec4f(.3f, .4f, .5f, transmittance); };
                const auto throughVolume = renderProductionShafts(inputs);
                ASSERT_EQ(exposed.pixels.size(), throughVolume.pixels.size());
                for (std::size_t pixel = 0; pixel < exposed.pixels.size(); ++pixel)
                    EXPECT_NEAR(throughVolume.pixels[pixel].a(), exposed.pixels[pixel].a() * transmittance, .0005f);
            }
            inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0, 0, 0, 0); };
            inputs.volumetricCloudsEnabled = false;
            const auto textured = renderProductionShafts(inputs);
            EXPECT_EQ(exposed.pixels, textured.pixels);
            inputs.volumetricCloudsEnabled = true;
            inputs.cloudSunOcclusion = false;
            const auto independent = renderProductionShafts(inputs);
            ASSERT_FALSE(independent.pixels.empty());
            for (const auto& pixel : independent.pixels)
                EXPECT_EQ(pixel.a(), 0.f);
        }
    }

    /// Keep mod-authored ray strength functional without tying source visibility to the mist concentration.
    TEST(AtmosphereShaderTest, sunShaftStrengthIsIndependentOfMistDensity)
    {
        NativePassInputs inputs;
        inputs.mistParameters = { 0.f, 800.f, 4500.f, 1.f };
        const auto clear = renderProductionShafts(inputs);
        ASSERT_FALSE(clear.pixels.empty());
        inputs.mistParameters.x() = 3.f;
        const auto mist = renderProductionShafts(inputs);
        ASSERT_EQ(clear.pixels.size(), mist.pixels.size());
        EXPECT_EQ(clear.pixels, mist.pixels);
        inputs.mistParameters.w() = .5f;
        const auto dimmed = renderProductionShafts(inputs);
        ASSERT_EQ(clear.pixels.size(), dimmed.pixels.size());
        for (std::size_t pixel = 0; pixel < clear.pixels.size(); ++pixel)
            EXPECT_NEAR(dimmed.pixels[pixel].a(), .5f * clear.pixels[pixel].a(), .0005f);
        inputs.mistParameters.w() = 0.f;
        const auto dark = renderProductionShafts(inputs);
        ASSERT_EQ(clear.pixels.size(), dark.pixels.size());
        for (const auto& pixel : dark.pixels)
            EXPECT_EQ(pixel.a(), 0.f);
    }

    /// Match MGE's light-veil composition on foreground geometry while preserving the engine's scene alpha.
    TEST(AtmosphereShaderTest, sunShaftCompositeMatchesMgeBlend)
    {
        NativePassInputs inputs;
        const osg::Vec4f landscape(0.12f, 0.08f, 0.04f, 0.7f);
        const osg::Vec4f ray(0.7f, 0.5f, 0.3f, 0.4f);
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(3000.f, false); };
        inputs.lastShader = [landscape](int, int) { return landscape; };
        inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        inputs.shaftInput = [ray](int, int) { return ray; };
        const auto composed = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composed.pixels.empty());
        const auto center = composed.at(composed.width / 2, composed.height / 2);
        for (int channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(center[channel], landscape[channel] * (1.f - 0.75f * ray.a()) + ray[channel] * ray.a(), 0.001f);
        EXPECT_NEAR(center.a(), landscape.a(), 0.001f);
        inputs.godraysEnabled = false;
        const auto disabled = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(disabled.pixels.empty());
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(disabled.at(32, 32)[channel], landscape[channel], 0.001f);
    }

    /// Preserve MGE's off-screen emitter range and independent toggle while an opaque source or a rearward sun
    /// contributes nothing.
    TEST(AtmosphereShaderTest, sunShaftsRespectMgeVisibilityAndToggle)
    {
        const auto opacity = [](const PassImage& image) {
            double result = 0.0;
            for (const auto& pixel : image.pixels)
                result += pixel.a();
            return result / image.pixels.size();
        };
        NativePassInputs inputs;
        inputs.atmosphereEnabled = false;
        inputs.sunlightEnabled = true;
        inputs.sunDirection = { 1.f, -1.f, 0.f, 0.f };
        const auto offscreen = renderProductionShafts(inputs);
        ASSERT_FALSE(offscreen.pixels.empty());
        EXPECT_GT(opacity(offscreen), 0.001);
        inputs.godraysEnabled = false;
        const auto disabled = renderProductionShafts(inputs);
        ASSERT_FALSE(disabled.pixels.empty());
        EXPECT_LT(opacity(disabled), 1e-6);
        inputs.godraysEnabled = true;
        inputs.sunDirection = { -1.f, 0.f, 0.f, 0.f };
        const auto behind = renderProductionShafts(inputs);
        ASSERT_FALSE(behind.pixels.empty());
        EXPECT_LT(opacity(behind), 1e-6);
        inputs.sunDirection = { 1.f, 0.f, 0.f, 0.f };
        inputs.depth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 1.f : 0.f; };
        const auto landscape = renderProductionShafts(inputs);
        ASSERT_FALSE(landscape.pixels.empty());
        EXPECT_LT(opacity(landscape), 1e-6);
    }

    /// The solar disc responds to actual texture opacity and to its toggle in both GL uniform binding modes.
    TEST(AtmosphereShaderTest, texturedCloudsOccludeSunIndependentlyOfScattering)
    {
        for (bool useBuffer : { false, true })
        {
            NativePassInputs inputs;
            inputs.useUniformBuffer = useBuffer;
            inputs.atmosphereEnabled = false;
            inputs.scatteringEnabled = false;
            inputs.sunlightEnabled = true;
            inputs.sunDirection = { 1.f, 0.f, 1.f, 0.f };
            inputs.viewDirection = { 1.f, 0.f, 1.f };
            inputs.lastShader = [](int, int) { return osg::Vec4f(0.1f, 0.1f, 0.1f, 0.6f); };
            inputs.texturedClouds.mEnabled = true;
            inputs.texturedClouds.mDomeMapping
                = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(0.5f, 0.5f, 1, 1); });
            inputs.texturedClouds.mOpacity = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(1, 0, 0, 1); });
            const auto blocked = renderNativeAtmospherePass("composite", inputs);
            ASSERT_FALSE(blocked.pixels.empty());
            const auto blockedSun = blocked.at(32, 32);
            EXPECT_NEAR(blockedSun.x(), 0.1f, 0.001f);
            EXPECT_NEAR(blockedSun.w(), 0.6f, 0.001f);
            inputs.cloudSunOcclusion = false;
            const auto bypassed = renderNativeAtmospherePass("composite", inputs);
            ASSERT_FALSE(bypassed.pixels.empty());
            EXPECT_GT(bypassed.at(32, 32).x(), 2.f);
            inputs.cloudSunOcclusion = true;
            inputs.texturedClouds.mOpacity = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(0, 0, 0, 1); });
            const auto clear = renderNativeAtmospherePass("composite", inputs);
            ASSERT_FALSE(clear.pixels.empty());
            EXPECT_NEAR(clear.at(32, 32).x(), bypassed.at(32, 32).x(), 0.002f);
        }
    }

    /// Thin cloud illumination and the authored glare texture are independent and never cross landscape depth.
    TEST(AtmosphereShaderTest, cloudBloomAndSunGlareStayInTheSky)
    {
        NativePassInputs inputs;
        inputs.atmosphereEnabled = false;
        inputs.sunlightEnabled = true;
        inputs.sunDirection = { 1.f, 0.10f, 1.f, 0.f };
        inputs.viewDirection = { 1.f, 0.f, 1.f };
        inputs.lastShader = [](int, int) { return osg::Vec4f(0.1f, 0.1f, 0.1f, 0.6f); };
        inputs.texturedClouds.mEnabled = true;
        inputs.texturedClouds.mDomeMapping
            = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(0.5f, 0.5f, 1, 1); });
        inputs.texturedClouds.mOpacity = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(0.3f, 0, 0, 1); });
        const auto bloom = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(bloom.pixels.empty());
        EXPECT_GT(bloom.at(32, 32).x(), 0.2f);
        inputs.cloudSunOcclusion = false;
        const auto noBloom = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(noBloom.pixels.empty());
        EXPECT_NEAR(noBloom.at(32, 32).x(), 0.1f, 0.002f);
        inputs.sunGlareTexture = makeColorTexture(4, 4, [](int, int) { return osg::Vec4f(1, 1, 1, 1); });
        inputs.sunGlareEnabled = true;
        const auto glare = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(glare.pixels.empty());
        EXPECT_GT(glare.at(32, 32).x(), noBloom.at(32, 32).x() + 0.01f);
        inputs.sunGlareAllowed = false;
        const auto replacedGlare = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(replacedGlare.pixels.empty());
        EXPECT_NEAR(replacedGlare.at(32, 32).x(), noBloom.at(32, 32).x(), 0.002f);
        inputs.sunGlareAllowed = true;
        inputs.cloudSunOcclusion = true;
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(3000.f, false); };
        const auto landscape = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(landscape.pixels.empty());
        EXPECT_NEAR(landscape.at(32, 32).x(), 0.1f, 0.001f);
        EXPECT_NEAR(landscape.at(32, 32).w(), 0.6f, 0.001f);
    }

    TEST(AtmosphereShaderTest, compositeCombinesHdrMistTransmittanceAndShafts)
    {
        NativePassInputs inputs;
        inputs.lastShader = [](int, int) { return osg::Vec4f(1.f, 2.f, 3.f, 0.8f); };
        inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.2f, 0.1f, 0.05f, 0.5f); };
        inputs.shaftInput = [](int, int) { return osg::Vec4f(0.1f, 0.2f, 0.3f, 1.f); };

        const PassImage composite = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composite.pixels.empty());
        const osg::Vec4f& center = composite.at(composite.width / 2, composite.height / 2);
        // MGE applies its alpha-weighted veil after the volume's radiance and transmittance.
        EXPECT_NEAR(center.x(), (1.f * .5f + .2f) * .25f + .1f, 0.002f);
        EXPECT_NEAR(center.y(), (2.f * .5f + .1f) * .25f + .2f, 0.002f);
        EXPECT_NEAR(center.z(), (3.f * .5f + .05f) * .25f + .3f, 0.002f);
        EXPECT_NEAR(center.w(), 0.8f, 0.002f);
        for (const osg::Vec4f& pixel : composite.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_TRUE(std::isfinite(pixel[channel]));
    }

    /// Keep atmospheric compositing deterministic so cloud coverage cannot project grain onto scene geometry.
    TEST(AtmosphereShaderTest, compositeDoesNotAddVolumeCorrelatedNoiseToGeometry)
    {
        const osg::Vec4f sceneColor(0.24f, 0.18f, 0.12f, 1.f);
        const osg::Vec4f volume(0.08f, 0.1f, 0.14f, 0.62f);
        const osg::Vec4f expected(sceneColor.x() * volume.w() + volume.x(), sceneColor.y() * volume.w() + volume.y(),
            sceneColor.z() * volume.w() + volume.z(), 1.f);
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 64;
        inputs.depth = [](int, int) { return normalizedDepthForViewDistance(12000.f, false); };
        inputs.lastShader = [sceneColor](int, int) { return sceneColor; };
        inputs.volumeHistoryInput = [volume](int, int) { return volume; };
        inputs.volumeMetadataInput = [](int, int) {
            const float sceneDistance = encodeAtmosphereDistance(12000.f);
            return osg::Vec4f(encodeAtmosphereDistance(8000.f), 1.f - 0.62f, sceneDistance, 2.f);
        };
        inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

        const PassImage composite = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composite.pixels.empty());
        const osg::Vec4f& first = composite.pixels.front();
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(first[channel], expected[channel], 1e-4f);
        for (const osg::Vec4f& pixel : composite.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_FLOAT_EQ(pixel[channel], first[channel]);
    }

    TEST(AtmosphereShaderTest, compositeRetainsPhysicalBeyondFarCloudsAcrossOpenSky)
    {
        NativePassInputs volumeInputs;
        volumeInputs.width = 64;
        volumeInputs.height = 64;
        volumeInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        volumeInputs.mistParameters = {};
        const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", volumeInputs);
        ASSERT_FALSE(raw.pixels.empty());
        NativePassInputs metadataInputs = volumeInputs;
        const PassImage metadata = renderNativeAtmosphereMetadata(metadataInputs);
        ASSERT_EQ(metadata.pixels.size(), raw.pixels.size());

        const osg::Vec4f skyColor(0.3f, 0.4f, 0.55f, 1.f);
        NativePassInputs compositeInputs = volumeInputs;
        compositeInputs.width = 128;
        compositeInputs.height = 128;
        compositeInputs.lastShader = [skyColor](int, int) { return skyColor; };
        compositeInputs.volumeHistoryInput = [&raw](int x, int y) { return raw.at(x, y); };
        compositeInputs.volumeHistoryWidth = raw.width;
        compositeInputs.volumeHistoryHeight = raw.height;
        compositeInputs.volumeMetadataInput = [&metadata](int x, int y) { return metadata.at(x, y); };
        compositeInputs.volumeMetadataWidth = metadata.width;
        compositeInputs.volumeMetadataHeight = metadata.height;
        compositeInputs.shaftInput = [](int, int) { return osg::Vec4f(); };
        const PassImage composite = renderNativeAtmospherePass("composite", compositeInputs);
        ASSERT_FALSE(composite.pixels.empty());

        std::size_t cloudPixelCount = 0;
        for (const osg::Vec4f& pixel : composite.pixels)
        {
            const float difference = std::abs(pixel.x() - skyColor.x()) + std::abs(pixel.y() - skyColor.y())
                + std::abs(pixel.z() - skyColor.z());
            if (difference > 0.002f)
                ++cloudPixelCount;
        }
        EXPECT_GT(cloudPixelCount, 64u);
    }

    TEST(AtmosphereShaderTest, disabledAtmospherePassChainPreservesTheScene)
    {
        const osg::Vec4f sceneColor(0.25f, 0.5f, 1.5f, 0.5f);
        NativePassInputs inputs;
        inputs.atmosphereEnabled = false;
        inputs.historyValid = true;
        inputs.cloudParameters = { 0.95f, 1.25f, 32000.f, 32000.f };
        inputs.mistParameters = { 1.5f, 1200.f, 6500.f, 1.f };
        inputs.lastShader = [sceneColor](int, int) { return sceneColor; };
        inputs.atmosphereHistory = [](int, int) { return osg::Vec4f(3.f, 2.f, 1.f, 0.1f); };

        const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(raw.pixels.empty());

        NativePassInputs resolveInputs = inputs;
        resolveInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        ASSERT_EQ(resolved.pixels.size(), raw.pixels.size());

        NativePassInputs shaftInputs = inputs;
        shaftInputs.volumeHistoryInput = [&resolved](int x, int y) { return resolved.at(x, y); };
        const PassImage shafts = renderProductionShafts(shaftInputs);
        ASSERT_EQ(shafts.pixels.size(), raw.pixels.size());

        NativePassInputs compositeInputs = inputs;
        compositeInputs.volumeHistoryInput = [&resolved](int x, int y) { return resolved.at(x, y); };
        compositeInputs.shaftInput = [&shafts](int x, int y) { return shafts.at(x, y); };
        const PassImage composite = renderNativeAtmospherePass("composite", compositeInputs);
        ASSERT_EQ(composite.pixels.size(), raw.pixels.size());

        for (std::size_t index = 0; index < raw.pixels.size(); ++index)
        {
            EXPECT_EQ(raw.pixels[index], osg::Vec4f(0.f, 0.f, 0.f, 1.f));
            EXPECT_EQ(resolved.pixels[index], raw.pixels[index]);
            EXPECT_EQ(shafts.pixels[index], osg::Vec4f());
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(composite.pixels[index][channel], sceneColor[channel], 1e-6f);
        }
    }

    TEST(AtmosphereShaderTest, compositePreservesTheSceneWhenVolumeTargetIsUnavailable)
    {
        const osg::Vec4f sceneColor(0.25f, 0.5f, 1.5f, 0.5f);
        NativePassInputs inputs;
        inputs.lastShader = [sceneColor](int, int) { return sceneColor; };
        inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(); };
        inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

        const PassImage composite = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composite.pixels.empty());
        for (const osg::Vec4f& pixel : composite.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(pixel[channel], sceneColor[channel], 1e-6f);
    }

    TEST(AtmosphereShaderTest, compositeUpsamplingRejectsMistAcrossDepthEdges)
    {
        NativePassInputs uniformDepthInputs;
        uniformDepthInputs.lastShader = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        uniformDepthInputs.depth = [](int, int) { return 0.5f; };
        uniformDepthInputs.volumeHistoryWidth = 32;
        uniformDepthInputs.volumeHistoryHeight = 32;
        uniformDepthInputs.volumeHistoryInput
            = [](int x, int) { return x < 16 ? osg::Vec4f(1.f, 0.f, 0.f, 1.f) : osg::Vec4f(0.f, 0.f, 1.f, 1.f); };
        uniformDepthInputs.volumeMetadataWidth = 32;
        uniformDepthInputs.volumeMetadataHeight = 32;
        uniformDepthInputs.volumeMetadataInput = [](int, int) {
            const float encodedNear = encodeAtmosphereDistance(2.f);
            return osg::Vec4f(encodedNear, 1.f, encodedNear, 1.f);
        };
        uniformDepthInputs.shaftInput = [](int, int) { return osg::Vec4f(); };
        const PassImage uniformDepth = renderNativeAtmospherePass("composite", uniformDepthInputs);
        ASSERT_FALSE(uniformDepth.pixels.empty());

        NativePassInputs depthEdgeInputs = uniformDepthInputs;
        depthEdgeInputs.depth = [](int x, int) {
            if (x < 32)
                return 0.5f;
            return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f;
        };
        depthEdgeInputs.volumeMetadataInput = [](int x, int) {
            const float sceneDistance = x < 16 ? 2.f : 20000.f;
            return osg::Vec4f(
                encodeAtmosphereDistance(sceneDistance), 1.f, encodeAtmosphereDistance(sceneDistance), 1.f);
        };
        const PassImage depthEdge = renderNativeAtmospherePass("composite", depthEdgeInputs);
        ASSERT_EQ(depthEdge.pixels.size(), uniformDepth.pixels.size());

        const osg::Vec4f& mixedLeft = uniformDepth.at(31, 32);
        const osg::Vec4f& rejectedLeft = depthEdge.at(31, 32);
        const osg::Vec4f& mixedRight = uniformDepth.at(32, 32);
        const osg::Vec4f& rejectedRight = depthEdge.at(32, 32);
        EXPECT_GT(rejectedLeft.x(), mixedLeft.x() + 0.2f);
        EXPECT_LT(rejectedLeft.z(), mixedLeft.z() - 0.2f);
        EXPECT_GT(rejectedRight.z(), mixedRight.z() + 0.2f);
        EXPECT_LT(rejectedRight.x(), mixedRight.x() - 0.2f);
    }

    TEST(AtmosphereShaderTest, compositeUpsamplingUsesAlignedBilinearWeights)
    {
        NativePassInputs inputs;
        inputs.width = 4;
        inputs.height = 4;
        inputs.lastShader = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        inputs.volumeHistoryWidth = 2;
        inputs.volumeHistoryHeight = 2;
        inputs.volumeHistoryInput
            = [](int x, int y) { return osg::Vec4f(static_cast<float>(x + 2 * y), 0.f, 0.f, 1.f); };
        inputs.volumeMetadataWidth = 2;
        inputs.volumeMetadataHeight = 2;
        inputs.volumeMetadataInput = [](int, int) {
            const float encodedFar = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(encodedFar, 1.f, encodedFar, 2.f);
        };
        inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

        const PassImage composite = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composite.pixels.empty());
        EXPECT_NEAR(composite.at(1, 1).x(), 0.75f, 0.002f);
        EXPECT_NEAR(composite.at(2, 2).x(), 2.25f, 0.002f);
    }

    /// Keep reconstruction texel centers aligned with the native integer target sizes.
    TEST(AtmosphereShaderTest, compositeUsesIntegerTargetDimensionsForOddScenes)
    {
        NativePassInputs inputs;
        inputs.width = 9;
        inputs.height = 7;
        inputs.lastShader = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        inputs.volumeHistoryWidth = inputs.volumeMetadataWidth = 4;
        inputs.volumeHistoryHeight = inputs.volumeMetadataHeight = 3;
        inputs.volumeHistoryInput = [](int x, int y) { return osg::Vec4f(float(x + 2 * y), 0.f, 0.f, 1.f); };
        inputs.volumeMetadataInput = [](int, int) {
            const float distance = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(distance, 1.f, distance, 2.f);
        };
        inputs.shaftInput = [](int, int) { return osg::Vec4f(); };
        const PassImage composite = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composite.pixels.empty());
        for (int y = 0; y < inputs.height; ++y)
            for (int x = 0; x < inputs.width; ++x)
            {
                const float sampleX = std::clamp((x + 0.5f) * 4.f / inputs.width - 0.5f, 0.f, 3.f);
                const float sampleY = std::clamp((y + 0.5f) * 3.f / inputs.height - 0.5f, 0.f, 2.f);
                EXPECT_NEAR(composite.at(x, y).x(), sampleX + 2.f * sampleY, 0.005f);
            }
    }

    TEST(AtmosphereShaderTest, compositeMatchesObliqueVolumeSamplesByCameraRayDistance)
    {
        constexpr int volumeWidth = 32;
        constexpr int volumeHeight = 32;
        constexpr int compositeWidth = 64;
        constexpr int compositeHeight = 64;
        constexpr float viewDistance = 5000.f;
        constexpr float verticalFovDegrees = 70.f;

        NativePassInputs inputs;
        inputs.width = compositeWidth;
        inputs.height = compositeHeight;
        inputs.lastShader = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        inputs.depth = [=](int, int) { return normalizedDepthForViewDistance(viewDistance, false); };
        inputs.volumeHistoryWidth = volumeWidth;
        inputs.volumeHistoryHeight = volumeHeight;
        inputs.volumeHistoryInput
            = [](int x, int) { return x % 2 == 0 ? osg::Vec4f(1.f, 0.f, 0.f, 1.f) : osg::Vec4f(0.f, 0.f, 1.f, 1.f); };
        inputs.volumeMetadataWidth = volumeWidth;
        inputs.volumeMetadataHeight = volumeHeight;
        inputs.volumeMetadataInput = [=](int x, int y) {
            const float terminationDistance = x % 2 == 0
                ? perspectiveRayDistance(x, y, volumeWidth, volumeHeight, verticalFovDegrees, viewDistance)
                : viewDistance;
            const float encodedDistance = encodeAtmosphereDistance(terminationDistance);
            return osg::Vec4f(encodedDistance, 1.f, encodedDistance, 1.f);
        };
        inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

        const PassImage composite = renderNativeAtmospherePass("composite", inputs);
        ASSERT_FALSE(composite.pixels.empty());
        const osg::Vec4f& obliquePixel = composite.at(compositeWidth - 2, compositeHeight / 2);
        EXPECT_GT(obliquePixel.x(), 0.8f);
        EXPECT_LT(obliquePixel.z(), 0.2f);
        for (const osg::Vec4f& pixel : composite.pixels)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_TRUE(std::isfinite(pixel[channel]));
    }

    TEST(AtmosphereShaderTest, compositePreservesThinSilhouettesWithoutReliableVolumeSamples)
    {
        constexpr int width = 64;
        constexpr int height = 64;
        constexpr int silhouetteX = width / 2;
        constexpr float viewDistance = 5000.f;
        constexpr float rejectedVolumeDistance = 20000.f;
        const osg::Vec4f skyColor(0.8f, 0.7f, 0.6f, 1.f);
        const osg::Vec4f silhouetteColor(0.18f, 0.2f, 0.24f, 1.f);

        for (const bool reverseZ : { false, true })
        {
            NativePassInputs inputs;
            inputs.width = width;
            inputs.height = height;
            inputs.reverseZ = reverseZ;
            inputs.lastShader = [=](int x, int) { return x == silhouetteX ? silhouetteColor : skyColor; };
            inputs.depth = [=](int x, int) {
                if (x == silhouetteX)
                    return normalizedDepthForViewDistance(viewDistance, reverseZ);
                return reverseZ ? 0.f : 1.f;
            };
            inputs.volumeHistoryWidth = 32;
            inputs.volumeHistoryHeight = 32;
            inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.0001f, 0.0001f, 0.0001f, 0.f); };
            inputs.volumeMetadataWidth = 32;
            inputs.volumeMetadataHeight = 32;
            inputs.volumeMetadataInput = [=](int, int) {
                const float encodedDistance = encodeAtmosphereDistance(rejectedVolumeDistance);
                return osg::Vec4f(encodedDistance, 1.f, encodedDistance, 1.f);
            };
            inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

            const PassImage composite = renderNativeAtmospherePass("composite", inputs);
            ASSERT_FALSE(composite.pixels.empty());
            double silhouetteTraceEnergy = 0.0;
            float minimumSilhouetteLuminance = std::numeric_limits<float>::max();
            for (int y = 0; y < height; ++y)
            {
                const osg::Vec4f& pixel = composite.at(silhouetteX, y);
                silhouetteTraceEnergy += std::abs(pixel.x() - silhouetteColor.x())
                    + std::abs(pixel.y() - silhouetteColor.y()) + std::abs(pixel.z() - silhouetteColor.z());
                minimumSilhouetteLuminance = std::min(
                    minimumSilhouetteLuminance, pixel.x() * 0.2126f + pixel.y() * 0.7152f + pixel.z() * 0.0722f);
            }
            EXPECT_GT(minimumSilhouetteLuminance, 0.18f) << "reverseZ=" << reverseZ;
            EXPECT_LT(silhouetteTraceEnergy / height, 0.003) << "reverseZ=" << reverseZ;
        }
    }

    /// Verify that foreground mist remains continuous across a subpixel geometry termination.
    TEST(AtmosphereShaderTest, compositeSharesForegroundMistAcrossSubpixelGeometry)
    {
        constexpr int width = 64;
        constexpr int height = 64;
        constexpr int silhouetteX = width / 2;
        constexpr float mistDistance = 1000.f;
        constexpr float silhouetteDistance = 5000.f;
        constexpr float backgroundDistance = 18000.f;
        const osg::Vec4f backgroundColor(0.7f, 0.68f, 0.64f, 1.f);
        const osg::Vec4f silhouetteColor(0.04f, 0.045f, 0.05f, 1.f);
        const osg::Vec4f foregroundMist(0.46f, 0.44f, 0.4f, 0.2f);

        for (const bool reverseZ : { false, true })
            for (const bool useUniformBuffer : { false, true })
            {
                NativePassInputs inputs;
                inputs.width = width;
                inputs.height = height;
                inputs.reverseZ = reverseZ;
                inputs.useUniformBuffer = useUniformBuffer;
                inputs.lastShader = [=](int x, int) { return x == silhouetteX ? silhouetteColor : backgroundColor; };
                inputs.depth = [=](int x, int) {
                    const float distance = x == silhouetteX ? silhouetteDistance : backgroundDistance;
                    return normalizedDepthForViewDistance(distance, reverseZ);
                };
                inputs.volumeHistoryWidth = width / 2;
                inputs.volumeHistoryHeight = height / 2;
                inputs.volumeHistoryInput = [=](int, int) { return foregroundMist; };
                inputs.volumeMetadataWidth = width / 2;
                inputs.volumeMetadataHeight = height / 2;
                inputs.volumeMetadataInput = [](int, int) {
                    return osg::Vec4f(
                        encodeAtmosphereDistance(mistDistance), 1.f, encodeAtmosphereDistance(backgroundDistance), 1.f);
                };
                inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

                const PassImage composite = renderNativeAtmospherePass("composite", inputs);
                ASSERT_FALSE(composite.pixels.empty());
                const osg::Vec4f expected = silhouetteColor * foregroundMist.w() + foregroundMist;
                for (int y = 0; y < height; ++y)
                {
                    const osg::Vec4f& pixel = composite.at(silhouetteX, y);
                    EXPECT_NEAR(pixel.x(), expected.x(), 0.002f) << "reverseZ=" << reverseZ;
                    EXPECT_NEAR(pixel.y(), expected.y(), 0.002f) << "reverseZ=" << reverseZ;
                    EXPECT_NEAR(pixel.z(), expected.z(), 0.002f) << "reverseZ=" << reverseZ;
                }
            }
    }

    TEST(AtmosphereShaderTest, compositeRejectsOpenSkyCloudVolumeFromDistantStaticSilhouettes)
    {
        constexpr int width = 64;
        constexpr int height = 64;
        constexpr int silhouetteX = width / 2;
        constexpr float distantStaticViewDistance = 19950.f;
        const osg::Vec4f skyColor(0.8f, 0.7f, 0.6f, 1.f);
        const osg::Vec4f silhouetteColor(0.22f, 0.24f, 0.28f, 1.f);

        for (const bool reverseZ : { false, true })
            for (const bool useUniformBuffer : { false, true })
            {
                NativePassInputs inputs;
                inputs.width = width;
                inputs.height = height;
                inputs.reverseZ = reverseZ;
                inputs.useUniformBuffer = useUniformBuffer;
                inputs.lastShader = [=](int x, int) { return x == silhouetteX ? silhouetteColor : skyColor; };
                inputs.depth = [=](int x, int) {
                    if (x == silhouetteX)
                        return normalizedDepthForViewDistance(distantStaticViewDistance, reverseZ);
                    return reverseZ ? 0.f : 1.f;
                };
                inputs.volumeHistoryWidth = width / 2;
                inputs.volumeHistoryHeight = height / 2;
                inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.0001f, 0.0001f, 0.0001f, 0.f); };
                inputs.volumeMetadataWidth = width / 2;
                inputs.volumeMetadataHeight = height / 2;
                inputs.volumeMetadataInput = [](int, int) {
                    const float encodedFar = encodeAtmosphereDistance(20000.f);
                    return osg::Vec4f(encodedFar, 1.f, encodedFar, 2.f);
                };
                inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

                const PassImage composite = renderNativeAtmospherePass("composite", inputs);
                ASSERT_FALSE(composite.pixels.empty());
                float maximumSilhouetteDeviation = 0.f;
                for (int y = 0; y < height; ++y)
                {
                    const osg::Vec4f& pixel = composite.at(silhouetteX, y);
                    for (int channel = 0; channel < 4; ++channel)
                        maximumSilhouetteDeviation
                            = std::max(maximumSilhouetteDeviation, std::abs(pixel[channel] - silhouetteColor[channel]));
                }
                EXPECT_LT(maximumSilhouetteDeviation, 0.003f)
                    << "reverseZ=" << reverseZ << ", useUniformBuffer=" << useUniformBuffer;
            }
    }

    TEST(AtmosphereShaderTest, compositePreservesFarSilhouettesAndWaterWithoutMatchingVolumeDepth)
    {
        constexpr int width = 64;
        constexpr int height = 64;
        constexpr float mountainViewDistance = 17000.f;
        constexpr float waterViewDistance = 12000.f;
        const osg::Vec4f skyColor(0.8f, 0.72f, 0.68f, 1.f);
        const osg::Vec4f mountainColor(0.18f, 0.2f, 0.24f, 1.f);
        const osg::Vec4f waterColor(0.16f, 0.28f, 0.38f, 1.f);

        for (const bool reverseZ : { false, true })
        {
            NativePassInputs inputs;
            inputs.width = width;
            inputs.height = height;
            inputs.reverseZ = reverseZ;
            inputs.lastShader = [=](int x, int y) {
                const bool mountainTip = y >= 31 && y <= 35 && std::abs(x - 32) <= 35 - y;
                if (mountainTip)
                    return mountainColor;
                if (y < 16)
                    return waterColor;
                return skyColor;
            };
            inputs.depth = [=](int x, int y) {
                const bool mountainTip = y >= 31 && y <= 35 && std::abs(x - 32) <= 35 - y;
                if (mountainTip)
                    return normalizedDepthForViewDistance(mountainViewDistance, reverseZ);
                if (y < 16)
                    return normalizedDepthForViewDistance(waterViewDistance, reverseZ);
                return reverseZ ? 0.f : 1.f;
            };
            inputs.volumeHistoryWidth = width / 2;
            inputs.volumeHistoryHeight = height / 2;
            inputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.0001f, 0.0001f, 0.0001f, 0.f); };
            inputs.volumeMetadataWidth = width / 2;
            inputs.volumeMetadataHeight = height / 2;
            inputs.volumeMetadataInput = [](int, int) {
                const float encodedFar = encodeAtmosphereDistance(20000.f);
                return osg::Vec4f(encodedFar, 1.f, encodedFar, 1.f);
            };
            inputs.shaftInput = [](int, int) { return osg::Vec4f(); };

            const PassImage composite = renderNativeAtmospherePass("composite", inputs);
            ASSERT_FALSE(composite.pixels.empty());
            double protectedTraceEnergy = 0.0;
            float minimumProtectedLuminance = std::numeric_limits<float>::max();
            std::size_t protectedPixelCount = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const bool mountainTip = y >= 31 && y <= 35 && std::abs(x - 32) <= 35 - y;
                    const bool water = y < 16;
                    if (!mountainTip && !water)
                        continue;
                    const osg::Vec4f expected = mountainTip ? mountainColor : waterColor;
                    const osg::Vec4f& pixel = composite.at(x, y);
                    protectedTraceEnergy += std::abs(pixel.x() - expected.x()) + std::abs(pixel.y() - expected.y())
                        + std::abs(pixel.z() - expected.z());
                    minimumProtectedLuminance = std::min(
                        minimumProtectedLuminance, pixel.x() * 0.2126f + pixel.y() * 0.7152f + pixel.z() * 0.0722f);
                    ++protectedPixelCount;
                }

            ASSERT_GT(protectedPixelCount, 0u);
            EXPECT_GT(minimumProtectedLuminance, 0.18f) << "reverseZ=" << reverseZ;
            EXPECT_LT(protectedTraceEnergy / protectedPixelCount, 0.003) << "reverseZ=" << reverseZ;
        }
    }

    TEST(AtmosphereShaderTest, referenceSunsetSeparatesSolarAndAntiSolarColors)
    {
        const osg::Vec3f sunDirection = osg::Vec3f(1.f, 0.f, 0.08f) / osg::Vec3f(1.f, 0.f, 0.08f).length();
        const Rgb skyColor{ 0.232298f, 0.361908f, 0.519572f };
        const Rgb fogColor{ 0.887052f, 0.638956f, 0.517662f };

        const Rgb solar = referenceSkyScatter({ 1.f, 0.f, 0.f }, sunDirection, skyColor, fogColor);
        const Rgb antiSolar = referenceSkyScatter({ -1.f, 0.f, 0.f }, sunDirection, skyColor, fogColor);

        EXPECT_GT(solar.r, antiSolar.r);
        EXPECT_GT(solar.r / solar.b, antiSolar.r / antiSolar.b);
        EXPECT_GT(antiSolar.b, antiSolar.r);
    }

    TEST(AtmosphereShaderTest, referenceNightSunSetsBelowTheHorizonAndDarkensScattering)
    {
        const Rgb skyColor{ 0.035294f, 0.039216f, 0.043137f };
        const Rgb fogColor{ 0.016429f, 0.020358f, 0.024350f };
        const osg::Vec3f physicalNightSun = osg::Vec3f(0.6f, 0.f, -0.8f) / osg::Vec3f(0.6f, 0.f, -0.8f).length();
        const Rgb corrected = referenceSkyScatter({ 1.f, 0.f, 0.f }, physicalNightSun, skyColor, fogColor, 0.44f);

        EXPECT_TRUE(std::isfinite(corrected.r));
        EXPECT_TRUE(std::isfinite(corrected.g));
        EXPECT_TRUE(std::isfinite(corrected.b));
        EXPECT_LT(corrected.r, 0.001f);
        EXPECT_LT(corrected.g, 0.001f);
        EXPECT_LT(corrected.b, 0.001f);
    }
}
