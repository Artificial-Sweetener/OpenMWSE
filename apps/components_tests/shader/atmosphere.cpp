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
        result.cloudStructureParameters
            = { volume["cloudTowerTaper"].as<float>(), volume["cloudBaseVariation"].as<float>(),
                  volume["cloudDeckType"].as<float>(), 0.f };
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

    /// Compile and execute one production atmosphere pass in an off-screen graphics context.
    PassImage renderNativeAtmospherePass(std::string_view passName, const NativePassInputs& inputs)
    {
        const int width = inputs.width;
        const int height = inputs.height;
        const int sceneWidth = inputs.sceneWidth > 0 ? inputs.sceneWidth : width;
        const int sceneHeight = inputs.sceneHeight > 0 ? inputs.sceneHeight : height;

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
        capture->setInternalTextureFormat(GL_RGBA16F);
        camera->attach(osg::Camera::COLOR_BUFFER, capture);

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
    float density = openmwseCloudDensity(samplePosition, 0.0, 250.0, 0.0, mainLayer);
    float diagnosticValue = density;
    if (openmwseTestCloudDensitySliceQuantity == 1 && density > 0.01)
    {
        diagnosticValue = openmwseCloudSunAccess(samplePosition, normalize(omw.sunPos.xyz),
            omw.atmosphereQuality, density, 0.0, mainLayer, lowLayer);
    }
    omw_FragColor = vec4(diagnosticValue, diagnosticValue, diagnosticValue, density);
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
            = makeColorTexture(width, height, inputs.atmosphereHistory, osg::Texture::NEAREST);
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
            if (inputName == "AtmosphereVolumeHistory" && inputs.volumeHistoryInput)
            {
                input = &inputs.volumeHistoryInput;
                inputWidth = inputs.volumeHistoryWidth > 0 ? inputs.volumeHistoryWidth : width;
                inputHeight = inputs.volumeHistoryHeight > 0 ? inputs.volumeHistoryHeight : height;
            }
            else if (inputName == "AtmosphereVolumeMetadataHistory" && inputs.volumeMetadataInput)
            {
                input = &inputs.volumeMetadataInput;
                inputWidth = inputs.volumeMetadataWidth > 0 ? inputs.volumeMetadataWidth : width;
                inputHeight = inputs.volumeMetadataHeight > 0 ? inputs.volumeMetadataHeight : height;
            }
            else if (inputName == "AtmosphereVolumeMetadataHistory")
                input = &defaultMetadata;
            else if (inputName == "AtmosphereShafts" && inputs.shaftInput)
            {
                input = &inputs.shaftInput;
                inputWidth = inputs.shaftWidth > 0 ? inputs.shaftWidth : width;
                inputHeight = inputs.shaftHeight > 0 ? inputs.shaftHeight : height;
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
        const double aspectRatio = static_cast<double>(width) / static_cast<double>(height);
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
        const osg::Matrixf previousView
            = osg::Matrixf::lookAt(previousEyePosition, previousEyePosition + viewDirection, osg::Vec3f(0.f, 0.f, 1.f));
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

    /// Own every native pass output produced for one production-scaled atmosphere frame.
    struct ProductionAtmosphereFrame
    {
        PassImage mRawVolume;
        PassImage mMetadata;
        PassImage mResolvedVolume;
        PassImage mShafts;
        PassImage mComposite;
    };

    /// Execute the complete native atmosphere technique at its production render-target resolutions.
    ProductionAtmosphereFrame renderProductionAtmosphereFrame(
        const NativePassInputs& requestedInputs, const ProductionAtmosphereFrame* previousFrame = nullptr)
    {
        const int sceneWidth = requestedInputs.width;
        const int sceneHeight = requestedInputs.height;
        const float volumeScale = MWRender::getAtmosphereVolumeResolutionScale(requestedInputs.quality);
        const int volumeWidth = std::max(1, static_cast<int>(std::round(sceneWidth * volumeScale)));
        const int volumeHeight = std::max(1, static_cast<int>(std::round(sceneHeight * volumeScale)));
        const int shaftWidth = std::max(1, static_cast<int>(std::round(sceneWidth * volumeScale * 0.5f)));
        const int shaftHeight = std::max(1, static_cast<int>(std::round(sceneHeight * volumeScale * 0.5f)));

        NativePassInputs volumeInputs = requestedInputs;
        volumeInputs.width = volumeWidth;
        volumeInputs.height = volumeHeight;
        volumeInputs.sceneWidth = sceneWidth;
        volumeInputs.sceneHeight = sceneHeight;
        volumeInputs.volumeResolutionScale = volumeScale;
        ProductionAtmosphereFrame frame;
        frame.mRawVolume = renderNativeAtmospherePass("integrateAtmosphereVolume", volumeInputs);

        NativePassInputs metadataInputs = volumeInputs;
        metadataInputs.passInput = [&frame](int x, int y) { return frame.mRawVolume.at(x, y); };
        frame.mMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", metadataInputs);

        NativePassInputs resolveInputs = volumeInputs;
        resolveInputs.historyValid = requestedInputs.historyValid && previousFrame != nullptr;
        resolveInputs.passInput = [&frame](int x, int y) { return frame.mRawVolume.at(x, y); };
        resolveInputs.volumeMetadataInput = [&frame](int x, int y) { return frame.mMetadata.at(x, y); };
        resolveInputs.volumeMetadataWidth = volumeWidth;
        resolveInputs.volumeMetadataHeight = volumeHeight;
        if (previousFrame != nullptr)
        {
            resolveInputs.atmosphereHistory
                = [previousFrame](int x, int y) { return previousFrame->mResolvedVolume.at(x, y); };
            resolveInputs.atmosphereMetadataHistory
                = [previousFrame](int x, int y) { return previousFrame->mMetadata.at(x, y); };
        }
        frame.mResolvedVolume = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);

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
        EXPECT_EQ(water->getShaderSource().find("skyColorEstimate"), std::string::npos);
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
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereVolume", clearInputs);
        ASSERT_FALSE(clear.pixels.empty());

        NativePassInputs mistInputs;
        mistInputs.mistParameters = { 2.5f, 800.f, 4500.f, 1.f };
        const PassImage mist = renderNativeAtmospherePass("integrateAtmosphereVolume", mistInputs);
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
        const PassImage disabled = renderNativeAtmospherePass("integrateAtmosphereVolume", disabledInputs);
        ASSERT_EQ(disabled.pixels.size(), clear.pixels.size());
        EXPECT_NEAR(disabled.at(disabled.width / 2, disabled.height / 2).x(), 0.f, 1e-6f);
        EXPECT_FLOAT_EQ(disabled.at(disabled.width / 2, disabled.height / 2).w(), 1.f);
    }

    TEST(AtmosphereShaderTest, integratesCloudsBehindOpenSkyAndRespectsSceneDepth)
    {
        NativePassInputs openSkyInputs;
        openSkyInputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        openSkyInputs.mistParameters = {};
        const PassImage openSky = renderNativeAtmospherePass("integrateAtmosphereVolume", openSkyInputs);
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
        const PassImage occluded = renderNativeAtmospherePass("integrateAtmosphereVolume", occludedInputs);
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
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereVolume", clearInputs);

        NativePassInputs cloudInputs = clearInputs;
        cloudInputs.cloudParameters = { 0.95f, 1.3f, 48000.f, 22000.f };
        const PassImage cloudy = renderNativeAtmospherePass("integrateAtmosphereVolume", cloudInputs);
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
        const PassImage horizon = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
        ASSERT_FALSE(horizon.pixels.empty());

        double lostTransmittance = 0.0;
        for (const osg::Vec4f& pixel : horizon.pixels)
            lostTransmittance += 1.0 - pixel.w();
        EXPECT_GT(lostTransmittance / static_cast<double>(horizon.pixels.size()), 0.001)
            << "A spherical cloud shell must remain visible beyond a short terrain far plane at the horizon";
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
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereVolume", clearInputs);

        NativePassInputs lowCloudInputs = clearInputs;
        lowCloudInputs.lowCloudParameters = { 0.78f, 1.05f, 2600.f, 9000.f };
        lowCloudInputs.lowCloudShapeParameters = { 0.18f, 0.9f, 0.24f, 1.f };
        const PassImage lowClouds = renderNativeAtmospherePass("integrateAtmosphereVolume", lowCloudInputs);

        NativePassInputs shiftedTerrainInputs = lowCloudInputs;
        shiftedTerrainInputs.mediumReferenceParameters = { 42000.f, 1.f, 6000.f, 0.f };
        const PassImage shiftedTerrain = renderNativeAtmospherePass("integrateAtmosphereVolume", shiftedTerrainInputs);
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
        const PassImage baseline = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);

        inputs.mediumReferenceParameters = { 42000.f, 1.f, 6000.f, 5000.f };
        inputs.orographicParameters = { 0.f, 0.f, 0.35f, -0.2f };
        const PassImage terrainShifted = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const PassImage windward = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
        inputs.orographicParameters = { 0.f, 0.f, 0.2f, 0.f };
        const PassImage leeward = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const PassImage flatMist = renderNativeAtmospherePass("integrateAtmosphereVolume", mistInputs);
        mistInputs.mediumReferenceParameters.w() = 5000.f;
        const PassImage valleyMist = renderNativeAtmospherePass("integrateAtmosphereVolume", mistInputs);

        NativePassInputs fogInputs = mistInputs;
        fogInputs.mistParameters = {};
        fogInputs.fogVolumeParameters = { 0.8f, 0.f, 4200.f, 0.45f };
        fogInputs.mediumReferenceParameters.w() = 0.f;
        const PassImage flatFog = renderNativeAtmospherePass("integrateAtmosphereVolume", fogInputs);
        fogInputs.mediumReferenceParameters.w() = 5000.f;
        const PassImage valleyFog = renderNativeAtmospherePass("integrateAtmosphereVolume", fogInputs);

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
        const PassImage clear = renderNativeAtmospherePass("integrateAtmosphereVolume", clearInputs);

        NativePassInputs lowCloudInputs = clearInputs;
        lowCloudInputs.lowCloudParameters = { 0.82f, 1.1f, 2400.f, 10000.f };
        lowCloudInputs.lowCloudShapeParameters = { 0.2f, 0.9f, 0.28f, 1.f };
        const PassImage lowClouds = renderNativeAtmospherePass("integrateAtmosphereVolume", lowCloudInputs);
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
        const PassImage stratus = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
        const osg::Vec4f cumulusShape{ 0.64f, 0.9f, 0.74f, 0.f };
        const osg::Vec4f cumulusOrganization{ 0.46f, 0.5f, 0.72f, 0.12f };
        const osg::Vec4f cumulusStructure{ 0.68f, 0.46f, 0.f, 0.f };
        inputs.cloudShapeParameters = cumulusShape;
        inputs.cloudOrganizationParameters = cumulusOrganization;
        inputs.cloudStructureParameters = cumulusStructure;
        const PassImage cumulus = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
        const osg::Vec4f stormShape{ 1.f, 1.05f, 1.f, 0.9f };
        const osg::Vec4f stormOrganization{ 0.9f, 0.72f, 1.f, 1.f };
        const osg::Vec4f stormStructure{ 0.72f, 0.24f, 0.f, 0.f };
        inputs.cloudShapeParameters = stormShape;
        inputs.cloudOrganizationParameters = stormOrganization;
        inputs.cloudStructureParameters = stormStructure;
        const PassImage storm = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
                  const PassImage image = renderNativeAtmospherePass("integrateAtmosphereVolume", depthInputs);
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
                  const PassImage image = renderNativeAtmospherePass("integrateAtmosphereVolume", upperInputs);
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
            const PassImage image = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
            const PassImage image = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
            std::vector<bool> support(image.pixels.size());
            for (std::size_t index = 0; index < image.pixels.size(); ++index)
                support[index] = image.pixels[index].x() > 0.01f;
            return support;
        };

        const std::vector<bool> lowerSupport = densitySupport(65000.f);
        const std::vector<bool> upperSupport = densitySupport(80000.f);
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
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const PassImage density = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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

    /// Verify that adaptive traversal does not erase broad cloud support as temporal sample positions change.
    TEST(AtmosphereShaderTest, adaptiveCloudTraversalRetainsSupportAcrossTemporalDither)
    {
        NativePassInputs inputs = makeProductionAtmosphereInputs("Clear", "Day");
        inputs.width = 96;
        inputs.height = 54;
        inputs.eyePosition = { 0.f, 0.f, 1200.f };
        inputs.viewDirection = { 1.f, 0.f, 0.18f };
        inputs.farPlane = 500000.f;
        inputs.fogNear = 1000000.f;
        inputs.fogFar = 2000000.f;
        inputs.mistParameters = {};
        inputs.lowCloudParameters = {};
        inputs.historyValid = true;
        inputs.historyLightingWeight = 1.f;

        std::vector<float> minimumOpacity(static_cast<std::size_t>(inputs.width * inputs.height), 1.f);
        std::vector<float> maximumOpacity(minimumOpacity.size(), 0.f);
        for (int frameNumber = 40; frameNumber < 48; ++frameNumber)
        {
            inputs.frameNumber = frameNumber;
            const PassImage frame = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
            ASSERT_EQ(frame.pixels.size(), minimumOpacity.size());
            for (std::size_t index = 0; index < frame.pixels.size(); ++index)
            {
                const float opacity = 1.f - frame.pixels[index].w();
                minimumOpacity[index] = std::min(minimumOpacity[index], opacity);
                maximumOpacity[index] = std::max(maximumOpacity[index], opacity);
            }
        }

        std::size_t occupiedPixels = 0;
        std::size_t droppedSupportPixels = 0;
        for (std::size_t index = 0; index < minimumOpacity.size(); ++index)
        {
            if (maximumOpacity[index] <= 0.08f)
                continue;
            ++occupiedPixels;
            droppedSupportPixels += minimumOpacity[index] < 0.01f ? 1u : 0u;
        }

        ASSERT_GT(occupiedPixels, 0u);
        EXPECT_LT(static_cast<double>(droppedSupportPixels) / static_cast<double>(occupiedPixels), 0.03)
            << "Changing temporal dither must not make established cloud tissue disappear from the raw volume"
            << " dropped=" << droppedSupportPixels << " occupied=" << occupiedPixels;
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
        const PassImage lighting = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const std::size_t populatedBins = std::ranges::count_if(
            sunAccessBins, [](std::size_t population) { return population > 0; });

        ASSERT_GT(occupiedPixels, 0u);
        EXPECT_GE(populatedBins, 32u)
            << "Cloud sun access must remain a continuous optical-depth response instead of a small shade palette"
            << " populated-bins=" << populatedBins << " occupied=" << occupiedPixels;
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
                const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const PassImage fogged = renderNativeAtmospherePass("integrateAtmosphereVolume", foggedInputs);
        ASSERT_FALSE(fogged.pixels.empty());

        NativePassInputs clearDistanceInputs = foggedInputs;
        clearDistanceInputs.fogNear = 1000000.f;
        clearDistanceInputs.fogFar = 2000000.f;
        const PassImage clearDistance = renderNativeAtmospherePass("integrateAtmosphereVolume", clearDistanceInputs);
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
        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);

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
        const PassImage lowSun = renderNativeAtmospherePass("integrateAtmosphereVolume", lowSunInputs);
        ASSERT_FALSE(lowSun.pixels.empty());

        NativePassInputs highSunInputs = lowSunInputs;
        highSunInputs.sunDirection = { 0.6f, 0.f, 0.8f, 0.f };
        const PassImage highSun = renderNativeAtmospherePass("integrateAtmosphereVolume", highSunInputs);
        NativePassInputs hiddenSunInputs = lowSunInputs;
        hiddenSunInputs.sunVisibility = 0.f;
        const PassImage hiddenSun = renderNativeAtmospherePass("integrateAtmosphereVolume", hiddenSunInputs);
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
                NativePassInputs inputs
                    = makeProductionAtmosphereInputs(scenario.mWeather, scenario.mTimeOfDay, preset);
                inputs.mistParameters = {};
                inputs.fogNear = 12000.f;
                inputs.fogFar = 180000.f;
                const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        const PassImage warm = renderNativeAtmospherePass("integrateAtmosphereVolume", warmInputs);
        ASSERT_FALSE(warm.pixels.empty());

        NativePassInputs coolInputs = warmInputs;
        coolInputs.fogColor = { 0.22f, 0.38f, 0.82f, 1.f };
        coolInputs.skyColor = { 0.18f, 0.2f, 0.34f, 1.f };
        coolInputs.skyIrradiance = { 0.18f, 0.2f, 0.34f };
        coolInputs.groundIrradiance = { 0.194f, 0.263f, 0.508f };
        const PassImage cool = renderNativeAtmospherePass("integrateAtmosphereVolume", coolInputs);
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
        const PassImage openSky = renderNativeAtmospherePass("integrateAtmosphereVolume", openSkyInputs);
        ASSERT_FALSE(openSky.pixels.empty());

        const double openSkyExtinction = std::accumulate(openSky.pixels.begin(), openSky.pixels.end(), 0.0,
            [](double total, const osg::Vec4f& pixel) { return total + 1.0 - pixel.w(); });
        EXPECT_GT(openSkyExtinction / static_cast<double>(openSky.pixels.size()), 0.005);

        NativePassInputs occludedInputs = openSkyInputs;
        occludedInputs.depth = [](int, int) { return 1.f; };
        const PassImage occluded = renderNativeAtmospherePass("integrateAtmosphereVolume", occludedInputs);
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

                const PassImage volume = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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

        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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

        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
        ASSERT_FALSE(clouds.pixels.empty());
        const double extinction = std::accumulate(clouds.pixels.begin(), clouds.pixels.end(), 0.0,
            [](double total, const osg::Vec4f& pixel) { return total + 1.0 - pixel.w(); });
        EXPECT_GT(extinction / static_cast<double>(clouds.pixels.size()), 0.005);
    }

    TEST(AtmosphereShaderTest, cloudVolumeAttenuatesAnalyticHdrSunSignal)
    {
        NativePassInputs cloudInputs;
        cloudInputs.cloudParameters = { 0.82f, 1.15f, 48000.f, 22000.f };
        cloudInputs.mistParameters = {};
        const PassImage clouds = renderNativeAtmospherePass("integrateAtmosphereVolume", cloudInputs);
        ASSERT_FALSE(clouds.pixels.empty());
        const auto densestCloud = std::min_element(clouds.pixels.begin(), clouds.pixels.end(),
            [](const osg::Vec4f& left, const osg::Vec4f& right) { return left.w() < right.w(); });
        const std::size_t cloudIndex = static_cast<std::size_t>(std::distance(clouds.pixels.begin(), densestCloud));
        const int sunX = static_cast<int>(cloudIndex % static_cast<std::size_t>(clouds.width));
        const int sunY = static_cast<int>(cloudIndex / static_cast<std::size_t>(clouds.width));
        ASSERT_LT(densestCloud->w(), 0.98f);

        NativePassInputs clearCompositeInputs;
        clearCompositeInputs.lastShader = [sunX, sunY](int x, int y) {
            const bool sunCore = std::abs(x - sunX) <= 3 && std::abs(y - sunY) <= 3;
            return sunCore ? osg::Vec4f(8.f, 6.8f, 5.2f, 1.f) : osg::Vec4f(0.3f, 0.4f, 0.55f, 1.f);
        };
        clearCompositeInputs.volumeHistoryInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        clearCompositeInputs.shaftInput = [](int, int) { return osg::Vec4f(); };
        const PassImage clearComposite = renderNativeAtmospherePass("composite", clearCompositeInputs);

        NativePassInputs cloudyCompositeInputs = clearCompositeInputs;
        cloudyCompositeInputs.volumeHistoryInput = [&clouds](int x, int y) { return clouds.at(x, y); };
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
        const PassImage unlit = renderNativeAtmospherePass("integrateAtmosphereVolume", unlitInputs);
        ASSERT_FALSE(unlit.pixels.empty());

        NativePassInputs litInputs = unlitInputs;
        litInputs.pointLightCount = 1;
        litInputs.pointLights[0] = { 1200.f, 0.f, 1200.f, 0.f };
        litInputs.pointLights[1] = { 8.f, 2.f, 1.f, 1.f };
        litInputs.pointLights[2] = { 1.f, 0.f, 0.f, 5000.f };
        const PassImage lit = renderNativeAtmospherePass("integrateAtmosphereVolume", litInputs);
        ASSERT_EQ(lit.pixels.size(), unlit.pixels.size());

        NativePassInputs lowQualityUnlitInputs = unlitInputs;
        lowQualityUnlitInputs.quality = 1;
        const PassImage lowQualityUnlit
            = renderNativeAtmospherePass("integrateAtmosphereVolume", lowQualityUnlitInputs);
        NativePassInputs lowQualityLitInputs = litInputs;
        lowQualityLitInputs.quality = 1;
        const PassImage lowQualityLit = renderNativeAtmospherePass("integrateAtmosphereVolume", lowQualityLitInputs);
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
        const PassImage translated
            = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", translatedInputs);
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
        EXPECT_NEAR(translatedCenter.x(), rawCenter.x(), 0.002f);
        EXPECT_NEAR(translatedCenter.w(), rawCenter.w(), 0.002f);
        EXPECT_NEAR(largeAdvectionCenter.x(), rawCenter.x(), 0.002f);
        EXPECT_NEAR(changedMediumCenter.x(), rawCenter.x(), 0.002f);
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
        constexpr int width = 96;
        constexpr int height = 54;
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
        const PassImage previousRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", previousInputs);
        NativePassInputs previousMetadataInputs = previousInputs;
        previousMetadataInputs.passInput = [&previousRaw](int x, int y) { return previousRaw.at(x, y); };
        const PassImage previousMetadata
            = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);

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
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", currentInputs);
        NativePassInputs currentMetadataInputs = currentInputs;
        currentMetadataInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        const PassImage currentMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);

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

    TEST(AtmosphereShaderTest, temporalCloudHistoryResolvesDeterministicFrameDither)
    {
        NativePassInputs firstFrameInputs;
        firstFrameInputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        firstFrameInputs.mistParameters = {};
        firstFrameInputs.frameNumber = 4;
        const PassImage firstFrame = renderNativeAtmospherePass("integrateAtmosphereVolume", firstFrameInputs);
        const PassImage repeatedFirstFrame = renderNativeAtmospherePass("integrateAtmosphereVolume", firstFrameInputs);
        ASSERT_EQ(firstFrame.pixels.size(), repeatedFirstFrame.pixels.size());

        NativePassInputs stableNonTemporalInputs = firstFrameInputs;
        stableNonTemporalInputs.frameNumber = 5;
        const PassImage stableNonTemporalFrame
            = renderNativeAtmospherePass("integrateAtmosphereVolume", stableNonTemporalInputs);
        ASSERT_EQ(firstFrame.pixels.size(), stableNonTemporalFrame.pixels.size());

        NativePassInputs secondFrameInputs = firstFrameInputs;
        secondFrameInputs.frameNumber = 5;
        secondFrameInputs.historyValid = true;
        const PassImage secondFrame = renderNativeAtmospherePass("integrateAtmosphereVolume", secondFrameInputs);
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
        currentMetadataInputs.passInput = resolveInputs.passInput;
        const PassImage currentMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);
        NativePassInputs previousMetadataInputs = firstFrameInputs;
        previousMetadataInputs.passInput = resolveInputs.atmosphereHistory;
        const PassImage previousMetadata
            = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);
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
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(disoccluded.pixels[index][channel], secondFrame.pixels[index][channel], 0.002f);
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
                const PassImage first = renderNativeAtmospherePass("integrateAtmosphereVolume", firstInputs);
                ASSERT_FALSE(first.pixels.empty());

                NativePassInputs secondInputs = firstInputs;
                secondInputs.frameNumber = 121;
                const PassImage second = renderNativeAtmospherePass("integrateAtmosphereVolume", secondInputs);
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
                = renderProductionAtmosphereFrame(inputs, frameNumber > 0 ? &previousFrame : nullptr);
            ASSERT_FALSE(frame.mComposite.pixels.empty());
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
            EXPECT_EQ(frame.mRawVolume.width, static_cast<int>(std::round(sceneWidth * volumeScale)));
            EXPECT_EQ(frame.mRawVolume.height, static_cast<int>(std::round(sceneHeight * volumeScale)));
            EXPECT_EQ(frame.mShafts.width, static_cast<int>(std::round(sceneWidth * volumeScale * 0.5f)));
            EXPECT_EQ(frame.mShafts.height, static_cast<int>(std::round(sceneHeight * volumeScale * 0.5f)));
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
        constexpr int width = 96;
        constexpr int height = 54;
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
                = renderProductionAtmosphereFrame(inputs, step > 0 ? &previousFrame : nullptr);
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
                NativePassInputs repeatedInputs = inputs;
                repeatedInputs.frameNumber += 41;
                const ProductionAtmosphereFrame repeated
                    = renderProductionAtmosphereFrame(repeatedInputs, &previousFrame);
                EXPECT_LT(meanAbsoluteDifference(frame.mRawVolume, repeated.mRawVolume), 1e-6)
                    << "step=" << step << " historyLightingWeight=" << inputs.historyLightingWeight;
            }
            previousFrame = frame;
        }
        EXPECT_GE(rejectedFrameCount, 3u);
    }

    TEST(AtmosphereShaderTest, classifiesOpenSkyCloudsAtTheirPhysicalDistanceBeyondTheFarPlane)
    {
        NativePassInputs inputs;
        inputs.width = 64;
        inputs.height = 64;
        inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        inputs.mistParameters = {};
        inputs.frameNumber = 0;
        const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
        ASSERT_FALSE(raw.pixels.empty());

        NativePassInputs classifyInputs = inputs;
        classifyInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
        const PassImage metadata = renderNativeAtmospherePass("classifyAtmosphereVolume", classifyInputs);
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

    TEST(AtmosphereShaderTest, cloudSupportClassificationRetainsOnlyStationaryDitherContribution)
    {
        NativePassInputs firstInputs;
        firstInputs.width = 96;
        firstInputs.height = 64;
        firstInputs.cloudParameters = { 0.3f, 0.48f, 48000.f, 22000.f };
        firstInputs.mistParameters = {};
        firstInputs.historyValid = true;
        firstInputs.frameNumber = 4;
        const PassImage firstRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", firstInputs);
        ASSERT_FALSE(firstRaw.pixels.empty());

        NativePassInputs secondInputs = firstInputs;
        secondInputs.frameNumber = 5;
        const PassImage secondRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", secondInputs);
        ASSERT_EQ(secondRaw.pixels.size(), firstRaw.pixels.size());

        NativePassInputs firstMetadataInputs = firstInputs;
        firstMetadataInputs.passInput = [&firstRaw](int x, int y) { return firstRaw.at(x, y); };
        const PassImage firstMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", firstMetadataInputs);
        NativePassInputs secondMetadataInputs = secondInputs;
        secondMetadataInputs.passInput = [&secondRaw](int x, int y) { return secondRaw.at(x, y); };
        secondMetadataInputs.atmosphereMetadataHistory
            = [&firstMetadata](int x, int y) { return firstMetadata.at(x, y); };
        const PassImage secondMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", secondMetadataInputs);
        ASSERT_EQ(secondMetadata.pixels.size(), firstMetadata.pixels.size());

        NativePassInputs movedMetadataInputs = secondInputs;
        movedMetadataInputs.previousEyeOffset = { 3000.f, -4000.f, 0.f };
        movedMetadataInputs.passInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        movedMetadataInputs.atmosphereMetadataHistory
            = [&firstMetadata](int x, int y) { return firstMetadata.at(x, y); };
        const PassImage movedMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", movedMetadataInputs);

        NativePassInputs expiredMetadataInputs = secondInputs;
        expiredMetadataInputs.frameNumber = 6;
        expiredMetadataInputs.passInput = [](int, int) { return osg::Vec4f(0.f, 0.f, 0.f, 1.f); };
        expiredMetadataInputs.atmosphereMetadataHistory
            = [&secondMetadata](int x, int y) { return secondMetadata.at(x, y); };
        const PassImage expiredMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", expiredMetadataInputs);
        ASSERT_EQ(expiredMetadata.pixels.size(), secondMetadata.pixels.size());

        std::size_t disappearingCloudSamples = 0;
        std::size_t movedRetainedSamples = 0;
        for (std::size_t index = 0; index < firstRaw.pixels.size(); ++index)
        {
            const bool firstContributes = 1.f - firstRaw.pixels[index].w() > 0.0001f;
            const bool secondContributes = 1.f - secondRaw.pixels[index].w() > 0.0001f;
            if (!firstContributes || secondContributes || firstMetadata.pixels[index].w() <= 1.5f)
                continue;
            ++disappearingCloudSamples;
            EXPECT_GT(secondMetadata.pixels[index].w(), 1.5f);
            EXPECT_NEAR(firstMetadata.pixels[index].x(), secondMetadata.pixels[index].x(), 0.002f);
            if (movedMetadata.pixels[index].w() > 1.5f)
                ++movedRetainedSamples;
            EXPECT_LT(expiredMetadata.pixels[index].w(), 0.5f);
        }
        EXPECT_GT(disappearingCloudSamples, 0u);
        EXPECT_LT(movedRetainedSamples, disappearingCloudSamples / 4);
    }

    TEST(AtmosphereShaderTest, newlyVisibleCloudRejectsEmptyHistory)
    {
        const osg::Vec4f currentVolume(0.18f, 0.22f, 0.28f, 0.64f);
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
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(center[channel], currentVolume[channel], 0.0002f);
    }

    TEST(AtmosphereShaderTest, movingGroundCameraRejectsOpenSkyCloudHistoryBeyondTheFarPlane)
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
            EXPECT_NEAR(center[channel], currentVolume[channel], 0.0003f);
    }

    TEST(AtmosphereShaderTest, movingCameraRejectsRealCloudHistoryFromGroundAndCloudLayerAltitudes)
    {
        constexpr int width = 96;
        constexpr int height = 64;
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
            const PassImage previousRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", previousInputs);
            ASSERT_FALSE(previousRaw.pixels.empty());
            NativePassInputs previousMetadataInputs = previousInputs;
            previousMetadataInputs.passInput = [&previousRaw](int x, int y) { return previousRaw.at(x, y); };
            const PassImage previousMetadata
                = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);
            ASSERT_EQ(previousMetadata.pixels.size(), previousRaw.pixels.size());

            const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", currentInputs);
            ASSERT_FALSE(currentRaw.pixels.empty());
            NativePassInputs currentMetadataInputs = currentInputs;
            currentMetadataInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
            const PassImage currentMetadata
                = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);
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
            EXPECT_EQ(historyContributingPixelCount, 0u) << "cameraAltitude=" << cameraAltitude;
        }
    }

    TEST(AtmosphereShaderTest, movingCameraOutputMatchesTheCurrentWorldSpaceCloudField)
    {
        constexpr int width = 96;
        constexpr int height = 64;
        constexpr int referenceFrameCount = 16;
        const osg::Vec3f currentEyePosition(0.f, 0.f, 1200.f);
        const osg::Vec3f previousEyeOffset(0.f, -180.f, 0.f);

        const auto renderCloudReference = [&](const osg::Vec3f& eyePosition) {
            PassImage reference;
            for (int frameNumber = 0; frameNumber < referenceFrameCount; ++frameNumber)
            {
                NativePassInputs inputs;
                inputs.width = width;
                inputs.height = height;
                inputs.eyePosition = eyePosition;
                inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
                inputs.mistParameters = {};
                inputs.frameNumber = frameNumber;
                inputs.historyValid = true;
                const PassImage sample = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
                if (reference.pixels.empty())
                {
                    reference.width = sample.width;
                    reference.height = sample.height;
                    reference.pixels.assign(sample.pixels.size(), osg::Vec4f());
                }
                for (std::size_t index = 0; index < sample.pixels.size(); ++index)
                    reference.pixels[index] += sample.pixels[index] / referenceFrameCount;
            }
            return reference;
        };

        const PassImage previousReference = renderCloudReference(currentEyePosition + previousEyeOffset);
        const PassImage currentReference = renderCloudReference(currentEyePosition);
        ASSERT_EQ(previousReference.pixels.size(), currentReference.pixels.size());

        NativePassInputs currentInputs;
        currentInputs.width = width;
        currentInputs.height = height;
        currentInputs.eyePosition = currentEyePosition;
        currentInputs.previousEyeOffset = previousEyeOffset;
        currentInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        currentInputs.mistParameters = {};
        currentInputs.frameNumber = referenceFrameCount + 1;
        currentInputs.historyValid = true;
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", currentInputs);

        NativePassInputs currentMetadataInputs = currentInputs;
        currentMetadataInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        const PassImage currentMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);

        NativePassInputs previousMetadataInputs = currentInputs;
        previousMetadataInputs.eyePosition += previousEyeOffset;
        previousMetadataInputs.previousEyeOffset = {};
        previousMetadataInputs.passInput = [&previousReference](int x, int y) { return previousReference.at(x, y); };
        const PassImage previousMetadata
            = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);

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

        EXPECT_NEAR(historyContribution, 0.0, 1e-6);
        EXPECT_NEAR(resolvedOpacityError, rawOpacityError, 1e-6)
            << "rawOpacityError=" << rawOpacityError << " resolvedOpacityError=" << resolvedOpacityError
            << " historyContribution=" << historyContribution;
    }

    TEST(AtmosphereShaderTest, sustainedCameraMotionDoesNotDragCloudHistoryBehindTheWorldField)
    {
        constexpr int width = 96;
        constexpr int height = 64;
        constexpr int referenceFrameCount = 8;
        constexpr int motionFrameCount = 10;
        const osg::Vec3f frameTranslation(0.f, 180.f, 0.f);

        const auto makeInputs = [&](const osg::Vec3f& eyePosition, int frameNumber) {
            NativePassInputs inputs;
            inputs.width = width;
            inputs.height = height;
            inputs.eyePosition = eyePosition;
            inputs.previousEyeOffset = -frameTranslation;
            inputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
            inputs.mistParameters = {};
            inputs.frameNumber = frameNumber;
            inputs.historyValid = true;
            return inputs;
        };
        const auto renderCloudReference = [&](const osg::Vec3f& eyePosition) {
            PassImage reference;
            for (int frameNumber = 0; frameNumber < referenceFrameCount; ++frameNumber)
            {
                NativePassInputs inputs = makeInputs(eyePosition, frameNumber);
                inputs.previousEyeOffset = {};
                const PassImage sample = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
                if (reference.pixels.empty())
                {
                    reference.width = sample.width;
                    reference.height = sample.height;
                    reference.pixels.assign(sample.pixels.size(), osg::Vec4f());
                }
                for (std::size_t index = 0; index < sample.pixels.size(); ++index)
                    reference.pixels[index] += sample.pixels[index] / referenceFrameCount;
            }
            return reference;
        };

        osg::Vec3f previousEyePosition;
        PassImage previousResolved = renderCloudReference(previousEyePosition);
        NativePassInputs previousMetadataInputs = makeInputs(previousEyePosition, 0);
        previousMetadataInputs.previousEyeOffset = {};
        previousMetadataInputs.passInput = [&previousResolved](int x, int y) { return previousResolved.at(x, y); };
        PassImage previousMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);

        double cumulativeRawOpacityError = 0.0;
        double cumulativeResolvedOpacityError = 0.0;
        for (int motionFrame = 1; motionFrame <= motionFrameCount; ++motionFrame)
        {
            const osg::Vec3f eyePosition = frameTranslation * static_cast<float>(motionFrame);
            NativePassInputs currentInputs = makeInputs(eyePosition, referenceFrameCount + motionFrame);
            const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", currentInputs);

            NativePassInputs currentMetadataInputs = currentInputs;
            currentMetadataInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
            currentMetadataInputs.atmosphereMetadataHistory
                = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
            const PassImage currentMetadata
                = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);

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

        EXPECT_NEAR(cumulativeResolvedOpacityError, cumulativeRawOpacityError, 1e-6)
            << "cumulativeRawOpacityError=" << cumulativeRawOpacityError
            << " cumulativeResolvedOpacityError=" << cumulativeResolvedOpacityError;
    }

    TEST(AtmosphereShaderTest, currentCloudFrameDoesNotDependOnPreviousCameraDirection)
    {
        constexpr int width = 96;
        constexpr int height = 64;
        const osg::Vec3f currentEyePosition(0.f, 0.f, 1200.f);

        NativePassInputs currentInputs;
        currentInputs.width = width;
        currentInputs.height = height;
        currentInputs.eyePosition = currentEyePosition;
        currentInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        currentInputs.mistParameters = {};
        currentInputs.frameNumber = 5;
        currentInputs.historyValid = true;
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", currentInputs);
        NativePassInputs currentMetadataInputs = currentInputs;
        currentMetadataInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        const PassImage currentMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);

        const auto resolveFromPreviousCamera = [&](const osg::Vec3f& previousEyeOffset) {
            NativePassInputs previousInputs = currentInputs;
            previousInputs.eyePosition += previousEyeOffset;
            previousInputs.frameNumber = 4;
            previousInputs.historyValid = false;
            const PassImage previousRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", previousInputs);
            NativePassInputs previousMetadataInputs = previousInputs;
            previousMetadataInputs.passInput = [&previousRaw](int x, int y) { return previousRaw.at(x, y); };
            const PassImage previousMetadata
                = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);

            NativePassInputs resolveInputs = currentInputs;
            resolveInputs.previousEyeOffset = previousEyeOffset;
            resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
            resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
            resolveInputs.atmosphereHistory = [&previousRaw](int x, int y) { return previousRaw.at(x, y); };
            resolveInputs.atmosphereMetadataHistory
                = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
            return renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        };

        const PassImage arrivedFromSouth = resolveFromPreviousCamera({ 0.f, -180.f, 0.f });
        const PassImage arrivedFromWest = resolveFromPreviousCamera({ -180.f, 0.f, 0.f });
        ASSERT_EQ(arrivedFromSouth.pixels.size(), arrivedFromWest.pixels.size());
        double directionDependentDifference = 0.0;
        for (std::size_t index = 0; index < arrivedFromSouth.pixels.size(); ++index)
            for (int channel = 0; channel < 4; ++channel)
                directionDependentDifference
                    += std::abs(arrivedFromSouth.pixels[index][channel] - arrivedFromWest.pixels[index][channel]);
        directionDependentDifference /= static_cast<double>(arrivedFromSouth.pixels.size() * 4);
        EXPECT_LT(directionDependentDifference, 1e-5);
    }

    TEST(AtmosphereShaderTest, translatingCameraUsesFrameStableCloudSamples)
    {
        NativePassInputs firstInputs;
        firstInputs.width = 96;
        firstInputs.height = 64;
        firstInputs.eyePosition = { 0.f, 0.f, 1200.f };
        firstInputs.previousEyeOffset = { 0.f, -180.f, 0.f };
        firstInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        firstInputs.mistParameters = {};
        firstInputs.frameNumber = 5;
        firstInputs.historyValid = true;
        const PassImage first = renderNativeAtmospherePass("integrateAtmosphereVolume", firstInputs);

        NativePassInputs secondInputs = firstInputs;
        secondInputs.frameNumber = 6;
        const PassImage second = renderNativeAtmospherePass("integrateAtmosphereVolume", secondInputs);
        ASSERT_EQ(first.pixels.size(), second.pixels.size());
        for (std::size_t index = 0; index < first.pixels.size(); ++index)
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(first.pixels[index][channel], second.pixels[index][channel], 1e-6f);
    }

    TEST(AtmosphereShaderTest, stationaryCloudHistoryConvergesFrameDither)
    {
        NativePassInputs volumeInputs;
        volumeInputs.cloudParameters = { 0.58f, 0.9f, 48000.f, 22000.f };
        volumeInputs.mistParameters = {};

        PassImage previousRaw;
        PassImage previousResolved;
        PassImage previousMetadata;
        double rawVariation = 0.0;
        double resolvedVariation = 0.0;
        std::size_t comparedValues = 0;

        for (int frameNumber = 0; frameNumber < 14; ++frameNumber)
        {
            volumeInputs.frameNumber = frameNumber;
            volumeInputs.historyValid = frameNumber > 0;
            const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereVolume", volumeInputs);
            ASSERT_FALSE(raw.pixels.empty());

            NativePassInputs metadataInputs = volumeInputs;
            metadataInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
            const PassImage metadata = renderNativeAtmospherePass("classifyAtmosphereVolume", metadataInputs);
            ASSERT_EQ(metadata.pixels.size(), raw.pixels.size());

            PassImage resolved = raw;
            if (frameNumber > 0)
            {
                NativePassInputs resolveInputs = volumeInputs;
                resolveInputs.historyValid = true;
                resolveInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
                resolveInputs.atmosphereHistory
                    = [&previousResolved](int x, int y) { return previousResolved.at(x, y); };
                resolveInputs.volumeMetadataInput = [&metadata](int x, int y) { return metadata.at(x, y); };
                resolveInputs.atmosphereMetadataHistory
                    = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
                resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
                ASSERT_EQ(resolved.pixels.size(), raw.pixels.size());
            }

            if (frameNumber >= 8)
            {
                for (std::size_t pixelIndex = 0; pixelIndex < raw.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        rawVariation
                            += std::abs(raw.pixels[pixelIndex][channel] - previousRaw.pixels[pixelIndex][channel]);
                        resolvedVariation += std::abs(
                            resolved.pixels[pixelIndex][channel] - previousResolved.pixels[pixelIndex][channel]);
                        ++comparedValues;
                    }
            }

            previousRaw = raw;
            previousResolved = std::move(resolved);
            previousMetadata = metadata;
        }

        ASSERT_GT(comparedValues, 0u);
        const double meanRawVariation = rawVariation / static_cast<double>(comparedValues);
        const double meanResolvedVariation = resolvedVariation / static_cast<double>(comparedValues);
        EXPECT_GT(meanRawVariation, 1e-5);
        EXPECT_LT(meanResolvedVariation, meanRawVariation * 0.35);
    }

    /// Verify that temporal accumulation denoises overlapping low clouds, weather clouds, and mist without shell
    /// sampling.
    TEST(AtmosphereShaderTest, mixedAtmosphericStrataConvergeWithoutPersistentGrain)
    {
        NativePassInputs volumeInputs;
        volumeInputs.width = 64;
        volumeInputs.height = 40;
        volumeInputs.cloudParameters = { 0.6f, 0.95f, 26000.f, 18000.f };
        volumeInputs.cloudShapeParameters = { 0.55f, 1.f, 0.62f, 0.08f };
        volumeInputs.lowCloudParameters = { 0.32f, 0.8f, 4200.f, 11000.f };
        volumeInputs.lowCloudShapeParameters = { 0.38f, 1.25f, 0.42f, 1.f };
        volumeInputs.mistParameters = { 0.18f, 150.f, 2800.f, 0.12f };
        volumeInputs.eyePosition = { 0.f, 0.f, 700.f };
        volumeInputs.viewDirection = { 1.f, 0.f, 0.08f };
        volumeInputs.farPlane = 300000.f;

        PassImage previousRaw;
        PassImage previousResolved;
        PassImage previousMetadata;
        double rawVariation = 0.0;
        double resolvedVariation = 0.0;
        std::size_t comparedValues = 0;

        for (int frameNumber = 0; frameNumber < 12; ++frameNumber)
        {
            volumeInputs.frameNumber = frameNumber;
            volumeInputs.historyValid = frameNumber > 0;
            const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereVolume", volumeInputs);
            ASSERT_FALSE(raw.pixels.empty());

            NativePassInputs metadataInputs = volumeInputs;
            metadataInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
            const PassImage metadata = renderNativeAtmospherePass("classifyAtmosphereVolume", metadataInputs);
            ASSERT_EQ(metadata.pixels.size(), raw.pixels.size());

            PassImage resolved = raw;
            if (frameNumber > 0)
            {
                NativePassInputs resolveInputs = volumeInputs;
                resolveInputs.historyValid = true;
                resolveInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
                resolveInputs.atmosphereHistory
                    = [&previousResolved](int x, int y) { return previousResolved.at(x, y); };
                resolveInputs.volumeMetadataInput = [&metadata](int x, int y) { return metadata.at(x, y); };
                resolveInputs.atmosphereMetadataHistory
                    = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
                resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
                ASSERT_EQ(resolved.pixels.size(), raw.pixels.size());
            }

            if (frameNumber >= 6)
            {
                for (std::size_t pixelIndex = 0; pixelIndex < raw.pixels.size(); ++pixelIndex)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        rawVariation
                            += std::abs(raw.pixels[pixelIndex][channel] - previousRaw.pixels[pixelIndex][channel]);
                        resolvedVariation += std::abs(
                            resolved.pixels[pixelIndex][channel] - previousResolved.pixels[pixelIndex][channel]);
                        ++comparedValues;
                    }
            }

            previousRaw = raw;
            previousResolved = std::move(resolved);
            previousMetadata = metadata;
        }

        ASSERT_GT(comparedValues, 0u);
        const double meanRawVariation = rawVariation / static_cast<double>(comparedValues);
        const double meanResolvedVariation = resolvedVariation / static_cast<double>(comparedValues);
        EXPECT_GT(meanRawVariation, 1e-5);
        EXPECT_LT(meanResolvedVariation, meanRawVariation * 0.2);
    }

    TEST(AtmosphereShaderTest, largeCloudAdvectionRejectsHistoryWithoutMotionSmearing)
    {
        constexpr int referenceFrameCount = 8;
        const auto renderCloudReference = [](const osg::Vec2f& cloudAdvectionOffsetWorldUnits) {
            PassImage reference;
            for (int frameNumber = 0; frameNumber < referenceFrameCount; ++frameNumber)
            {
                NativePassInputs inputs;
                inputs.cloudParameters = { 0.62f, 0.9f, 48000.f, 22000.f };
                inputs.mistParameters = {};
                inputs.cloudAdvectionOffsetWorldUnits = cloudAdvectionOffsetWorldUnits;
                inputs.frameNumber = frameNumber;
                inputs.historyValid = true;
                const PassImage sample = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
        currentInputs.cloudParameters = { 0.62f, 0.9f, 48000.f, 22000.f };
        currentInputs.mistParameters = {};
        currentInputs.cloudAdvectionOffsetWorldUnits = elapsedCloudAdvectionWorldUnits;
        currentInputs.frameNumber = referenceFrameCount + 1;
        currentInputs.historyValid = true;
        const PassImage currentRaw = renderNativeAtmospherePass("integrateAtmosphereVolume", currentInputs);
        ASSERT_EQ(currentRaw.pixels.size(), currentReference.pixels.size());

        NativePassInputs currentMetadataInputs = currentInputs;
        currentMetadataInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        const PassImage currentMetadata = renderNativeAtmospherePass("classifyAtmosphereVolume", currentMetadataInputs);
        NativePassInputs previousMetadataInputs = currentInputs;
        previousMetadataInputs.cloudAdvectionOffsetWorldUnits = {};
        previousMetadataInputs.passInput = [&previousReference](int x, int y) { return previousReference.at(x, y); };
        const PassImage previousMetadata
            = renderNativeAtmospherePass("classifyAtmosphereVolume", previousMetadataInputs);

        NativePassInputs resolveInputs = currentInputs;
        resolveInputs.cloudAdvectionDeltaWorldUnits = elapsedCloudAdvectionWorldUnits;
        resolveInputs.passInput = [&currentRaw](int x, int y) { return currentRaw.at(x, y); };
        resolveInputs.atmosphereHistory = [&previousReference](int x, int y) { return previousReference.at(x, y); };
        resolveInputs.volumeMetadataInput = [&currentMetadata](int x, int y) { return currentMetadata.at(x, y); };
        resolveInputs.atmosphereMetadataHistory
            = [&previousMetadata](int x, int y) { return previousMetadata.at(x, y); };
        const PassImage resolved = renderNativeAtmospherePass("resolveAtmosphereVolumeHistory", resolveInputs);
        ASSERT_EQ(resolved.pixels.size(), currentRaw.pixels.size());

        double historyContribution = 0.0;
        for (std::size_t pixelIndex = 0; pixelIndex < resolved.pixels.size(); ++pixelIndex)
            for (int channel = 0; channel < 3; ++channel)
            {
                historyContribution
                    += std::abs(resolved.pixels[pixelIndex][channel] - currentRaw.pixels[pixelIndex][channel]);
            }

        EXPECT_NEAR(historyContribution, 0.0, 1e-6)
            << "A large single-frame cloud displacement must reject stale history rather than smear it";
    }

    TEST(AtmosphereShaderTest, sunShaftsRespectDepthOcclusionAndBehindCameraState)
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

    TEST(AtmosphereShaderTest, compositeRetainsPhysicalBeyondFarCloudsAcrossOpenSky)
    {
        NativePassInputs volumeInputs;
        volumeInputs.width = 64;
        volumeInputs.height = 64;
        volumeInputs.cloudParameters = { 0.72f, 1.1f, 55000.f, 18000.f };
        volumeInputs.mistParameters = {};
        const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereVolume", volumeInputs);
        ASSERT_FALSE(raw.pixels.empty());
        NativePassInputs metadataInputs = volumeInputs;
        metadataInputs.passInput = [&raw](int x, int y) { return raw.at(x, y); };
        const PassImage metadata = renderNativeAtmospherePass("classifyAtmosphereVolume", metadataInputs);
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

        const PassImage raw = renderNativeAtmospherePass("integrateAtmosphereVolume", inputs);
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
