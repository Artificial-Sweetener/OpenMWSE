#include <components/fx/pass.hpp>
#include <components/fx/stateupdater.hpp>
#include <components/fx/technique.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>

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
#include <functional>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

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
        std::function<osg::Vec4f(int, int)> shaftInput;
        int rawVolumeWidth = 0;
        int rawVolumeHeight = 0;
        int volumeHistoryWidth = 0;
        int volumeHistoryHeight = 0;
        int volumeMetadataWidth = 0;
        int volumeMetadataHeight = 0;
        int shaftWidth = 0;
        int shaftHeight = 0;
        osg::Vec4f cloudParameters;
        osg::Vec4f cloudShapeParameters{ 0.62f, 0.9f, 0.72f, 0.08f };
        osg::Vec4f cloudOrganizationParameters{ 0.5f, 0.3f, 0.6f, 0.1f };
        osg::Vec4f cloudStructureParameters{ 0.7f, 0.5f, 0.f, 0.f };
        osg::Vec4f lowCloudParameters;
        osg::Vec4f lowCloudShapeParameters{ 0.15f, 1.f, 0.2f, 1.f };
        osg::Vec4f mistParameters{ 1.f, 800.f, 4500.f, 1.f };
        osg::Vec4f fogVolumeParameters;
        osg::Vec4f mediumReferenceParameters;
        osg::Vec4f orographicParameters;
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
        bool reverseZ = false;
        bool useUniformBuffer = false;
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
        bool renderCloudRayOffsets = false;
        bool resolveReprojectedVolume = false;
    };

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
    NativePassInputs makeProductionAtmosphereInputs(
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
        result.cloudShapeParameters = { volume["cloudType"].as<float>(), volume["cloudFeatureScale"].as<float>(),
            volume["cloudVerticalDevelopment"].as<float>(), volume["cloudAnvilStrength"].as<float>() };
        result.cloudOrganizationParameters
            = { volume["cloudDeckContinuity"].as<float>(), volume["cloudConvectiveFrequency"].as<float>(),
                  volume["cloudConvectiveMaturity"].as<float>(), volume["cloudStormOrganization"].as<float>() };
        result.cloudStructureParameters = { volume["cloudTowerTaper"].as<float>(),
            volume["cloudBaseVariation"].as<float>(), volume["cloudDeckType"].as<float>(), 0.f };
        result.lowCloudParameters = { volume["lowCloudCoverage"].as<float>(), volume["lowCloudDensity"].as<float>(),
            volume["lowCloudAltitude"].as<float>(), volume["lowCloudThickness"].as<float>() };
        result.lowCloudShapeParameters = { volume["lowCloudType"].as<float>(),
            volume["lowCloudFeatureScale"].as<float>(), volume["lowCloudVerticalDevelopment"].as<float>(),
            volume["lowCloudEnvironmentInfluence"].as<float>() };
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
            inputs.renderCloudDensitySlice && inputs.cloudDensitySliceQuantity == 2 ? GL_RGBA32F : GL_RGBA16F);
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
                source += inputs.renderCloudRayOffsets ? R"glsl(
void main()
{
    float temporal = atmosphereCloudRayOffset(gl_FragCoord.xy, true);
    float stable = atmosphereCloudRayOffset(gl_FragCoord.xy, false);
    omw_FragColor = vec4(temporal, stable, 0.0, 1.0);
    omw_FragColor1 = vec4(0.0);
}
)glsl"
                                                       : R"glsl(
uniform vec2 openmwseTestRayGridDimensions;
void main()
{
    vec4 metadata;
    omw_FragColor = integrateAtmosphereAtUv(
        gl_FragCoord.xy / openmwseTestRayGridDimensions, gl_FragCoord.xy, atmosphereTemporalSamplingEnabled(), metadata);
    omw_FragColor1 = metadata;
}
)glsl";
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
                source += R"glsl(
uniform vec2 openmwseTestCloudDensitySliceExtent;
uniform int openmwseTestCloudDensitySliceAxis;
uniform int openmwseTestCloudDensitySliceQuantity;

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
        OpenMWSECloudIllumination illumination = openmwseCloudIllumination(
            omw.atmosphereSolarRadiance, omw.atmosphereSkyIrradiance, aerialPerspectiveParameters());
        omw_FragColor = vec4(openmwseTestCloudDensitySliceAxis == 0
            ? illumination.solarIrradiance : illumination.isotropicSkyRadiance, 1.0);
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
        float shellRadius = openmwseCloudVirtualPlanetRadius + omw.eyePos.z - openmwseCloudWaterHeight();
        vec2 horizontalOffset = horizontalPosition - planetCenter.xy;
        float physicalHeight = planetCenter.z
            + sqrt(max(shellRadius * shellRadius - dot(horizontalOffset, horizontalOffset), 0.0));
        samplePosition = vec3(horizontalPosition, physicalHeight);
    }
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    if (openmwseTestCloudDensitySliceQuantity == 7)
    {
        vec2 bounds = openmwseMainCloudDensityHeightBounds(mainLayer);
        omw_FragColor = vec4(bounds / 100000.0, 0.0, 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 6)
    {
        omw_FragColor = vec4(openmwseCloudCurlNoiseSample(openmwseTestCloudDensitySliceExtent), 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 5)
    {
        omw_FragColor = vec4(openmwseCloudBaseNoiseCoordinates(
            vec3(0.0), mainLayer, openmwseTestCloudDensitySliceExtent.x), 1.0);
        return;
    }
    if (openmwseTestCloudDensitySliceQuantity == 4)
    {
        vec3 position = vec3(0.0, 0.0, openmwseCloudWaterHeight() + mainLayer.parameters.z
            + openmwseTestCloudDensitySliceExtent.x * mainLayer.parameters.w
                * openmwseCloudVerticalExtentScale(mainLayer));
        OpenMWSECloudColumnProperties column = openmwseCloudLayerColumnProperties(
            position, openmwseCloudLayerPosition(position, mainLayer), mainLayer);
        column.localCoverage = 1.0;
        column.family = mainLayer.shape.x;
        omw_FragColor = vec4(openmwseCloudDensityFromNoise(position, 1.0, column, mainLayer));
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
        vec2 sunTransmittance = openmwseCloudSunTransmittance(samplePosition, normalize(omw.sunPos.xyz),
            omw.atmosphereQuality, mainLayer, lowLayer);
        diagnosticValue = vec3(sunTransmittance, 0.0);
    }
    omw_FragColor = vec4(diagnosticValue, density);
}
)glsl";
                shader->setShaderSource(source);
            }
            state->addUniform(new osg::Uniform("openmwseTestCloudDensitySliceExtent", inputs.cloudDensitySliceExtent));
            state->addUniform(new osg::Uniform("openmwseTestCloudDensitySliceAxis", inputs.cloudDensitySliceAxis));
            state->addUniform(
                new osg::Uniform("openmwseTestCloudDensitySliceQuantity", inputs.cloudDensitySliceQuantity));
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
            int inputWidth = width;
            int inputHeight = height;
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
        updater.setAtmosphere(inputs.inScattering, inputs.outScattering, inputs.cloudParameters,
            inputs.cloudShapeParameters, inputs.cloudOrganizationParameters, inputs.cloudStructureParameters,
            inputs.lowCloudParameters, inputs.lowCloudShapeParameters, inputs.mistParameters,
            inputs.fogVolumeParameters, inputs.mediumReferenceParameters, inputs.orographicParameters,
            inputs.sunDiscColor, inputs.solarRadiance, inputs.skyIrradiance, inputs.groundIrradiance,
            inputs.cloudAdvectionOffsetWorldUnits, inputs.cloudAdvectionDeltaWorldUnits, inputs.historyLightingWeight,
            inputs.weatherWeight, inputs.quality, inputs.volumeResolutionScale, inputs.atmosphereEnabled);
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        scene->addChild(geode);
        osg::StateSet* updaterState = inputs.bindUniformBufferOnAncestor ? scene->getOrCreateStateSet() : state;
        updater.setDefaults(updaterState);
        updater.apply(updaterState, nullptr);

        viewer.setSceneData(scene);
        viewer.realize();
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
        const int shaftWidth = std::max(1, static_cast<int>(sceneWidth * volumeScale * 0.5f));
        const int shaftHeight = std::max(1, static_cast<int>(sceneHeight * volumeScale * 0.5f));

        NativePassInputs volumeInputs = requestedInputs;
        volumeInputs.width = shaftWidth;
        volumeInputs.height = shaftHeight;
        volumeInputs.sceneWidth = sceneWidth;
        volumeInputs.sceneHeight = sceneHeight;
        volumeInputs.volumeResolutionScale = volumeScale;
        ProductionAtmosphereFrame frame;
        volumeInputs.historyValid = requestedInputs.historyValid && previousFrame != nullptr;
        frame.mRawVolume = renderNativeAtmospherePass("integrateAtmosphereVolume", volumeInputs, &frame.mRawMetadata);

        NativePassInputs resolveInputs = volumeInputs;
        resolveInputs.width = volumeWidth;
        resolveInputs.height = volumeHeight;
        resolveInputs.rawVolumeWidth = shaftWidth;
        resolveInputs.rawVolumeHeight = shaftHeight;
        resolveInputs.historyValid = requestedInputs.historyValid && previousFrame != nullptr;
        resolveInputs.passInput = [&frame](int x, int y) { return frame.mRawVolume.at(x, y); };
        resolveInputs.volumeMetadataInput = [&frame](int x, int y) { return frame.mRawMetadata.at(x, y); };
        resolveInputs.volumeMetadataWidth = shaftWidth;
        resolveInputs.volumeMetadataHeight = shaftHeight;
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

    /// Execute the complete native atmosphere technique at its production render-target resolutions.
    ProductionAtmosphereFrame renderProductionAtmosphereFrame(
        const NativePassInputs& requestedInputs, const ProductionAtmosphereFrame* previousFrame = nullptr)
    {
        const int sceneWidth = requestedInputs.width;
        const int sceneHeight = requestedInputs.height;
        const float volumeScale = MWRender::getAtmosphereVolumeResolutionScale(requestedInputs.quality);
        const int volumeWidth = std::max(1, static_cast<int>(sceneWidth * volumeScale));
        const int volumeHeight = std::max(1, static_cast<int>(sceneHeight * volumeScale));
        const int shaftWidth = std::max(1, static_cast<int>(sceneWidth * volumeScale * 0.5f));
        const int shaftHeight = std::max(1, static_cast<int>(sceneHeight * volumeScale * 0.5f));
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
        frame.mShafts = renderNativeAtmospherePass("integrateShafts", shaftInputs);

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
        defines["groundcoverFadeStart"] = "0.0";
        defines["groundcoverFadeEnd"] = "1.0";
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
            sky->getShaderSource().find("atmosphereCloudScattering(worldDirection, skyRadiance)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseApplyAerialPerspectiveTowardTarget"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseIntegrateCloudsStable"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseApplyAerialPerspective"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseIncidentSolarRadiance"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("const int maximumSampleCount = 192"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("int maximumAllocatedSamples = 48 + quality * 16"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("if (cameraAboveEnabledLayer)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("if (transmittance < 0.015)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("atmosphereSunDisc"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("haloAngularDistance"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("float halo ="), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("openmwseTriangularDither(gl_FragCoord.xy) * (0.5 / 255.0)"),
            std::string::npos);
        const std::size_t legacyCloudPass = sky->getShaderSource().find("void paintClouds");
        const std::size_t moonPass = sky->getShaderSource().find("void paintMoon");
        ASSERT_NE(legacyCloudPass, std::string::npos);
        ASSERT_NE(moonPass, std::string::npos);
        EXPECT_NE(
            sky->getShaderSource().substr(legacyCloudPass, moonPass - legacyCloudPass).find("if (atmosphere.enabled)"),
            std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("color.a = 1.0;"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("normalize(atmosphere.sunDirection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("dot(worldDirection, sunDirection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("vec3(0.371, 0.637, 1.108), 0.44"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("vec3(0.371, 0.637, 1.108), 0.62"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("atmosphereScatter"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("openmwseAtmosphereScatteredLight"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("sampleSkyColor"), std::string::npos);
        EXPECT_EQ(terrain->getShaderSource().find("!atmosphere.enabled"), std::string::npos);
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
        EXPECT_NE(objects->getShaderSource().find("if (atmosphere.enabled && !cameraBelowWater && fog.depth < 0.0)"),
            std::string::npos);
        EXPECT_EQ(objects->getShaderSource().find("!cameraBelowWater && !useWaterDepthFog"), std::string::npos);
        EXPECT_EQ(objects->getShaderSource().find("WATER_COLOR * length(sun.ambient.xyz)"), std::string::npos);
        EXPECT_EQ(water->getShaderSource().find("skyColorEstimate"), std::string::npos);
    }

    TEST(AtmosphereShaderTest, waterInScatteringPreservesAmbientSpectrumWithoutNormAmplification)
    {
        const osg::Vec3f waterFilter(0.090195f, 0.115685f, 0.12745f);
        const osg::Vec3f cloudyAmbient(0.72f, 0.68f, 0.61f);
        const osg::Vec3f filteredAmbient(waterFilter.x() * cloudyAmbient.x(),
            waterFilter.y() * cloudyAmbient.y(), waterFilter.z() * cloudyAmbient.z());
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

    TEST(AtmosphereShaderTest, compilesAndLinksSkyProgramInGraphicsContext)
    {
        Shader::ShaderManager manager;
        manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
        auto defines = getAtmosphereTestDefines();
        manager.setGlobalDefines(defines);

        const osg::ref_ptr<osg::Program> program = manager.getProgram("compatibility/sky");
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
        ASSERT_EQ(technique.getPasses().size(), 5);
        EXPECT_EQ(technique.getPasses().front()->getSecondaryTarget(), "AtmosphereVolumeMetadataRaw");
        EXPECT_TRUE(technique.getLights());
        EXPECT_NE(technique.getFlags() & Fx::Technique::Flag_Disable_Interiors, 0u);
        EXPECT_NE(technique.getFlags() & Fx::Technique::Flag_Disable_Underwater, 0u);
        EXPECT_NE(technique.getFlags() & Fx::Technique::Flag_Disable_SunGlare, 0u);

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

    /// Verify that the virtual main-cloud shell reaches a ground observer's geometric horizon.
    TEST(AtmosphereShaderTest, sphericalMainCloudShellReachesGeometricHorizon)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 48;
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.cloudParameters = { 1.f, 1.3f, 48000.f, 32000.f };
        inputs.cloudShapeParameters = { 0.5f, 1.f, 0.8f, 0.2f };
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        inputs.farPlane = 20000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;
        const PassImage horizon = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(horizon.pixels.empty());

        double lostTransmittance = 0.0;
        for (const osg::Vec4f& pixel : horizon.pixels)
            lostTransmittance += 1.0 - pixel.w();
        EXPECT_GT(lostTransmittance / static_cast<double>(horizon.pixels.size()), 0.001)
            << "A spherical cloud shell must remain visible beyond a short terrain far plane at the horizon";
    }

    /// Retain a distant shell entry while bounding the work inside it independently of the empty approach.
    TEST(AtmosphereShaderTest, sphericalCloudMarchBudgetStartsAtShellEntry)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.sunDirection = { 1.f, 0.f, 0.f, 0.f };
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudShapeParameters.z() = 0.f;
        inputs.cloudDensitySliceExtent = { 1.f, 1.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 2;
        inputs.renderCloudDensitySlice = true;
        const PassImage geometry = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(geometry.pixels.size(), 1u);
        const osg::Vec4f interval = geometry.pixels.front();
        constexpr double unitsPerMeter = 69.99125109f;
        const double radius = static_cast<double>(6360000.f * 69.99125109f);
        const double entry = std::sqrt((16000.0 - 1200.0) * (2.0 * radius + 16000.0 + 1200.0));
        ASSERT_GT(entry, 30000.0 * unitsPerMeter);
        ASSERT_LT(entry, 70000.0 * unitsPerMeter);
        EXPECT_NEAR(interval.y(), entry, 0.5);
        EXPECT_NEAR(interval.z() - interval.y(), 30000.0 * unitsPerMeter, 0.5);

        inputs.cloudParameters.z() = 48000.f;
        const PassImage outside = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(outside.pixels.size(), 1u);
        EXPECT_GT(outside.pixels.front().y(), outside.pixels.front().z())
            << "Shells whose entry exceeds the render distance must remain rejected";
    }

    /// Render horizon cloud density after an empty approach longer than the bounded in-cloud march.
    TEST(AtmosphereShaderTest, horizonCloudTransportRetainsDistantShellSupport)
    {
        NativePassInputs inputs = makeProductionAtmosphereInputs("Overcast", "Day");
        inputs.width = 32;
        inputs.height = 16;
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.viewDirection = { 1.f, 0.f, 0.f };
        inputs.verticalFov = 0.1f;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudShapeParameters = { 0.3f, 1.f, 0.f, 0.f };
        inputs.cloudOrganizationParameters = { 1.f, 0.f, 0.f, 0.f };
        inputs.cloudStructureParameters.z() = 0.3f;
        inputs.lowCloudParameters = {};
        inputs.mistParameters = {};
        inputs.fogVolumeParameters = {};
        PassImage metadata;
        const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs, &metadata);
        ASSERT_EQ(volume.pixels.size(), 512u);
        ASSERT_EQ(metadata.pixels.size(), volume.pixels.size());
        std::size_t occupied = 0;
        for (std::size_t index = 0; index < volume.pixels.size(); ++index)
        {
            if (volume.pixels[index].w() >= 0.9f)
                continue;
            ++occupied;
            EXPECT_GT(metadata.pixels[index].x(), encodeAtmosphereDistance(30000.f * 69.99125109f));
        }
        EXPECT_GT(occupied, volume.pixels.size() / 4)
            << "The geometric horizon must retain actual transported cloud support, not only an intersection";
        RecordProperty("distant_occupied_pixels", static_cast<int>(occupied));

        inputs.depth = [](int, int) { return 0.5f; };
        const PassImage occluded = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(occluded.pixels.size(), volume.pixels.size());
        for (const osg::Vec4f& pixel : occluded.pixels)
            EXPECT_EQ(pixel, osg::Vec4f(0.f, 0.f, 0.f, 1.f))
                << "Distant clouds must not survive an opaque scene termination before shell entry";
    }

    /// Allocate the main march to all selectable type heights while retaining the separate nominal shell.
    TEST(AtmosphereShaderTest, cloudDensityBoundsEncloseBothWeatherTypeEndpoints)
    {
        NativePassInputs inputs;
        inputs.width = 1;
        inputs.height = 1;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudShapeParameters.z() = 0.f;
        inputs.cloudDensitySliceQuantity = 7;
        inputs.renderCloudDensitySlice = true;
        for (const auto& [type, top] : std::array{
                 std::pair{ 0.f, 0.2f }, std::pair{ 0.5f, 0.4f }, std::pair{ 0.75f, 0.675f }, std::pair{ 1.f, 0.95f } })
        {
            for (const bool deckIsTaller : { false, true })
            {
                inputs.cloudShapeParameters.x() = deckIsTaller ? 0.f : type;
                inputs.cloudStructureParameters.z() = deckIsTaller ? type : 0.f;
                const PassImage bounds = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_EQ(bounds.pixels.size(), 1u);
                EXPECT_NEAR(bounds.pixels.front().x(), 0.17f, 0.001f);
                EXPECT_NEAR(bounds.pixels.front().y(), 0.16f + top, 0.001f);
            }
        }
    }

    /// Preserve local altitude and shell intersections when Earth-scale geometry is evaluated in game units.
    TEST(AtmosphereShaderTest, sphericalCloudGeometryRetainsLocalPrecision)
    {
        NativePassInputs inputs;
        inputs.width = 16;
        inputs.height = 16;
        inputs.cloudParameters = { 1.f, 1.f, 16000.f, 100000.f };
        inputs.cloudShapeParameters.z() = 0.f;
        inputs.cloudDensitySliceExtent = { 16.f, 16.f };
        inputs.cloudDensitySliceAxis = 1;
        inputs.cloudDensitySliceQuantity = 2;
        inputs.renderCloudDensitySlice = true;
        // Match the shader's representable radius; use double precision only for the independent reference.
        const double planetRadius = static_cast<double>(6360000.f * 69.99125109f);
        const double maximumMarchingDistance = static_cast<double>(30000.f * 69.99125109f);
        const double maximumEntryDistance = static_cast<double>(70000.f * 69.99125109f);
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
                        if (entry > maximumEntryDistance)
                        {
                            EXPECT_GT(pixel.y(), pixel.z());
                            continue;
                        }
                        EXPECT_NEAR(pixel.y(), entry, 0.5);
                        EXPECT_NEAR(pixel.z(), std::min(exit, entry + maximumMarchingDistance), 0.5);
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
        inputs.cloudShapeParameters.z() = 0.f;
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

    /// Keep sunset in-scattering at zero until an opaque surface's view path reaches the fog volume.
    TEST(AtmosphereShaderTest, sunsetInScatteringDoesNotOverlayNearOpaqueGeometry)
    {
        NativePassInputs inputs = makeProductionAtmosphereInputs("Clear", "Sunset");
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
        lowCloudInputs.lowCloudShapeParameters = { 0.18f, 0.9f, 0.24f, 1.f };
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
        inputs.cloudShapeParameters = { 0.58f, 1.f, 0.68f, 0.1f };
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

    TEST(AtmosphereShaderTest, lowCloudCondensateFormsWindwardAndDissipatesLeeward)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 96;
        inputs.lowCloudParameters = { 0.88f, 0.9f, 2600.f, 9000.f };
        inputs.lowCloudShapeParameters = { 0.18f, 0.9f, 0.24f, 1.f };
        inputs.mistParameters = {};
        inputs.mediumReferenceParameters = { 0.f, 1.f, 2600.f, 0.f };
        inputs.viewDirection = { 0.f, 1.f, 0.16f };
        inputs.farPlane = 180000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;

        inputs.orographicParameters = { 0.f, 0.f, -0.2f, 0.f };
        const PassImage windward = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        inputs.orographicParameters = { 0.f, 0.f, 0.2f, 0.f };
        const PassImage leeward = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(windward.pixels.size(), leeward.pixels.size());

        const auto meanLostTransmittance = [](const PassImage& image) {
            double result = 0.0;
            for (const osg::Vec4f& pixel : image.pixels)
                result += 1.0 - pixel.w();
            return result / static_cast<double>(image.pixels.size());
        };
        EXPECT_GT(meanLostTransmittance(windward), meanLostTransmittance(leeward) * 1.15)
            << "The same low-cloud field must condense on rising terrain and dissipate on descending terrain";
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
        lowCloudInputs.lowCloudShapeParameters = { 0.2f, 0.9f, 0.28f, 1.f };
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
        lowCloudInputs.lowCloudShapeParameters = { 0.2f, 0.9f, 0.28f, 1.f };
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
        inputs.cloudShapeParameters = { 0.829f, 0.897f, 0.661f, 0.886f };
        inputs.cloudOrganizationParameters = { 0.53f, 0.166f, 0.51f, 0.04f };
        inputs.cloudStructureParameters = { 0.687f, 0.355f, 0.059f, 0.f };
        inputs.lowCloudParameters = { 0.149f, 0.542f, 4727.f, 6788.f };
        inputs.lowCloudShapeParameters = { 0.261f, 0.924f, 0.215f, 1.f };
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
        EXPECT_LT(maximumRowSpatialResidual, 0.17)
            << "A narrow low-cloud horizon band must not contain spatially incoherent ray estimates";
    }

    /// Verify that cloud-family data produces materially different dimensional cloud fields.
    TEST(AtmosphereShaderTest, typedCloudFamiliesProduceDistinctDensityFields)
    {
        NativePassInputs inputs;
        inputs.width = 96;
        inputs.height = 96;
        inputs.cloudParameters = { 0.62f, 1.05f, 22000.f, 18000.f };
        inputs.mistParameters = {};
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.viewDirection = { 1.f, 0.f, 0.28f };
        inputs.farPlane = 300000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;

        const osg::Vec4f stratusShape{ 0.08f, 1.35f, 0.14f, 0.f };
        const osg::Vec4f stratusOrganization{ 0.94f, 0.02f, 0.08f, 0.f };
        const osg::Vec4f stratusStructure{ 0.72f, 0.16f, 0.f, 0.f };
        inputs.cloudShapeParameters = stratusShape;
        inputs.cloudOrganizationParameters = stratusOrganization;
        inputs.cloudStructureParameters = stratusStructure;
        const PassImage stratus = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        const osg::Vec4f cumulusShape{ 0.64f, 0.9f, 0.74f, 0.f };
        const osg::Vec4f cumulusOrganization{ 0.46f, 0.5f, 0.72f, 0.12f };
        const osg::Vec4f cumulusStructure{ 0.68f, 0.46f, 0.f, 0.f };
        inputs.cloudShapeParameters = cumulusShape;
        inputs.cloudOrganizationParameters = cumulusOrganization;
        inputs.cloudStructureParameters = cumulusStructure;
        const PassImage cumulus = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        const osg::Vec4f stormShape{ 1.f, 1.05f, 1.f, 0.9f };
        const osg::Vec4f stormOrganization{ 0.9f, 0.72f, 1.f, 1.f };
        const osg::Vec4f stormStructure{ 0.72f, 0.24f, 0.f, 0.f };
        inputs.cloudShapeParameters = stormShape;
        inputs.cloudOrganizationParameters = stormOrganization;
        inputs.cloudStructureParameters = stormStructure;
        const PassImage storm = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(stratus.pixels.size(), cumulus.pixels.size());
        ASSERT_EQ(stratus.pixels.size(), storm.pixels.size());

        const auto meanOpacityDifference = [](const PassImage& first, const PassImage& second) {
            double difference = 0.0;
            for (std::size_t index = 0; index < first.pixels.size(); ++index)
                difference += std::abs(first.pixels[index].w() - second.pixels[index].w());
            return difference / static_cast<double>(first.pixels.size());
        };
        const auto topDownOpticalDepth
            = [&](const osg::Vec4f& shape, const osg::Vec4f& organization, const osg::Vec4f& structure) {
                  NativePassInputs depthInputs = inputs;
                  depthInputs.cloudParameters.y() = 0.08f;
                  depthInputs.cloudShapeParameters = shape;
                  depthInputs.cloudOrganizationParameters = organization;
                  depthInputs.cloudStructureParameters = structure;
                  depthInputs.cloudAdvectionOffsetWorldUnits = {};
                  depthInputs.eyePosition = { 0.f, 0.f, 500000.f };
                  depthInputs.viewDirection = { 0.04f, 0.01f, -1.f };
                  depthInputs.verticalFov = 70.f;
                  depthInputs.farPlane = 800000.f;
                  const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", depthInputs);
                  std::vector<double> opticalDepths;
                  opticalDepths.reserve(image.pixels.size());
                  for (const osg::Vec4f& pixel : image.pixels)
                      opticalDepths.push_back(-std::log(std::max(static_cast<double>(pixel.w()), 1e-6)));
                  std::ranges::sort(opticalDepths);
                  const double meanOpticalDepth = std::reduce(opticalDepths.begin(), opticalDepths.end())
                      / static_cast<double>(opticalDepths.size());
                  const double developedColumnOpticalDepth = opticalDepths[opticalDepths.size() * 49 / 50];
                  return std::array{ meanOpticalDepth, developedColumnOpticalDepth };
              };
        const auto upperVolumeSupport
            = [&](const osg::Vec4f& shape, const osg::Vec4f& organization, const osg::Vec4f& structure) {
                  NativePassInputs upperInputs = inputs;
                  upperInputs.cloudParameters.y() = 0.2f;
                  upperInputs.cloudShapeParameters = shape;
                  upperInputs.cloudOrganizationParameters = organization;
                  upperInputs.cloudStructureParameters = structure;
                  upperInputs.width = 192;
                  upperInputs.height = 1;
                  upperInputs.eyePosition = { -300000.f, 0.f, 45000.f };
                  upperInputs.viewDirection = { 1.f, 0.f, 0.f };
                  upperInputs.verticalFov = 15.f;
                  upperInputs.farPlane = 600000.f;
                  const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", upperInputs);
                  std::vector<double> opticalDepths;
                  opticalDepths.reserve(image.pixels.size());
                  for (const osg::Vec4f& pixel : image.pixels)
                      opticalDepths.push_back(-std::log(std::max(static_cast<double>(pixel.w()), 1e-6)));
                  std::ranges::sort(opticalDepths);
                  return opticalDepths[opticalDepths.size() * 19 / 20];
              };
        const auto opacityPopulation = [](const PassImage& image) {
            std::array<std::size_t, 3> population{};
            for (const osg::Vec4f& pixel : image.pixels)
            {
                const float opacity = 1.f - pixel.w();
                if (opacity > 0.05f)
                    ++population[0];
                if (opacity > 0.985f)
                    ++population[1];
                if (opacity > 0.1f && opacity < 0.9f)
                    ++population[2];
            }
            return population;
        };
        const std::array<std::size_t, 3> stormOpacity = opacityPopulation(storm);
        ASSERT_GT(stormOpacity[0], 0u);
        EXPECT_GT(meanOpacityDifference(stratus, cumulus), 0.004)
            << "Stratiform and cumulus profiles must not collapse to one density silhouette";
        EXPECT_GT(meanOpacityDifference(cumulus, storm), 0.004)
            << "Cumulus and developed storm profiles must not collapse to one density silhouette";
        EXPECT_GT(meanOpacityDifference(stratus, storm), 0.008)
            << "Stratus and storm fields must remain materially distinct after integration";
        const std::array<double, 2> stratusOpticalDepth
            = topDownOpticalDepth(stratusShape, stratusOrganization, stratusStructure);
        const std::array<double, 2> cumulusOpticalDepth
            = topDownOpticalDepth(cumulusShape, cumulusOrganization, cumulusStructure);
        const std::array<double, 2> stormOpticalDepth
            = topDownOpticalDepth(stormShape, stormOrganization, stormStructure);
        EXPECT_GT(stormOpticalDepth[0], stratusOpticalDepth[0] * 1.1)
            << "Developed storms must integrate more optical depth than stratiform cover";
        EXPECT_GT(stormOpticalDepth[0], cumulusOpticalDepth[0] * 1.1)
            << "Developed storms must integrate more optical depth than ordinary cumulus";
        const double stratusUpperSupport = upperVolumeSupport(stratusShape, stratusOrganization, stratusStructure);
        const double cumulusUpperSupport = upperVolumeSupport(cumulusShape, cumulusOrganization, cumulusStructure);
        const double stormUpperSupport = upperVolumeSupport(stormShape, stormOrganization, stormStructure);
        EXPECT_LT(stratusUpperSupport, 0.001) << "Stratiform cover must not occupy the developed upper-cloud volume";
        EXPECT_GE(stormUpperSupport, cumulusUpperSupport)
            << "Developed storms must not retain less upper-volume support than tapering cumulus";
        EXPECT_LT(static_cast<double>(stormOpacity[1]) / static_cast<double>(stormOpacity[0]), 0.7)
            << "The weather support field must not integrate into a near-binary opaque storm mask";
        EXPECT_GT(static_cast<double>(stormOpacity[2]) / static_cast<double>(stormOpacity[0]), 0.08)
            << "The reconstructed storm boundary must retain a meaningful partial-density population";
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
                for (float development : { 0.35f, 1.f })
                {
                    NativePassInputs inputs;
                    inputs.width = 4;
                    inputs.height = 4;
                    inputs.renderCloudDensitySlice = true;
                    inputs.cloudDensitySliceQuantity = 4;
                    inputs.cloudDensitySliceExtent.x() = height;
                    inputs.cloudParameters = { 1.f, 1.f, 48000.f, 200000.f };
                    inputs.cloudShapeParameters = { type, 1.f, development, 1.f };
                    inputs.lowCloudParameters = {};
                    inputs.mistParameters = {};
                    const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                    ASSERT_FALSE(image.pixels.empty());
                    EXPECT_NEAR(image.at(0, 0).x(), profile * std::min(top + bottom, 1.0), 0.002)
                        << "type=" << type << " height=" << height << " development=" << development;
                }
            }
        }
    }

    /// Verify that convective support narrows with altitude instead of extruding one weather-map footprint vertically.
    TEST(AtmosphereShaderTest, convectiveCloudSupportTapersInsteadOfExtrudingWeatherFootprints)
    {
        const auto densitySupport = [](float eyeHeight) {
            NativePassInputs inputs;
            inputs.width = 96;
            inputs.height = 96;
            inputs.eyePosition = { 65000.f, 355000.f, eyeHeight };
            inputs.cloudDensitySliceExtent = { 500000.f, 500000.f };
            inputs.renderCloudDensitySlice = true;
            inputs.mistParameters = {};
            inputs.cloudParameters = { 0.12f, 1.f, 32000.f, 90000.f };
            inputs.cloudShapeParameters = { 0.68f, 1.2f, 1.f, 0.f };
            inputs.cloudOrganizationParameters = { 0.15f, 0.55f, 1.f, 0.f };
            inputs.cloudStructureParameters = { 0.82f, 0.22f, 0.f, 0.f };
            const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            std::vector<bool> support(image.pixels.size());
            for (std::size_t index = 0; index < image.pixels.size(); ++index)
                support[index] = image.pixels[index].x() > 0.01f;
            return support;
        };

        const std::vector<bool> lowerSupport = densitySupport(52000.f);
        const std::vector<bool> upperSupport = densitySupport(72000.f);
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
        EXPECT_GT(lowerCount, upperCount) << "Convective cloud bodies must taper before reaching their developed tops";
        EXPECT_LT(static_cast<double>(intersectionCount) / static_cast<double>(unionCount), 0.82)
            << "Upper convection must not repeat the lower weather-map footprint as a vertical stamp";
    }

    /// Verify that production Clear clouds do not carry their lower support unchanged into the upper layer.
    TEST(AtmosphereShaderTest, productionClearCloudsDoNotExtrudeCoverageColumns)
    {
        const auto densitySupport = [](float eyeHeight) {
            NativePassInputs inputs = makeProductionAtmosphereInputs("Clear", "Day");
            inputs.width = 96;
            inputs.height = 96;
            inputs.eyePosition = { 43333.f, 236667.f, eyeHeight };
            inputs.cloudDensitySliceExtent = { 333333.f, 333333.f };
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

        const std::vector<bool> lowerSupport = densitySupport(78000.f);
        const std::vector<bool> upperSupport = densitySupport(175000.f);
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Clear", "Day");
        inputs.width = 192;
        inputs.height = 96;
        inputs.eyePosition = { 43333.f, 355000.f, 78000.f };
        inputs.cloudDensitySliceExtent = { 333333.f, 110000.f };
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Clear", "Day");
        inputs.width = 192;
        inputs.height = 128;
        inputs.eyePosition = { 43333.f, 355000.f, 78000.f };
        inputs.cloudDensitySliceExtent = { 333333.f, 130000.f };
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

    /// Verify that Clear weather produces a varied population rather than one dominant macro-scale blob.
    TEST(AtmosphereShaderTest, productionClearCloudPopulationUsesConvectiveCellScale)
    {
        NativePassInputs inputs = makeProductionAtmosphereInputs("Clear", "Day");
        inputs.width = 128;
        inputs.height = 128;
        inputs.eyePosition = { 43333.f, 236667.f, 65000.f };
        inputs.cloudDensitySliceExtent = { 500000.f, 500000.f };
        inputs.renderCloudDensitySlice = true;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(density.width, inputs.width);
        ASSERT_EQ(density.height, inputs.height);

        std::vector<bool> visited(density.pixels.size());
        std::size_t occupiedPixels = 0;
        std::size_t largestComponent = 0;
        std::size_t substantialComponents = 0;
        for (int y = 0; y < density.height; ++y)
            for (int x = 0; x < density.width; ++x)
            {
                const std::size_t startIndex = static_cast<std::size_t>(y * density.width + x);
                if (visited[startIndex] || density.pixels[startIndex].x() <= 0.01f)
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
                        if (visited[neighborIndex] || density.pixels[neighborIndex].x() <= 0.01f)
                            continue;
                        visited[neighborIndex] = true;
                        pending.emplace_back(neighborX, neighborY);
                    }
                }
                occupiedPixels += componentSize;
                largestComponent = std::max(largestComponent, componentSize);
                substantialComponents += componentSize >= 12 ? 1u : 0u;
            }

        ASSERT_GT(occupiedPixels, 0u);
        EXPECT_GE(substantialComponents, 4u)
            << "Clear weather must contain several independently shaped cloud masses"
            << " components=" << substantialComponents << " occupied=" << occupiedPixels;
        EXPECT_LT(static_cast<double>(largestComponent) / static_cast<double>(occupiedPixels), 0.6)
            << "A single weather-map feature must not dominate the entire Clear cloud population"
            << " largest=" << largestComponent << " occupied=" << occupiedPixels;
    }

    /// Verify that cloud sun access varies continuously across an occupied density field.
    TEST(AtmosphereShaderTest, productionCloudLightingAvoidsQuantizedSunAccessContours)
    {
        NativePassInputs inputs = makeProductionAtmosphereInputs("Thunderstorm", "Day");
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Thunderstorm", "Day");
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
            NativePassInputs inputs = makeProductionAtmosphereInputs(weather, "Day");
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
                NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Day");
                inputs.width = 4;
                inputs.height = 4;
                inputs.renderCloudDensitySlice = true;
                inputs.cloudDensitySliceQuantity = 5;
                inputs.cloudDensitySliceExtent.x() = relativeHeight;
                inputs.cloudShapeParameters.y() = featureScale;
                const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(image.pixels.empty());
                // The 700 m shear is applied after the position warp and before the 0.0006/m noise scale.
                const double displacement = relativeHeight * 700.0 * 0.0006 / featureScale;
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Day");
        inputs.width = 4;
        inputs.height = 4;
        inputs.renderCloudDensitySlice = true;
        inputs.cloudDensitySliceQuantity = 5;
        inputs.cloudDensitySliceExtent.x() = 0.f;
        inputs.cloudShapeParameters.y() = 1.f;
        for (bool useUniformBuffer : { false, true })
        {
            inputs.useUniformBuffer = useUniformBuffer;
            for (float phase : { -1.f, 0.f, 1.f, 2.f })
            {
                inputs.cloudAdvectionOffsetWorldUnits = osg::Vec2f(7800.f, 3600.f) * phase;
                const PassImage image = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
                ASSERT_FALSE(image.pixels.empty());
                const double alongWind = std::hypot(7800.0, 3600.0) * phase;
                const double angle = std::atan2(3600.0, 7800.0) + 0.75;
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Day");
        inputs.width = 128;
        inputs.height = 128;
        inputs.eyePosition = { 0.f, 0.f, 70000.f };
        inputs.cloudDensitySliceExtent = { 333333.f, 333333.f };
        inputs.renderCloudDensitySlice = true;
        const PassImage initial = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_FALSE(initial.pixels.empty());
        for (float phase : { 0.0001f, 1.f })
        {
            NativePassInputs moving = inputs;
            moving.cloudAdvectionOffsetWorldUnits = osg::Vec2f(15600.f, 7200.f) * phase;
            moving.eyePosition.x() -= moving.cloudAdvectionOffsetWorldUnits.x();
            moving.eyePosition.y() -= moving.cloudAdvectionOffsetWorldUnits.y();
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Day");
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
        inputs.cloudShapeParameters = { 1.f, 1.05f, 1.f, 0.9f };
        inputs.cloudOrganizationParameters = { 0.92f, 0.72f, 1.f, 1.f };
        inputs.cloudStructureParameters = { 0.72f, 0.24f, 0.f, 0.f };
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
            double clearDayRedBlueRatio = 0.0;
            double clearSunsetRedBlueRatio = 0.0;
            for (const Scenario& scenario : scenarios)
            {
                if (preset != "default" && scenario.mWeather != "Clear")
                    continue;
                NativePassInputs inputs
                    = makeProductionAtmosphereInputs(scenario.mWeather, scenario.mTimeOfDay, preset);
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

                if (scenario.mWeather == "Clear" && scenario.mTimeOfDay == "Day")
                    clearDayRedBlueRatio = meanCloudRadiance.x() / std::max(meanCloudRadiance.z(), 1e-6);
                if (scenario.mWeather == "Clear" && scenario.mTimeOfDay == "Sunset")
                    clearSunsetRedBlueRatio = meanCloudRadiance.x() / std::max(meanCloudRadiance.z(), 1e-6);
            }
            EXPECT_GT(clearSunsetRedBlueRatio, clearDayRedBlueRatio + 0.03) << preset;
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

    TEST(AtmosphereShaderTest, temporalHistoryClipsAllChannelsToCurrentNeighborhoodVariance)
    {
        NativePassInputs inputs;
        inputs.width = 8;
        inputs.height = 8;
        inputs.volumeResolutionScale = 1.f;
        inputs.quality = 3;
        inputs.historyValid = true;
        inputs.passInput = [](int x, int) {
            const float value = x % 2 == 0 ? 0.75f : 0.25f;
            return osg::Vec4f(value, value * 2.f, value * 0.5f, value);
        };
        inputs.volumeMetadataInput = [](int, int) {
            const float distance = encodeAtmosphereDistance(20000.f);
            return osg::Vec4f(distance, 1.f, distance, 2.f);
        };
        inputs.atmosphereMetadataHistory = inputs.volumeMetadataInput;
        inputs.atmosphereHistory = [](int, int) { return osg::Vec4f(10.f, -10.f, 10.f, 10.f); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(resolved.pixels.empty());
        const osg::Vec4f center = inputs.passInput(4, 4);
        for (int channel = 0; channel < 4; ++channel)
        {
            double sum = 0.0;
            double squaredSum = 0.0;
            constexpr std::array<std::pair<int, int>, 5> blockOffsets{ std::pair{ 0, 0 }, { -1, 0 }, { 1, 0 },
                { 0, -1 }, { 0, 1 } };
            for (const auto [offsetX, offsetY] : blockOffsets)
            {
                const int blockX = 2 + offsetX;
                const int blockY = 2 + offsetY;
                const int phase = (inputs.frameNumber + (blockX + blockY) % 2) % 4;
                const double value = inputs.passInput(blockX * 2 + phase % 2, blockY * 2 + phase / 2)[channel];
                sum += value;
                squaredSum += value * value;
            }
            const double mean = sum / static_cast<double>(blockOffsets.size());
            const double deviation = std::sqrt(squaredSum / static_cast<double>(blockOffsets.size()) - mean * mean);
            const double boundedHistory = channel == 1 ? std::min(mean - deviation, double(center[channel]))
                                                       : std::max(mean + deviation, double(center[channel]));
            // Match the settled volumetric-cloud temporal response.
            const double historyWeight = 0.96;
            const double expected = (1.0 - historyWeight) * center[channel] + historyWeight * boundedHistory;
            EXPECT_NEAR(resolved.at(4, 4)[channel], expected, 0.001);
        }
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
        EXPECT_FLOAT_EQ(scaleFor(0.f, 0.f, 2.f), 1.f);
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

    /// Constrain copied clear-sky checkerboard history to the current traced neighborhood.
    TEST(AtmosphereShaderTest, copiedClearSkyHistoryRejectsValuesOutsideCurrentNeighborhood)
    {
        const osg::Vec4f currentCloud(0.2f, 0.24f, 0.28f, 0.8f);
        const osg::Vec4f staleCloud(0.9f, 0.85f, 0.8f, 0.1f);
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
        inputs.atmosphereHistory = [staleCloud](int, int) { return staleCloud; };
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
                    EXPECT_NEAR(reprojected.at(x, y)[channel], currentCloud[channel], 0.002f);
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

    TEST(AtmosphereShaderTest, translatedHistoryMatchesAnAnalyticWorldSpaceRadiancePlane)
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
                    const float noise = (x + y) % 2 == 0 ? 0.003f : -0.003f;
                    return color(x, y, 0.f) + osg::Vec4f(noise, noise, noise, noise);
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
                        for (int channel = 0; channel < 4; ++channel)
                            EXPECT_NEAR(result.at(x, y)[channel], color(x, y, 0.f)[channel], 0.001f);
            }
    }

    /// Reject geometric discontinuities while retaining compatible history across jittered medium labels.
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
        EXPECT_LT(std::abs(changedMediumCenter.x() - 0.55f), std::abs(rawCenter.x() - 0.55f));
        EXPECT_LT(std::abs(changedMediumCenter.w() - 0.55f), std::abs(rawCenter.w() - 0.55f));
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

        NativePassInputs previousInputs = makeProductionAtmosphereInputs("Cloudy", "Sunset");
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
        NativePassInputs currentInputs = makeProductionAtmosphereInputs("Cloudy", "Day");
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

    /// Follow the per-frame temporal blue-noise phase used by the reference cloud reconstruction.
    TEST(AtmosphereShaderTest, cloudSamplesFollowTemporalBlueNoiseSequence)
    {
        NativePassInputs inputs;
        inputs.width = 16;
        inputs.height = 16;
        inputs.renderCloudRayOffsets = true;
        constexpr std::array frameNumbers{ 0, 1, 7, 15 };
        std::array<PassImage, frameNumbers.size()> offsets;
        for (std::size_t sample = 0; sample < offsets.size(); ++sample)
        {
            inputs.frameNumber = frameNumbers[sample];
            offsets[sample] = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(offsets[sample].pixels.size(), 256u);
        }
        float maximumTemporalError = 0.f;
        float maximumStableError = 0.f;
        constexpr float goldenRatioConjugate = 0.61803398875f;
        for (std::size_t pixel = 0; pixel < offsets.front().pixels.size(); ++pixel)
        {
            const float spatial = offsets.front().pixels[pixel].g();
            for (std::size_t sample = 0; sample < offsets.size(); ++sample)
            {
                const int frame = frameNumbers[sample];
                const float temporalPhase = frame * goldenRatioConjugate - std::floor(frame * goldenRatioConjugate);
                const float phased = spatial + temporalPhase;
                const float expected = phased - std::floor(phased);
                maximumTemporalError
                    = std::max(maximumTemporalError, std::abs(offsets[sample].pixels[pixel].r() - expected));
                maximumStableError
                    = std::max(maximumStableError, std::abs(offsets[sample].pixels[pixel].g() - spatial));
            }
        }
        EXPECT_LT(maximumTemporalError, 0.001f);
        EXPECT_LT(maximumStableError, 0.001f);
    }

    /// Keep reconstructed checkerboard members from sharing a directional spatial sample pattern.
    TEST(AtmosphereShaderTest, cloudCheckerboardMembersUseDistinctSpatialDither)
    {
        NativePassInputs inputs;
        inputs.width = 16;
        inputs.height = 16;
        inputs.renderCloudRayOffsets = true;
        const PassImage offsets = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
        ASSERT_EQ(offsets.pixels.size(), 256u);

        std::size_t distinctBlocks = 0;
        std::size_t blockCount = 0;
        for (int y = 0; y < inputs.height; y += 2)
            for (int x = 0; x < inputs.width; x += 2)
            {
                const std::array<float, 4> samples{ offsets.at(x, y).g(), offsets.at(x + 1, y).g(),
                    offsets.at(x, y + 1).g(), offsets.at(x + 1, y + 1).g() };
                std::size_t distinctSampleCount = 0;
                for (std::size_t sampleIndex = 0; sampleIndex < samples.size(); ++sampleIndex)
                {
                    bool matchesEarlierSample = false;
                    for (std::size_t earlierIndex = 0; earlierIndex < sampleIndex; ++earlierIndex)
                        matchesEarlierSample |= std::abs(samples[sampleIndex] - samples[earlierIndex]) < 0.001f;
                    distinctSampleCount += matchesEarlierSample ? 0u : 1u;
                }
                distinctBlocks += distinctSampleCount >= 3 ? 1u : 0u;
                ++blockCount;
            }

        EXPECT_GT(distinctBlocks, blockCount * 9 / 10);
    }

    /// Preserve a broad blue-noise distribution throughout checkerboard refresh cycles.
    TEST(AtmosphereShaderTest, cloudRayPhaseRetainsBroadDistribution)
    {
        NativePassInputs inputs;
        inputs.width = 16;
        inputs.height = 16;
        inputs.renderCloudRayOffsets = true;
        for (const int frameNumber : { 20, 21, 23, 27 })
        {
            inputs.frameNumber = frameNumber;
            const PassImage offsets = renderNativeAtmospherePass("integrateAtmosphereRayGrid", inputs);
            ASSERT_EQ(offsets.pixels.size(), 256u);
            double mean = 0.0;
            float minimum = 1.f;
            float maximum = 0.f;
            for (const osg::Vec4f& pixel : offsets.pixels)
            {
                mean += pixel.r();
                minimum = std::min(minimum, pixel.r());
                maximum = std::max(maximum, pixel.r());
            }
            mean /= static_cast<double>(offsets.pixels.size());
            EXPECT_NEAR(mean, 0.5, 0.03);
            EXPECT_LT(minimum, 0.05f);
            EXPECT_GT(maximum, 0.95f);
        }
    }

    TEST(AtmosphereShaderTest, temporalCloudHistoryResolvesDeterministicFrameDither)
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
        const PassImage secondFrame = renderNativeAtmospherePass("integrateAtmosphereRayGrid", secondFrameInputs);
        ASSERT_EQ(firstFrame.pixels.size(), secondFrame.pixels.size());

        double deterministicDifference = 0.0;
        double stableNonTemporalDifference = 0.0;
        double temporalDitherDifference = 0.0;
        for (std::size_t index = 0; index < firstFrame.pixels.size(); ++index)
            for (int channel = 0; channel < 4; ++channel)
            {
                deterministicDifference
                    += std::abs(firstFrame.pixels[index][channel] - repeatedFirstFrame.pixels[index][channel]);
                stableNonTemporalDifference
                    += std::abs(firstFrame.pixels[index][channel] - stableNonTemporalFrame.pixels[index][channel]);
                temporalDitherDifference
                    += std::abs(firstFrame.pixels[index][channel] - secondFrame.pixels[index][channel]);
            }
        EXPECT_LT(deterministicDifference, 1e-6);
        EXPECT_LT(stableNonTemporalDifference, 1e-6);
        EXPECT_GT(temporalDitherDifference, 0.01);

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
            const int x = static_cast<int>(index) % disoccluded.width;
            const int y = static_cast<int>(index) / disoccluded.width;
            const int phase = (secondFrameInputs.frameNumber + (x / 2 + y / 2) % 2) % 4;
            const bool tracedPixel = x % 2 + (y % 2) * 2 == phase;
            const PassImage& fresh = tracedPixel ? secondFrame : firstFrame;
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(disoccluded.pixels[index][channel], fresh.pixels[index][channel], 0.002f);
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
                    = makeProductionAtmosphereInputs(cloudsEnabled ? "Cloudy" : "Rain", "Sunset");
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Rain", "Sunset");
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

    /// Verify the real sparse trace visits each dense ray once, without moving its dither or optical model.
    TEST(AtmosphereShaderTest, checkerboardFourPhasesMatchDenseRayGrid)
    {
        NativePassInputs denseInputs = makeProductionAtmosphereInputs("Cloudy", "Sunset");
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
        NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Sunset");
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
                NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Sunset");
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

        NativePassInputs inputs = makeProductionAtmosphereInputs("Rain", "Sunset");
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
        constexpr int width = 48;
        constexpr int height = 27;
        const NativePassInputs day = makeProductionAtmosphereInputs("Cloudy", "Day");
        const NativePassInputs sunset = makeProductionAtmosphereInputs("Cloudy", "Sunset");
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

    TEST(AtmosphereShaderTest, integratedMetadataPreservesOrderedMistCloudMoments)
    {
        NativePassInputs inputs;
        inputs.width = 32;
        inputs.height = 24;
        inputs.cloudParameters = { 1.f, 1.2f, 55000.f, 18000.f };
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

    /// Retain bounded ordered-volume history when jitter changes which overlapping medium dominates the sample.
    TEST(AtmosphereShaderTest, dominantMediumChangesRetainCompatibleRadianceHistory)
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
            EXPECT_GT(std::abs(center[channel] - brightCloud[channel]), 0.05f);
            EXPECT_LT(std::abs(center[channel] - historyVolume[channel]),
                std::abs(brightCloud[channel] - historyVolume[channel]));
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

    /// Suppress an isolated stochastic cloud-ray excursion with the production per-refresh temporal response.
    TEST(AtmosphereShaderTest, cloudHistoryRejectsVisibleRayMarchGrain)
    {
        const osg::Vec4f stableCloud(0.3f, 0.32f, 0.34f, 0.7f);
        const osg::Vec4f noisyCloud(0.9f, 0.92f, 0.94f, 0.1f);
        const osg::Vec4f cloudMetadata(encodeAtmosphereDistance(18000.f), 0.3f, encodeAtmosphereDistance(20000.f), 2.f);

        NativePassInputs inputs;
        inputs.historyValid = true;
        inputs.passInput = [stableCloud, noisyCloud](int x, int y) {
            return x >= 31 && x <= 32 && y >= 31 && y <= 32 ? noisyCloud : stableCloud;
        };
        inputs.volumeMetadataInput = [cloudMetadata](int, int) { return cloudMetadata; };
        inputs.atmosphereHistory = [stableCloud](int, int) { return stableCloud; };
        inputs.atmosphereMetadataHistory = [cloudMetadata](int, int) { return cloudMetadata; };

        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(resolved.pixels.empty());
        const osg::Vec4f& center = resolved.at(resolved.width / 2, resolved.height / 2);
        for (int channel = 0; channel < 4; ++channel)
        {
            const float expected = noisyCloud[channel] * 0.04f + stableCloud[channel] * 0.96f;
            EXPECT_NEAR(center[channel], expected, 0.002f);
        }
    }

    /// Keep shallow mist and fog integration stable when the medium field and camera have not changed.
    TEST(AtmosphereShaderTest, stationaryLowMediaDoNotInjectTemporalRayNoise)
    {
        NativePassInputs inputs = makeProductionAtmosphereInputs("Cloudy", "Sunset");
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
            NativePassInputs previous = makeProductionAtmosphereInputs("Cloudy", "Day");
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
            return x >= 31 && x <= 32 && y >= 31 && y <= 32 ? currentVolume : historyVolume;
        };
        inputs.volumeMetadataInput = [cloudMetadata](int, int) { return cloudMetadata; };
        inputs.atmosphereHistory = [historyVolume](int, int) { return historyVolume; };
        inputs.atmosphereMetadataHistory = [cloudMetadata](int, int) { return cloudMetadata; };
        inputs.previousDepth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f; };

        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", inputs);
        ASSERT_FALSE(resolved.pixels.empty());
        const osg::Vec4f& center = resolved.at(resolved.width / 2, resolved.height / 2);
        for (int channel = 0; channel < 4; ++channel)
        {
            EXPECT_GT(std::abs(center[channel] - currentVolume[channel]), 0.001f);
            EXPECT_LT(std::abs(center[channel] - currentVolume[channel]),
                std::abs(historyVolume[channel] - currentVolume[channel]));
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
                cumulativeResolvedOpacityError
                    += std::abs(resolved.pixels[index].w() - reference.pixels[index].w());
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
            ProductionAtmosphereFrame resolved
                = renderProductionAtmosphereVolumeFrame(currentInputs, &previousFrame);
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

    TEST(AtmosphereShaderTest, translatingCameraUsesTemporalCloudSamples)
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
        EXPECT_GT(meanRgbDifference(first, second), 1e-6);
    }

    TEST(AtmosphereShaderTest, stationaryCloudHistoryConvergesFrameDither)
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
                        rawVariation
                            += std::abs(raw.pixels[pixelIndex][channel]
                                - previousFrame.mRawVolume.pixels[pixelIndex][channel]);
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
        volumeInputs.cloudShapeParameters = { 0.55f, 1.f, 0.62f, 0.08f };
        volumeInputs.lowCloudParameters = { 0.32f, 0.8f, 4200.f, 11000.f };
        volumeInputs.lowCloudShapeParameters = { 0.38f, 1.25f, 0.42f, 1.f };
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

    /// Ensure screen-space shafts cannot originate behind geometry, cloud extinction, or the camera.
    TEST(AtmosphereShaderTest, sunShaftsRequireVisibleUnextinguishedSunSource)
    {
        NativePassInputs visibleInputs;
        visibleInputs.lastShader = [](int x, int y) {
            if (std::abs(x - 32) <= 2 && std::abs(y - 32) <= 2)
                return osg::Vec4f(4.f, 3.4f, 2.6f, 1.f);
            return osg::Vec4f(0.3f, 0.4f, 0.55f, 1.f);
        };
        visibleInputs.passInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        const PassImage visible = renderNativeAtmospherePass("integrateShafts", visibleInputs);
        ASSERT_FALSE(visible.pixels.empty());

        NativePassInputs occludedInputs = visibleInputs;
        occludedInputs.depth = [](int, int) { return SceneUtil::AutoDepth::isReversed() ? 1.f : 0.f; };
        const PassImage occluded = renderNativeAtmospherePass("integrateShafts", occludedInputs);
        NativePassInputs sourceOccludedInputs = visibleInputs;
        sourceOccludedInputs.depth = [](int x, int y) {
            const bool coversSun = std::abs(x - 32) <= 2 && std::abs(y - 32) <= 2;
            if (coversSun)
                return SceneUtil::AutoDepth::isReversed() ? 1.f : 0.f;
            return SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f;
        };
        const PassImage sourceOccluded = renderNativeAtmospherePass("integrateShafts", sourceOccludedInputs);
        NativePassInputs cloudExtinguishedInputs = visibleInputs;
        cloudExtinguishedInputs.volumeHistoryInput = [](int x, int y) {
            const bool coversSun = std::abs(x - 32) <= 2 && std::abs(y - 32) <= 2;
            return osg::Vec4f(0.f, 0.f, 0.f, coversSun ? 0.f : 1.f);
        };
        const PassImage cloudExtinguished
            = renderNativeAtmospherePass("integrateShafts", cloudExtinguishedInputs);
        NativePassInputs behindInputs = visibleInputs;
        behindInputs.sunDirection = { -1.f, 0.f, 0.f, 0.f };
        const PassImage behind = renderNativeAtmospherePass("integrateShafts", behindInputs);

        const auto energy = [](const PassImage& image) {
            double result = 0.0;
            for (const osg::Vec4f& pixel : image.pixels)
                result += pixel.x() + pixel.y() + pixel.z();
            return result / static_cast<double>(image.pixels.size());
        };
        EXPECT_GT(energy(visible), 0.002);
        EXPECT_LT(energy(occluded), energy(visible) * 0.02);
        EXPECT_LT(energy(sourceOccluded), 1e-6);
        EXPECT_LT(energy(cloudExtinguished), 1e-6);
        EXPECT_LT(energy(behind), 1e-6);
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
        EXPECT_NEAR(center.x(), 0.8f, 0.002f);
        EXPECT_NEAR(center.y(), 1.3f, 0.002f);
        EXPECT_NEAR(center.z(), 1.85f, 0.002f);
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
        const PassImage shafts = renderNativeAtmospherePass("integrateShafts", shaftInputs);
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
