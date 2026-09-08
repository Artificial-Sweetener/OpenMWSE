#include <gtest/gtest.h>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/color.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Group>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Texture3D>
#include <osg/Uniform>
#include <osg/Viewport>
#include <osgDB/ReadFile>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgViewer/Viewer>

#include "apps/openmw/mwrender/atmosphere.hpp"
#include "apps/openmw/mwrender/skyutil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace MWRender
{
    namespace
    {
        constexpr int captureSize = 128;

        /// Load the repository-owned cloud noise volume used by production compatibility shaders.
        osg::ref_ptr<osg::Texture3D> loadAtmosphereNoiseTexture()
        {
            const std::filesystem::path path = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files" / "data"
                / "textures" / "openmwse" / "atmosphere_cloud_noise.dds";
            osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(path.string());
            if (!image)
                throw std::runtime_error("Unable to load production atmosphere cloud noise texture");
            osg::ref_ptr<osg::Texture3D> texture = new osg::Texture3D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);
            return texture;
        }

        /// Load the repository-owned detail erosion volume used by production compatibility shaders.
        osg::ref_ptr<osg::Texture3D> loadAtmosphereDetailNoiseTexture()
        {
            const std::filesystem::path path = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files" / "data"
                / "textures" / "openmwse" / "atmosphere_cloud_detail.dds";
            osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(path.string());
            if (!image)
                throw std::runtime_error("Unable to load production atmosphere cloud detail texture");
            osg::ref_ptr<osg::Texture3D> texture = new osg::Texture3D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);
            return texture;
        }

        /// Load the repository-owned curl field used to distort cloud edges without disconnecting their base density.
        osg::ref_ptr<osg::Texture2D> loadAtmosphereCurlNoiseTexture()
        {
            const std::filesystem::path path = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files" / "data"
                / "textures" / "openmwse" / "atmosphere_cloud_curl.dds";
            osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(path.string());
            if (!image)
                throw std::runtime_error("Unable to load production atmosphere cloud curl texture");
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            return texture;
        }

        /// Load the repository-owned broad weather field used by production compatibility shaders.
        osg::ref_ptr<osg::Texture2D> loadAtmosphereWeatherTexture()
        {
            const std::filesystem::path path = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files" / "data"
                / "textures" / "openmwse" / "atmosphere_cloud_weather.dds";
            osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(path.string());
            if (!image)
                throw std::runtime_error("Unable to load production atmosphere cloud weather texture");
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            return texture;
        }

        /// Pair a deterministic sky sample with its world-space direction.
        struct AtmosphereSample
        {
            osg::Vec3f mDirection;
        };

        /// Represent one scene-linear RGB sample captured by the sky harness.
        struct CapturedColor
        {
            float mRed;
            float mGreen;
            float mBlue;
        };

        /// Own scene-linear pixels captured from the compatibility sky harness.
        struct CapturedImage
        {
            int mWidth;
            int mHeight;
            std::vector<CapturedColor> mPixels;

            /// Return the captured pixel at the requested image coordinate.
            const CapturedColor& at(int x, int y) const { return mPixels[static_cast<std::size_t>(y * mWidth + x)]; }
        };

        /// Interpolate independent reference colors without production shader code.
        CapturedColor mix(const CapturedColor& from, const CapturedColor& to, float factor)
        {
            return { std::lerp(from.mRed, to.mRed, factor), std::lerp(from.mGreen, to.mGreen, factor),
                std::lerp(from.mBlue, to.mBlue, factor) };
        }

        /// Evaluate the GLSL smoothstep transfer function for the independent CPU reference.
        float smoothstep(float lower, float upper, float value)
        {
            const float normalized = std::clamp((value - lower) / (upper - lower), 0.f, 1.f);
            return normalized * normalized * (3.f - 2.f * normalized);
        }

        /// Create a filtered stock-like star texture with dark translucent edge texels around a bright core.
        osg::ref_ptr<osg::Texture2D> createFilteredStarTexture(const osg::Vec3f& coreColor = { 1.f, 0.92f, 0.78f })
        {
            constexpr int textureSize = 16;
            constexpr int starCenterX = textureSize / 4;
            constexpr int starCenterY = textureSize / 2;
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(textureSize, textureSize, 1, GL_RGBA, GL_FLOAT);
            for (int y = 0; y < textureSize; ++y)
                for (int x = 0; x < textureSize; ++x)
                {
                    const float distance
                        = std::hypot(static_cast<float>(x - starCenterX), static_cast<float>(y - starCenterY));
                    osg::Vec4f texel(0.f, 0.f, 0.f, 0.f);
                    if (distance < 0.75f)
                        texel = osg::Vec4f(coreColor, 1.f);
                    else if (distance < 1.75f)
                        texel = { 0.035f, 0.04f, 0.05f, 0.48f };
                    else if (distance < 2.75f)
                        texel = { 0.01f, 0.012f, 0.016f, 0.18f };
                    float* destination = reinterpret_cast<float*>(image->data(x, y));
                    std::copy(texel.ptr(), texel.ptr() + 4, destination);
                }

            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setInternalFormat(GL_RGBA16F);
            texture->setResizeNonPowerOfTwoHint(false);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            return texture;
        }

        /// Evaluate the independently transcribed directional atmosphere reference for one direction.
        CapturedColor referenceAtmosphereSky(const osg::Vec3f& requestedDirection)
        {
            constexpr CapturedColor skyColor{ 0.232298f, 0.361908f, 0.519572f };
            constexpr CapturedColor fogColor{ 0.887052f, 0.638956f, 0.517662f };
            constexpr CapturedColor farSkyColor{ 0.371f, 0.637f, 1.108f };
            constexpr CapturedColor inScattering{ 0.25f, 0.38f, 0.48f };
            constexpr CapturedColor outScattering{ 0.07f, 0.36f, 0.76f };

            osg::Vec3f direction = requestedDirection;
            direction.normalize();
            osg::Vec3f sunDirection(1.f, 0.f, 0.08f);
            sunDirection.normalize();
            const float skyAmount = 1.f
                - std::pow(std::clamp(1.f - 2.22f * std::clamp(direction.z() - 0.075f, 0.f, 1.f), 0.f, 1.f), 1.15f);
            const CapturedColor directionalSky = mix(fogColor, skyColor, skyAmount);
            const float sunCosine = direction * sunDirection;
            const float altitude = std::pow(1.f + sunDirection.z(), 10.f);
            const float altitudeBrightness = std::clamp(1.f - std::exp2(-1.9f * altitude), 0.f, 1.f);
            const float horizonSun
                = std::clamp(std::exp(-4.f * sunDirection.z()), 0.f, 1.f) * std::clamp(altitude, 0.f, 1.f);
            const float forwardScatter = (1.58f / std::max(1.24f - sunCosine, 0.02f)) * horizonSun;
            const float diffuseScatter = 1.f - 0.09f * forwardScatter;
            const float atmosphereDepth = 1.33f * std::exp(-2.f * std::clamp(direction.z(), 0.f, 1.f));
            const CapturedColor directionalScatter = mix(inScattering, outScattering, 0.5f * (1.f + sunCosine));
            const float altitudeScale = 2.8f + 4.3f / std::max(altitude, 1e-4f);
            const CapturedColor attenuation{
                atmosphereDepth * directionalScatter.mRed * (altitudeScale + 0.7f * forwardScatter),
                atmosphereDepth * directionalScatter.mGreen * (altitudeScale + 0.7f * forwardScatter),
                atmosphereDepth * directionalScatter.mBlue * (altitudeScale + 0.7f * forwardScatter),
            };
            const CapturedColor integrated{
                (1.f - std::exp(-attenuation.mRed)) / std::max(attenuation.mRed, 1e-4f),
                (1.f - std::exp(-attenuation.mGreen)) / std::max(attenuation.mGreen, 1e-4f),
                (1.f - std::exp(-attenuation.mBlue)) / std::max(attenuation.mBlue, 1e-4f),
            };
            const CapturedColor farSky = mix(skyColor, farSkyColor, 0.44f);
            const CapturedColor scattered{
                0.125f * forwardScatter + farSky.mRed * diffuseScatter,
                0.125f * forwardScatter + farSky.mGreen * diffuseScatter,
                0.125f * forwardScatter + farSky.mBlue * diffuseScatter,
            };
            const float depthBrightness = 1.17f * atmosphereDepth + 0.89f;
            const CapturedColor result{
                scattered.mRed * integrated.mRed * depthBrightness * altitudeBrightness,
                scattered.mGreen * integrated.mGreen * depthBrightness * altitudeBrightness,
                scattered.mBlue * integrated.mBlue * depthBrightness * altitudeBrightness,
            };
            const float integratedLuminance = 0.2126f * result.mRed + 0.7152f * result.mGreen + 0.0722f * result.mBlue;
            const float diffuseLuminance = 0.2126f * farSky.mRed + 0.7152f * farSky.mGreen + 0.0722f * farSky.mBlue;
            const CapturedColor upperSky{
                farSky.mRed * integratedLuminance / diffuseLuminance,
                farSky.mGreen * integratedLuminance / diffuseLuminance,
                farSky.mBlue * integratedLuminance / diffuseLuminance,
            };
            const float elevation = std::max(direction.z(), 0.f);
            const CapturedColor weatherBlended
                = mix(directionalSky, mix(result, upperSky, -std::expm1(-20.f * elevation * elevation)), 1.f);

            const float sunElevationDegrees = osg::RadiansToDegrees(std::asin(std::clamp(sunDirection.z(), -1.f, 1.f)));
            const float airMassDenominator
                = sunDirection.z() + 0.50572f * std::pow(std::max(sunElevationDegrees + 6.07995f, 0.05f), -1.6364f);
            const float relativeAirMass = std::clamp(1.f / std::max(airMassDenominator, 0.035f), 1.f, 30.f);
            const CapturedColor longPathTransmittance{
                std::exp(-relativeAirMass * 0.018f),
                std::exp(-relativeAirMass * 0.041f),
                std::exp(-relativeAirMass * 0.095f),
            };
            const float twilight
                = smoothstep(-0.18f, 0.02f, sunDirection.z()) * (1.f - smoothstep(0.06f, 0.34f, sunDirection.z()));
            const float horizonBand = std::exp(-44.1786467f * direction.z() * direction.z());
            const float solarAfterglow = std::pow(std::clamp(0.5f + 0.5f * sunCosine, 0.f, 1.f), 3.f);
            const float antiSolarArch = std::pow(std::clamp(0.5f - 0.5f * sunCosine, 0.f, 1.f), 2.f);
            const CapturedColor sunsetRadiance{
                std::lerp(1.22f, 1.f, longPathTransmittance.mRed) * solarAfterglow,
                std::lerp(0.24f, 0.62f, longPathTransmittance.mGreen) * solarAfterglow,
                std::lerp(0.055f, 0.24f, longPathTransmittance.mBlue) * solarAfterglow,
            };
            const CapturedColor twilightArch{
                0.22f * antiSolarArch,
                0.12f * antiSolarArch,
                0.34f * antiSolarArch,
            };
            const float twilightWeight = twilight * horizonBand;
            return {
                weatherBlended.mRed + (sunsetRadiance.mRed + twilightArch.mRed) * twilightWeight,
                weatherBlended.mGreen + (sunsetRadiance.mGreen + twilightArch.mGreen) * twilightWeight,
                weatherBlended.mBlue + (sunsetRadiance.mBlue + twilightArch.mBlue) * twilightWeight,
            };
        }

        /// Compare a captured sky sample with the independent directional reference.
        void expectAtmosphereReference(const CapturedColor& actual, const osg::Vec3f& direction)
        {
            const CapturedColor expected = referenceAtmosphereSky(direction);
            constexpr float tolerance = 0.035f;
            /// Evaluate the reference highlight shoulder from its asymptote and preserved low-range slope.
            const auto sceneChannel = [](float value) { return value <= 0.8f ? value : 1.f - 0.04f / (value - 0.6f); };
            EXPECT_NEAR(actual.mRed, sceneChannel(expected.mRed), tolerance);
            EXPECT_NEAR(actual.mGreen, sceneChannel(expected.mGreen), tolerance);
            EXPECT_NEAR(actual.mBlue, sceneChannel(expected.mBlue), tolerance);
        }

        /// Return the production-compatible shader permutation used by the sky harness.
        Shader::ShaderManager::DefineMap getAtmosphereHarnessDefines()
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
            defines["skyBlending"] = "1";
            for (const std::string_view name :
                { "additiveBlending", "blendMap", "bumpMap", "darkMap", "decalMap", "detailMap", "diffuseMap",
                    "diffuseParallax", "emissiveMap", "envMap", "glossMap", "normalMap", "parallax",
                    "particleOcclusion", "reconstructNormalZ", "softParticles", "specularMap", "shadows_enabled",
                    "simpleLighting", "adjustCoverage", "alphaToCoverage", "alphaFunc", "useShadowDebugOverlay",
                    "perspectiveShadowMaps", "disableNormalOffsetShadows", "limitShadowMapDistance" })
                defines[std::string(name)] = "0";
            return defines;
        }

        /// Create a closed deterministic dome that exposes every sky direction to the harness.
        osg::ref_ptr<osg::Node> createHarnessDome()
        {
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            osg::ref_ptr<osg::Vec2Array> textureCoordinates = new osg::Vec2Array;

            constexpr int longitudeSegments = 64;
            constexpr int latitudeSegments = 32;
            constexpr float radius = 5000.f;
            for (int latitude = 0; latitude <= latitudeSegments; ++latitude)
            {
                const float elevation = -osg::PI_2 + static_cast<float>(latitude) / latitudeSegments * osg::PI;
                for (int longitude = 0; longitude <= longitudeSegments; ++longitude)
                {
                    const float azimuth = static_cast<float>(longitude) / longitudeSegments * 2.f * osg::PI;
                    vertices->push_back({ radius * std::cos(elevation) * std::cos(azimuth),
                        radius * std::cos(elevation) * std::sin(azimuth), radius * std::sin(elevation) });
                    colors->push_back({ 1.f, 1.f, 1.f, 1.f });
                    textureCoordinates->push_back({ static_cast<float>(longitude) / longitudeSegments,
                        static_cast<float>(latitude) / latitudeSegments });
                }
            }

            geometry->setVertexArray(vertices);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->setTexCoordArray(0, textureCoordinates);
            for (int latitude = 0; latitude < latitudeSegments; ++latitude)
            {
                osg::ref_ptr<osg::DrawElementsUShort> strip
                    = new osg::DrawElementsUShort(osg::PrimitiveSet::TRIANGLE_STRIP);
                for (int longitude = 0; longitude <= longitudeSegments; ++longitude)
                {
                    strip->push_back(static_cast<unsigned short>((latitude + 1) * (longitudeSegments + 1) + longitude));
                    strip->push_back(static_cast<unsigned short>(latitude * (longitudeSegments + 1) + longitude));
                }
                geometry->addPrimitiveSet(strip);
            }
            geode->addDrawable(geometry);
            return geode;
        }

        /// Create a stock-style open horizon band for sky-coverage regression tests.
        osg::ref_ptr<osg::Node> createStockStyleHorizonBand()
        {
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            constexpr int segments = 16;
            constexpr float horizonRadius = 1600.f;
            constexpr float upperRadius = 1460.f;
            for (int segment = 0; segment < segments; ++segment)
            {
                const float azimuth = static_cast<float>(segment) / segments * 2.f * osg::PI;
                const float cosine = std::cos(azimuth);
                const float sine = std::sin(azimuth);
                vertices->push_back({ upperRadius * cosine, upperRadius * sine, 800.f });
                vertices->push_back({ horizonRadius * cosine, horizonRadius * sine, 100.f });
            }
            geometry->setVertexArray(vertices);
            osg::ref_ptr<osg::DrawElementsUShort> strip
                = new osg::DrawElementsUShort(osg::PrimitiveSet::TRIANGLE_STRIP);
            for (std::size_t index = 0; index < vertices->size(); ++index)
                strip->push_back(static_cast<unsigned short>(index));
            strip->push_back(0);
            strip->push_back(1);
            geometry->addPrimitiveSet(strip);
            geode->addDrawable(geometry);
            return geode;
        }

        /// Return whether the supplied sky geometry covers a requested viewing direction.
        bool intersectsDome(osg::Node& dome, const osg::Vec3f& requestedDirection)
        {
            osg::Vec3f direction = requestedDirection;
            direction.normalize();
            osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector
                = new osgUtil::LineSegmentIntersector(osg::Vec3f(), direction * 10000.f);
            osgUtil::IntersectionVisitor visitor(intersector);
            dome.accept(visitor);
            return intersector->containsIntersections();
        }

        /// Execute production compatibility sky shaders in a deterministic off-screen graphics context.
        class AtmosphereRenderHarness
        {
        public:
            /// Create a configurable sky capture or timing context around the supplied dome geometry.
            explicit AtmosphereRenderHarness(osg::ref_ptr<osg::Node> dome, int width = captureSize,
                int height = captureSize, double verticalFov = 70.0, double projectionAspectRatio = 0.0)
                : mWidth(width)
                , mHeight(height)
            {
                osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
                traits->width = width;
                traits->height = height;
                traits->pbuffer = true;
                traits->doubleBuffer = false;
                traits->glContextVersion = "2.1";
                traits->glContextProfileMask = 0;
                mContext = osg::GraphicsContext::createGraphicsContext(traits);
                if (!mContext || !mContext->realize())
                    return;

                mViewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
                osg::Camera* camera = mViewer.getCamera();
                camera->setGraphicsContext(mContext);
                camera->setViewport(new osg::Viewport(0, 0, width, height));
                const double aspectRatio
                    = projectionAspectRatio > 0.0 ? projectionAspectRatio : static_cast<double>(width) / height;
                camera->setProjectionMatrixAsPerspective(verticalFov, aspectRatio, 1.0, 20000.0);
                camera->setClearColor(osg::Vec4(0.f, 0.f, 0.f, 1.f));
                camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
                mCapture = new osg::Image;
                mCapture->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
                mCapture->setInternalTextureFormat(GL_RGBA16F);
                camera->attach(osg::Camera::COLOR_BUFFER, mCapture);

                mRoot = new osg::Group;
                mSimulationTime = new osg::Uniform("osg_SimulationTime", 0.f);
                mRoot->getOrCreateStateSet()->addUniform(
                    mSimulationTime, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                osg::ref_ptr<osg::Group> sceneRoot = new osg::Group;
                mCameraRelative = new CameraRelativeTransform;
                mReflection = new osg::Uniform("isReflection", false);
                mCameraRelative->getOrCreateStateSet()->addUniform(
                    mReflection, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                mCameraRelative->addChild(dome);
                sceneRoot->addChild(mCameraRelative);
                mRoot->addChild(sceneRoot);

                osg::ref_ptr<SceneUtil::SharedUniformStateUpdater> sharedState
                    = new SceneUtil::SharedUniformStateUpdater(0.8f);
                sharedState->setNear(1.f);
                sharedState->setFar(20000.f);
                sharedState->setScreenRes(static_cast<float>(width), static_cast<float>(height));
                mRoot->addUpdateCallback(sharedState);

                osg::ref_ptr<SceneUtil::PerViewUniformStateUpdater> perViewState
                    = new SceneUtil::PerViewUniformStateUpdater(nullptr);
                perViewState->setProjectionMatrix(static_cast<osg::Matrixf>(camera->getProjectionMatrix()));
                mRoot->addCullCallback(perViewState);

                mAtmosphereState
                    = new SceneUtil::StateUpdater(loadAtmosphereNoiseTexture(), 14, loadAtmosphereDetailNoiseTexture(),
                        15, loadAtmosphereCurlNoiseTexture(), 16, loadAtmosphereWeatherTexture(), 17);
                mAtmosphereState->setFogColor({ 0.887052f, 0.638956f, 0.517662f, 1.f });
                mAtmosphereState->setFogStart(1000.f);
                mAtmosphereState->setFogEnd(20000.f);
                mAtmosphereState->setUnderwaterFogColor({ 0.f, 0.f, 0.f, 1.f });
                mAtmosphereState->setUnderwaterFogStart(1000.f);
                mAtmosphereState->setUnderwaterFogEnd(20000.f);
                applyAtmosphereState();
                sceneRoot->addUpdateCallback(mAtmosphereState);

                Shader::ShaderManager manager;
                manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
                auto defines = getAtmosphereHarnessDefines();
                manager.setGlobalDefines(defines);
                mProgram = manager.getProgram("sky");
                if (!mProgram)
                    return;

                osg::StateSet* state = mCameraRelative->getOrCreateStateSet();
                state->setAttributeAndModes(
                    mProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                state->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                state->addUniform(new osg::Uniform("pass", 0), osg::StateAttribute::OVERRIDE);
                state->addUniform(new osg::Uniform("diffuseColor", osg::Vec4f(1.f, 1.f, 1.f, 1.f)));

                mViewer.setSceneData(mRoot);
                mViewer.realize();
                mValid = mContext->makeCurrent();
            }

            /// Release the off-screen graphics context owned by the harness.
            ~AtmosphereRenderHarness()
            {
                if (mContext)
                {
                    mContext->releaseContext();
                    mContext->close();
                }
            }

            /// Return whether the off-screen graphics context and production sky program initialized successfully.
            bool isValid() const { return mValid; }

            /// Set deterministic integrated world-space cloud displacement.
            void setCloudAdvectionOffset(const osg::Vec2f& offsetWorldUnits)
            {
                mCloudAdvectionOffsetWorldUnits = offsetWorldUnits;
                applyAtmosphereState();
            }

            /// Set the physical sun direction and analytic disc radiance used by the harness.
            void setSunDirection(
                const osg::Vec3f& direction, const osg::Vec4f& sunDiscColor = { 1.f, 0.85f, 0.65f, 0.f })
            {
                mSunDirection = normalized(direction);
                mSunDiscColor = sunDiscColor;
                applyAtmosphereState();
            }

            /// Set the canonical scene-linear illumination consumed by sky, cloud, and aerial scattering.
            void setAtmosphereLighting(
                const osg::Vec3f& solarRadiance, const osg::Vec3f& skyIrradiance, const osg::Vec3f& groundIrradiance)
            {
                mSolarRadiance = solarRadiance;
                mSkyIrradiance = skyIrradiance;
                mGroundIrradiance = groundIrradiance;
                applyAtmosphereState();
            }

            /// Set weather volume controls and quality without changing canonical scattering colors.
            void setAtmosphereVolumes(const osg::Vec4f& cloudParameters, const osg::Vec4f& mistParameters,
                const osg::Vec4f& sunDiscColor, int quality = 2)
            {
                mCloudParameters = cloudParameters;
                mMistParameters = mistParameters;
                mSunDiscColor = sunDiscColor;
                mQuality = quality;
                applyAtmosphereState();
            }

            /// Set the path-atmosphere range used to verify cloud visibility and distance convergence independently.
            void setFogRange(float start, float end)
            {
                mAtmosphereState->setFogStart(start);
                mAtmosphereState->setFogEnd(end);
            }

            /// Capture the main-view sky for a world-space viewing direction.
            CapturedImage renderImage(const osg::Vec3f& direction)
            {
                const osg::Vec3f up
                    = std::abs(direction.z()) > 0.99f ? osg::Vec3f(0.f, 1.f, 0.f) : osg::Vec3f(0.f, 0.f, 1.f);
                return renderImageFrom(osg::Vec3f(), direction, up, false);
            }

            /// Composite the production night-dome shader over a controlled scene-linear sky background.
            CapturedImage renderNightStars(osg::Texture2D& starTexture, const osg::Vec4f& background, float opacity,
                AtmosphereNightUpdater::StarBlendMode blendMode
                = AtmosphereNightUpdater::StarBlendMode::EmissiveRadiance)
            {
                mViewer.getCamera()->setClearColor(background);
                osg::StateSet* state = mCameraRelative->getOrCreateStateSet();
                state->setMode(GL_BLEND, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                state->setAttributeAndModes(
                    new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA, osg::BlendFunc::ONE_MINUS_SRC_ALPHA,
                        osg::BlendFunc::ZERO, osg::BlendFunc::ONE),
                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                state->setTextureAttributeAndModes(
                    0, &starTexture, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                state->addUniform(new osg::Uniform("diffuseMap", 0), osg::StateAttribute::OVERRIDE);
                osg::ref_ptr<AtmosphereNightUpdater> updater = new AtmosphereNightUpdater(blendMode);
                updater->setFade(opacity);
                mCameraRelative->addUpdateCallback(updater);
                return renderImage(osg::Vec3f(0.f, 1.f, 0.f));
            }

            /// Capture the water-reflected sky from a world-space eye position and direction.
            CapturedImage renderReflectedImage(
                const osg::Vec3f& eyePosition, const osg::Vec3f& direction, float waterHeight = 0.f)
            {
                mAtmosphereState->setWaterHeight(waterHeight);
                const osg::Matrixf view
                    = osg::Matrixf::lookAt(eyePosition, eyePosition + direction, osg::Vec3f(0.f, 0.f, 1.f));
                const osg::Matrixf reflection
                    = osg::Matrixf::scale(1.f, 1.f, -1.f) * osg::Matrixf::translate(0.f, 0.f, 2.f * waterHeight);
                return renderImageWithViewMatrix(view * reflection, true);
            }

            /// Capture the sky from an explicit eye, direction, up vector, and reflection state.
            CapturedImage renderImageFrom(
                const osg::Vec3f& eyePosition, const osg::Vec3f& direction, const osg::Vec3f& up, bool reflection)
            {
                return renderImageWithViewMatrix(
                    osg::Matrixf::lookAt(eyePosition, eyePosition + direction, up), reflection);
            }

            /// Capture the sky using an explicit view matrix and reflection shader permutation.
            CapturedImage renderImageWithViewMatrix(const osg::Matrixf& viewMatrix, bool reflection)
            {
                mReflection->set(reflection);
                mViewer.getCamera()->setViewMatrix(viewMatrix);
                mViewer.frame();

                CapturedImage result{ mWidth, mHeight, {} };
                result.mPixels.reserve(static_cast<std::size_t>(mWidth * mHeight));
                for (int y = 0; y < mHeight; ++y)
                {
                    for (int x = 0; x < mWidth; ++x)
                    {
                        const float* color = reinterpret_cast<const float*>(mCapture->data(x, y));
                        result.mPixels.push_back({ color[0], color[1], color[2] });
                    }
                }
                return result;
            }

            /// Return the center pixel from a main-view sky capture.
            CapturedColor render(const osg::Vec3f& direction)
            {
                const CapturedImage image = renderImage(direction);
                return image.at(mWidth / 2, mHeight / 2);
            }

        private:
            /// Return a unit-length test direction.
            static osg::Vec3f normalized(const osg::Vec3f& value) { return value / value.length(); }

            /// Publish the harness's current scene state through the production scene-uniform owner.
            void applyAtmosphereState()
            {
                mAtmosphereState->setAtmosphere(true, { 0.232298f, 0.361908f, 0.519572f, 1.f }, { 0.25f, 0.38f, 0.48f },
                    { 0.07f, 0.36f, 0.76f }, 1.f, mSunDirection, mCloudParameters, {}, {}, {}, 0.f, mMistParameters, {},
                    {}, {}, mSunDiscColor, mSolarRadiance, mSkyIrradiance, mGroundIrradiance,
                    mCloudAdvectionOffsetWorldUnits, mQuality);
            }

            osgViewer::Viewer mViewer;
            osg::ref_ptr<osg::GraphicsContext> mContext;
            osg::ref_ptr<osg::Group> mRoot;
            osg::ref_ptr<CameraRelativeTransform> mCameraRelative;
            osg::ref_ptr<SceneUtil::StateUpdater> mAtmosphereState;
            osg::ref_ptr<osg::Uniform> mReflection;
            osg::ref_ptr<osg::Uniform> mSimulationTime;
            osg::ref_ptr<osg::Image> mCapture;
            osg::ref_ptr<osg::Program> mProgram;
            osg::Vec3f mSunDirection = normalized({ 1.f, 0.f, 0.08f });
            osg::Vec4f mCloudParameters;
            osg::Vec4f mMistParameters;
            osg::Vec4f mSunDiscColor{ 1.f, 0.85f, 0.65f, 0.f };
            osg::Vec3f mSolarRadiance{ 1.f, 0.85f, 0.65f };
            osg::Vec3f mSkyIrradiance{ 0.3f, 0.4f, 0.55f };
            osg::Vec3f mGroundIrradiance{ 0.45f, 0.5f, 0.6f };
            osg::Vec2f mCloudAdvectionOffsetWorldUnits;
            int mWidth;
            int mHeight;
            int mQuality = 2;
            bool mValid = false;
        };

        /// Return the explicitly configured external-data directory for optional stock-mesh tests.
        std::optional<std::filesystem::path> getExternalAtmosphereDataDirectory()
        {
            char* path = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&path, &size, "OPENMW_ATMOSPHERE_TEST_DATA") != 0 || path == nullptr)
                return std::nullopt;
            const std::filesystem::path result(path);
            std::free(path);
            return result;
        }

        /// Return ordered external data directories used to reproduce production foliage materials.
        std::vector<std::filesystem::path> getExternalFoliageDataDirectories()
        {
            char* value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, "OPENMW_FOLIAGE_TEST_DATA") != 0 || value == nullptr)
                return {};

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
            return directories;
        }

        /// Summarize the effective alpha and shader states used by loaded production foliage geometry.
        struct FoliageMaterialSummary
        {
            std::size_t mGeometryCount = 0;
            std::size_t mBlendedGeometryCount = 0;
            std::size_t mAlphaTestedGeometryCount = 0;
            std::size_t mCoverageAdjustedGeometryCount = 0;
            std::unordered_set<std::string> mTextureNames;
        };

        /// Inspect inherited render state for every drawable in a production-loaded tree.
        class FoliageMaterialVisitor : public osg::NodeVisitor
        {
        public:
            /// Traverse every drawable and include its local state in the effective material state.
            FoliageMaterialVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            /// Record effective alpha, texture, and shader state for one foliage geometry.
            void apply(osg::Geometry& geometry) override
            {
                ++mSummary.mGeometryCount;
                bool blended = false;
                bool alphaTested = false;
                bool coverageAdjusted = false;
                std::vector<const osg::StateSet*> stateSets;
                stateSets.reserve(getNodePath().size() + 1);
                for (const osg::Node* node : getNodePath())
                    if (node->getStateSet())
                        stateSets.push_back(node->getStateSet());
                if (geometry.getStateSet())
                    stateSets.push_back(geometry.getStateSet());

                for (const osg::StateSet* stateSet : stateSets)
                {
                    const osg::StateAttribute::GLModeValue blendMode = stateSet->getMode(GL_BLEND);
                    if (blendMode != osg::StateAttribute::INHERIT)
                        blended = (blendMode & osg::StateAttribute::ON) != 0;
                    if (const auto* alphaFunc
                        = dynamic_cast<const osg::AlphaFunc*>(stateSet->getAttribute(osg::StateAttribute::ALPHAFUNC)))
                        alphaTested = alphaFunc->getFunction() != osg::AlphaFunc::ALWAYS;
                    if (const auto* texture = dynamic_cast<const osg::Texture2D*>(
                            stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE)))
                    {
                        if (const osg::Image* image = texture->getImage())
                            mSummary.mTextureNames.insert(image->getFileName());
                    }
                    if (const auto* program
                        = dynamic_cast<const osg::Program*>(stateSet->getAttribute(osg::StateAttribute::PROGRAM)))
                    {
                        for (unsigned int index = 0; index < program->getNumShaders(); ++index)
                        {
                            const std::string& source = program->getShader(index)->getShaderSource();
                            const std::size_t function = source.find("float coveragePreservingAlphaScale");
                            if (function != std::string::npos
                                && source.substr(function, 400).find("#if 1") != std::string::npos)
                                coverageAdjusted = true;
                        }
                    }
                }

                mSummary.mBlendedGeometryCount += blended;
                mSummary.mAlphaTestedGeometryCount += alphaTested;
                mSummary.mCoverageAdjustedGeometryCount += coverageAdjusted;
            }

            FoliageMaterialSummary mSummary;
        };

        /// Load one production foliage model through the normal VFS, resource, and shader owners.
        osg::ref_ptr<osg::Node> loadVfsFoliageModel(const std::vector<std::filesystem::path>& dataDirectories,
            VFS::Path::NormalizedView modelPath, bool adjustCoverage = true)
        {
            VFS::Manager vfs;
            for (const std::filesystem::path& directory : dataDirectories)
                vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(directory));
            vfs.buildIndex();
            if (!vfs.exists(modelPath))
                return nullptr;

            Resource::ResourceSystem resources(&vfs, 0.0, nullptr);
            Resource::SceneManager* sceneManager = resources.getSceneManager();
            sceneManager->setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
            sceneManager->setFilterSettings("linear", "linear", "linear", 16.f);
            sceneManager->setAdjustCoverageForAlphaTest(adjustCoverage);
            auto defines = getAtmosphereHarnessDefines();
            sceneManager->getShaderManager().setGlobalDefines(defines);
            return sceneManager->getInstance(modelPath);
        }

        /// Own color and depth captured immediately after production foliage shading.
        struct FoliagePassCapture
        {
            int mWidth = 0;
            int mHeight = 0;
            osg::Vec3f mEyePosition;
            osg::Vec3f mViewDirection;
            float mVerticalFov = 70.f;
            float mNearPlane = 1.f;
            float mFarPlane = 229376.f;
            float mFogStart = 0.f;
            float mFogEnd = 229376.f;
            osg::Vec4f mFogColor;
            std::vector<osg::Vec4f> mColors;
            std::vector<float> mDepths;
        };

        /// Describe the effective production fog range and color applied to a foliage capture.
        struct FoliageFogState
        {
            float mStart = 1228.8f;
            float mEnd = 99532.8f;
            osg::Vec4f mColor{ 0.34f, 0.29f, 0.27f, 1.f };
        };

        /// Render a production-loaded tree through its effective object shader before post-processing.
        FoliagePassCapture renderProductionFoliagePass(
            osg::Node& tree, float distance, float azimuthDegrees = 0.f, FoliageFogState fog = {})
        {
            constexpr int width = 1280;
            constexpr int height = 720;
            constexpr float nearPlane = 1.f;
            constexpr float farPlane = 229376.f;

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
            viewer.setLightingMode(osgViewer::View::NO_LIGHT);
            osg::Camera* camera = viewer.getCamera();
            camera->setGraphicsContext(context);
            camera->setViewport(new osg::Viewport(0, 0, width, height));
            camera->setProjectionMatrixAsPerspective(70.0, static_cast<double>(width) / height, nearPlane, farPlane);
            const float azimuth = osg::DegreesToRadians(azimuthDegrees);
            const osg::Vec3f eye(-std::cos(azimuth) * distance, -std::sin(azimuth) * distance, 0.f);
            const osg::Vec3f viewDirection = -eye / eye.length();
            camera->setViewMatrix(osg::Matrixf::lookAt(eye, osg::Vec3f(), osg::Vec3f(0.f, 0.f, 1.f)));
            camera->setClearColor(osg::Vec4f(fog.mColor.x(), fog.mColor.y(), fog.mColor.z(), 0.f));
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            camera->setCullingMode(0);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);

            osg::ref_ptr<osg::Image> colorCapture = new osg::Image;
            colorCapture->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);
            colorCapture->setInternalTextureFormat(GL_RGBA16F);
            camera->attach(osg::Camera::COLOR_BUFFER, colorCapture);
            osg::ref_ptr<osg::Image> depthCapture = new osg::Image;
            depthCapture->allocateImage(width, height, 1, GL_DEPTH_COMPONENT, GL_FLOAT);
            depthCapture->setInternalTextureFormat(GL_DEPTH_COMPONENT32F);
            camera->attach(osg::Camera::DEPTH_BUFFER, depthCapture);

            const osg::BoundingSphere bound = tree.getBound();
            osg::ref_ptr<osg::MatrixTransform> centeredTree = new osg::MatrixTransform;
            centeredTree->setMatrix(osg::Matrixf::translate(-bound.center()));
            centeredTree->addChild(&tree);
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(centeredTree);
            osg::StateSet* state = root->getOrCreateStateSet();
            state->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
            state->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
            state->addUniform(
                new osg::Uniform("projectionMatrix", static_cast<osg::Matrixf>(camera->getProjectionMatrix())));
            state->addUniform(new osg::Uniform("near", nearPlane));
            state->addUniform(new osg::Uniform("far", farPlane));
            state->addUniform(new osg::Uniform("skyBlendingStart", farPlane * 0.8f));
            state->addUniform(new osg::Uniform("screenRes", osg::Vec2f(width, height)));
            state->addUniform(new osg::Uniform("isReflection", false));
            state->addUniform(new osg::Uniform("windSpeed", 0.f));
            state->addUniform(new osg::Uniform("playerPos", osg::Vec3f()));
            state->addUniform(new osg::Uniform("useTreeAnim", false));
            state->addUniform(new osg::Uniform("osg_SimulationTime", 0.f));
            state->addUniform(new osg::Uniform("distortionStrength", 0.f));
            state->addUniform(new osg::Uniform("alpha", 1.f));
            state->addUniform(new osg::Uniform("actorFade", 1.f));
            state->addUniform(new osg::Uniform("sun.position", osg::Vec4f(0.f, -0.3f, 0.95f, 0.f)));
            state->addUniform(new osg::Uniform("sun.diffuse", osg::Vec4f(0.8f, 0.68f, 0.6f, 1.f)));
            state->addUniform(new osg::Uniform("sun.ambient", osg::Vec4f(0.22f, 0.2f, 0.19f, 1.f)));
            state->addUniform(new osg::Uniform("sun.specular", osg::Vec4f()));

            constexpr int skyTextureUnit = 15;
            osg::ref_ptr<osg::Image> skyImage = new osg::Image;
            skyImage->allocateImage(1, 1, 1, GL_RGBA, GL_FLOAT);
            std::copy(fog.mColor.ptr(), fog.mColor.ptr() + 4, reinterpret_cast<float*>(skyImage->data()));
            osg::ref_ptr<osg::Texture2D> skyTexture = new osg::Texture2D(skyImage);
            skyTexture->setInternalFormat(GL_RGBA16F);
            state->setTextureAttributeAndModes(skyTextureUnit, skyTexture, osg::StateAttribute::ON);
            state->addUniform(new osg::Uniform("sky", skyTextureUnit));

            osg::ref_ptr<SceneUtil::StateUpdater> atmosphereState
                = new SceneUtil::StateUpdater(loadAtmosphereNoiseTexture(), 14, loadAtmosphereDetailNoiseTexture(), 15,
                    loadAtmosphereCurlNoiseTexture(), 16, loadAtmosphereWeatherTexture(), 17);
            atmosphereState->setAmbientColor({ 0.22f, 0.2f, 0.19f, 1.f });
            atmosphereState->setFogColor(fog.mColor);
            atmosphereState->setFogStart(fog.mStart);
            atmosphereState->setFogEnd(fog.mEnd);
            atmosphereState->setUnderwaterFogColor(fog.mColor);
            atmosphereState->setUnderwaterFogStart(fog.mStart);
            atmosphereState->setUnderwaterFogEnd(fog.mEnd);
            atmosphereState->setAtmosphere(true, fog.mColor, { 0.25f, 0.38f, 0.48f }, { 0.07f, 0.36f, 0.76f }, 0.f,
                { 0.95f, 0.f, 0.31f }, {}, {}, {}, {}, 0.f, {}, {}, {}, {}, {}, { 0.8f, 0.68f, 0.6f },
                { 0.34f, 0.29f, 0.27f }, { 0.34f, 0.29f, 0.27f }, {}, 2);
            atmosphereState->setDefaults(state);

            viewer.setSceneData(root);
            viewer.realize();
            context->makeCurrent();
            viewer.frame();
            glFinish();

            FoliagePassCapture result{ width, height, eye, viewDirection, 70.f, nearPlane, farPlane, fog.mStart,
                fog.mEnd, fog.mColor, {}, {} };
            result.mColors.reserve(static_cast<std::size_t>(width * height));
            result.mDepths.reserve(static_cast<std::size_t>(width * height));
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const float* color = reinterpret_cast<const float*>(colorCapture->data(x, y));
                    result.mColors.emplace_back(color[0], color[1], color[2], color[3]);
                    result.mDepths.push_back(*reinterpret_cast<const float*>(depthCapture->data(x, y)));
                }
            context->releaseContext();
            context->close();
            return result;
        }

        /// Build the flattened, merged static-instance structure used by distant object paging.
        osg::ref_ptr<osg::Node> createObjectPagedFoliageCluster(const std::vector<osg::ref_ptr<osg::Node>>& trees)
        {
            osg::ref_ptr<osg::Group> mergeGroup = new osg::Group;
            mergeGroup->setDataVariance(osg::Object::STATIC);
            const std::array<osg::Vec3f, 15> positions{ osg::Vec3f(-5200.f, -1600.f, -250.f),
                osg::Vec3f(-3900.f, 900.f, 120.f), osg::Vec3f(-2700.f, -500.f, -80.f),
                osg::Vec3f(-1500.f, 2100.f, 220.f), osg::Vec3f(-400.f, 350.f, 0.f), osg::Vec3f(850.f, -1300.f, -180.f),
                osg::Vec3f(1900.f, 1400.f, 180.f), osg::Vec3f(3100.f, -250.f, -100.f),
                osg::Vec3f(4400.f, 2400.f, 260.f), osg::Vec3f(5600.f, 500.f, 40.f), osg::Vec3f(-4700.f, 3800.f, 340.f),
                osg::Vec3f(-2200.f, 4600.f, 390.f), osg::Vec3f(100.f, 4200.f, 360.f), osg::Vec3f(2600.f, 5000.f, 420.f),
                osg::Vec3f(5100.f, 4300.f, 370.f) };
            for (std::size_t index = 0; index < positions.size(); ++index)
            {
                osg::ref_ptr<osg::Node> instance = static_cast<osg::Node*>(trees[index % trees.size()]->clone(
                    osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES));
                osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
                transform->setDataVariance(osg::Object::STATIC);
                transform->setMatrix(osg::Matrixf::rotate(osg::DegreesToRadians(static_cast<float>(index * 37)),
                                         osg::Vec3f(0.f, 0.f, 1.f))
                    * osg::Matrixf::translate(positions[index]));
                transform->addChild(instance);
                mergeGroup->addChild(transform);
            }

            SceneUtil::Optimizer optimizer;
            optimizer.setViewPoint({ 0.f, -12000.f, 600.f });
            optimizer.setMergeAlphaBlending(true);
            optimizer.optimize(mergeGroup,
                SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES
                    | SceneUtil::Optimizer::MERGE_GEOMETRY);
            return mergeGroup;
        }

        /// Load an optional stock atmosphere dome through the same VFS and resource owners as production.
        osg::ref_ptr<osg::Node> loadVfsAtmosphereDome(const std::filesystem::path& dataDirectory)
        {
            VFS::Manager vfs;
            vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(dataDirectory));
            vfs.buildIndex();
            constexpr VFS::Path::NormalizedView atmospherePath("meshes/sky_atmosphere.nif");
            if (!vfs.exists(atmospherePath))
                return nullptr;

            Resource::ResourceSystem resources(&vfs, 0.0, nullptr);
            resources.getSceneManager()->setShaderPath(
                std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
            auto defines = getAtmosphereHarnessDefines();
            resources.getSceneManager()->getShaderManager().setGlobalDefines(defines);
            osg::ref_ptr<osg::Node> dome = resources.getSceneManager()->getInstance(atmospherePath);
            return dome;
        }

        /// Record local and transformed bounds for optional stock atmosphere geometry.
        struct MeshVertexBounds
        {
            osg::BoundingBox mLocal;
            osg::BoundingBox mWorld;
            osg::Matrix mLocalToWorld;
        };

        /// Calculate local and world-space bounds for every geometry vertex under a node.
        MeshVertexBounds getVertexBounds(osg::Node& node)
        {
            /// Accumulate geometry bounds while preserving the last traversed local-to-world transform.
            class VertexBoundsVisitor : public osg::NodeVisitor
            {
            public:
                /// Traverse every child in the optional stock atmosphere hierarchy.
                VertexBoundsVisitor()
                    : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                {
                }

                /// Expand local and world bounds with one geometry's vertices.
                void apply(osg::Geometry& geometry) override
                {
                    const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                    if (!vertices)
                        return;
                    const osg::Matrix localToWorld = osg::computeLocalToWorld(getNodePath());
                    for (const osg::Vec3f& vertex : *vertices)
                    {
                        mBounds.mLocal.expandBy(vertex);
                        mBounds.mWorld.expandBy(vertex * localToWorld);
                    }
                    mBounds.mLocalToWorld = localToWorld;
                }

                MeshVertexBounds mBounds;
            } visitor;
            node.accept(visitor);
            return visitor.mBounds;
        }

        /// Verify source texture dimensions, independent RGB weather fields, and opaque unused alpha.
        TEST(MWRenderAtmosphereTest, packagedCloudWeatherMapCarriesContinuousFormationFields)
        {
            const osg::ref_ptr<osg::Texture2D> texture = loadAtmosphereWeatherTexture();
            const osg::Image* image = texture->getImage();
            ASSERT_NE(image, nullptr);
            ASSERT_EQ(image->s(), 1024);
            ASSERT_EQ(image->t(), 1024);

            std::array<std::unordered_set<int>, 4> channelValues;
            double typeRainDifference = 0.0;
            std::size_t interiorTypeSamples = 0;
            std::size_t interiorRainSamples = 0;
            for (int y = 0; y < image->t(); ++y)
            {
                for (int x = 0; x < image->s(); ++x)
                {
                    const osg::Vec4f sample = image->getColor(x, y);
                    for (std::size_t channel = 0; channel < channelValues.size(); ++channel)
                        channelValues[channel].insert(static_cast<int>(std::round(sample[channel] * 255.f)));
                    typeRainDifference += std::abs(sample.y() - sample.z());
                    interiorTypeSamples += sample.y() > 0.02f && sample.y() < 0.98f ? 1u : 0u;
                    interiorRainSamples += sample.z() > 0.02f && sample.z() < 0.98f ? 1u : 0u;
                }
            }

            const double sampleCount = static_cast<double>(image->s() * image->t());
            EXPECT_GE(channelValues[0].size(), 240u) << "Coverage must retain the source's continuous dynamic range";
            EXPECT_GE(channelValues[1].size(), 96u) << "Cloud type must vary continuously across the weather map";
            EXPECT_GE(channelValues[2].size(), 96u) << "Precipitation must vary continuously across the weather map";
            EXPECT_EQ(channelValues[3], std::unordered_set<int>{ 255 });
            EXPECT_GT(static_cast<double>(interiorTypeSamples) / sampleCount, 0.75);
            EXPECT_GT(static_cast<double>(interiorRainSamples) / sampleCount, 0.75);
            EXPECT_GT(typeRainDifference / sampleCount, 0.04)
                << "Cloud type and precipitation must not collapse to one repeated control field";
        }

        TEST(MWRenderAtmosphereTest, derivesPhysicalSunDirectionFromDaylightRatherThanWeatherVisibility)
        {
            const osg::Vec3f renderedDirection = osg::Vec3f(0.8f, -0.15f, 0.58f);

            const osg::Vec3f day = getScatteringSunDirection(renderedDirection, true);
            const osg::Vec3f night = getScatteringSunDirection(renderedDirection, false);

            EXPECT_GT(day.z(), 0.f);
            EXPECT_LT(night.z(), 0.f);
            EXPECT_FLOAT_EQ(day.x(), night.x());
            EXPECT_FLOAT_EQ(day.y(), night.y());
            EXPECT_FLOAT_EQ(day.z(), -night.z());
            EXPECT_NEAR(night.length(), 1.f, 1e-6f);
        }

        TEST(MWRenderAtmosphereTest, mapsSunriseEastAndSunsetWest)
        {
            const osg::Vec3f sunrise = getRenderedExteriorSunPosition({ -400.f, 75.f, -100.f });
            const osg::Vec3f sunset = getRenderedExteriorSunPosition({ 400.f, 75.f, -100.f });

            EXPECT_GT(sunrise.x(), 0.f);
            EXPECT_LT(sunset.x(), 0.f);
            EXPECT_FLOAT_EQ(sunrise.y(), sunset.y());
            EXPECT_FLOAT_EQ(sunrise.z(), sunset.z());
        }

        TEST(MWRenderAtmosphereTest, atmosphereQualityTiersBoundIntermediateMemory)
        {
            EXPECT_FLOAT_EQ(getAtmosphereVolumeResolutionScale(0), 0.25f);
            EXPECT_FLOAT_EQ(getAtmosphereVolumeResolutionScale(1), 0.375f);
            EXPECT_FLOAT_EQ(getAtmosphereVolumeResolutionScale(2), 0.5f);
            EXPECT_FLOAT_EQ(getAtmosphereVolumeResolutionScale(3), 0.5f);

            const auto intermediateBytesAt4k = [](int quality) {
                constexpr double width = 3840.0;
                constexpr double height = 2160.0;
                const double volumeScale = getAtmosphereVolumeResolutionScale(quality);
                const double volumePixels = width * volumeScale * height * volumeScale;
                const double shaftPixels = width * volumeScale * 0.5 * height * volumeScale * 0.5;
                return 2.0 * (2.0 * volumePixels * 8.0 + shaftPixels * 6.0);
            };

            constexpr double mebibyte = 1024.0 * 1024.0;
            EXPECT_LT(intermediateBytesAt4k(0) / mebibyte, 19.0);
            EXPECT_LT(intermediateBytesAt4k(1) / mebibyte, 42.0);
            EXPECT_LT(intermediateBytesAt4k(2) / mebibyte, 74.0);
            EXPECT_LT(intermediateBytesAt4k(3) / mebibyte, 74.0);
        }

        TEST(MWRenderAtmosphereTest, cloudAdvectionUsesWeatherSpeedAndGameTimeWithoutStalling)
        {
            CloudAdvectionController controller;
            const osg::Vec2f direction = osg::Vec2f(0.78f, 0.36f) / osg::Vec2f(0.78f, 0.36f).length();

            controller.update(0.5f, false, 1.25f, 30.f);
            const CloudAdvectionState first = controller.getState();
            controller.update(0.25f, false, 2.f, 30.f);
            const CloudAdvectionState second = controller.getState();
            controller.update(0.75f, false, 2.f, 10000.f);
            const CloudAdvectionState third = controller.getState();

            EXPECT_GT(first.mOffsetWorldUnits * direction, 0.f);
            EXPECT_GT(second.mOffsetWorldUnits * direction, first.mOffsetWorldUnits * direction);
            EXPECT_GT(third.mOffsetWorldUnits * direction, second.mOffsetWorldUnits * direction);
            EXPECT_NEAR(
                (second.mOffsetWorldUnits - first.mOffsetWorldUnits).x(), second.mFrameDeltaWorldUnits.x(), 1e-5f);
            EXPECT_NEAR(
                (second.mOffsetWorldUnits - first.mOffsetWorldUnits).y(), second.mFrameDeltaWorldUnits.y(), 1e-5f);
            EXPECT_GT(third.mFrameDeltaWorldUnits.length(), second.mFrameDeltaWorldUnits.length() * 900.f);

            controller.update(10.f, true, 2.f, 10000.f);
            const CloudAdvectionState paused = controller.getState();
            EXPECT_EQ(paused.mOffsetWorldUnits, third.mOffsetWorldUnits);
            EXPECT_EQ(paused.mFrameDeltaWorldUnits, osg::Vec2f());

            controller.update(0.5f, false, 1.25f, 30.f);
            const CloudAdvectionState resumed = controller.getState();
            EXPECT_GT(resumed.mOffsetWorldUnits * direction, paused.mOffsetWorldUnits * direction);
            EXPECT_NEAR(
                (resumed.mOffsetWorldUnits - paused.mOffsetWorldUnits).x(), resumed.mFrameDeltaWorldUnits.x(), 0.002f);
            EXPECT_NEAR(
                (resumed.mOffsetWorldUnits - paused.mOffsetWorldUnits).y(), resumed.mFrameDeltaWorldUnits.y(), 0.002f);
        }

        TEST(MWRenderAtmosphereTest, reservedSuspendedMediumIsOpticallyNeutralByDefault)
        {
            const AtmosphereState atmosphere;
            EXPECT_EQ(atmosphere.mSuspendedMedium.mExtinctionPerWorldUnit, osg::Vec3f());
            EXPECT_EQ(atmosphere.mSuspendedMedium.mSingleScatteringAlbedo, osg::Vec3f(1.f, 1.f, 1.f));
            EXPECT_FLOAT_EQ(atmosphere.mSuspendedMedium.mAnisotropy, 0.f);
            EXPECT_FLOAT_EQ(atmosphere.mSuspendedMedium.mTerrainFollowing, 0.f);
        }

        TEST(MWRenderAtmosphereTest, cloudAdvectionRejectsInvalidOrStoppedProductionClocks)
        {
            CloudAdvectionController controller;
            controller.update(1.f, false, 1.25f, 30.f);
            const CloudAdvectionState moving = controller.getState();

            controller.update(1.f, false, 1.25f, 0.f);
            EXPECT_EQ(controller.getState().mOffsetWorldUnits, moving.mOffsetWorldUnits);
            EXPECT_EQ(controller.getState().mFrameDeltaWorldUnits, osg::Vec2f());

            controller.update(1.f, false, 0.f, 30.f);
            EXPECT_EQ(controller.getState().mOffsetWorldUnits, moving.mOffsetWorldUnits);
            EXPECT_EQ(controller.getState().mFrameDeltaWorldUnits, osg::Vec2f());
        }

        TEST(MWRenderAtmosphereTest, atmosphereHistoryLightingTracksStableFramesAndRejectsRapidTransitions)
        {
            AtmosphereHistoryLightingState stable;
            stable.mSolarRadiance = { 1.f, 0.82f, 0.62f };
            stable.mSkyIrradiance = { 0.32f, 0.41f, 0.58f };
            stable.mGroundIrradiance = { 0.38f, 0.4f, 0.44f };
            stable.mSkyColor = { 0.3f, 0.42f, 0.64f };
            stable.mFogColor = { 0.44f, 0.48f, 0.56f };
            stable.mSunDirection = { 0.98f, 0.f, 0.2f };

            AtmosphereHistoryLightingController controller;
            EXPECT_FLOAT_EQ(controller.update(stable, true), 0.f);
            EXPECT_FLOAT_EQ(controller.update(stable, true), 1.f);

            AtmosphereHistoryLightingState gradual = stable;
            gradual.mSolarRadiance += osg::Vec3f(0.003f, 0.002f, 0.001f);
            gradual.mSkyColor += osg::Vec3f(0.002f, 0.002f, 0.003f);
            EXPECT_GT(controller.update(gradual, true), 0.95f);

            AtmosphereHistoryLightingState rapid = gradual;
            rapid.mSolarRadiance = { 0.2f, 0.08f, 0.03f };
            rapid.mSkyIrradiance = { 0.08f, 0.04f, 0.025f };
            rapid.mSkyColor = { 0.7f, 0.2f, 0.06f };
            rapid.mSunDirection = { 0.4f, 0.f, -0.92f };
            EXPECT_FLOAT_EQ(controller.update(rapid, true), 0.f);

            EXPECT_FLOAT_EQ(controller.update(rapid, false), 0.f);
            EXPECT_FLOAT_EQ(controller.update(stable, true), 0.f);
        }

        /// Publish complete source layer arrays and canonical placement during initialization and updates.
        TEST(MWRenderAtmosphereTest, sceneUniformStatePublishesCanonicalVolumeReferences)
        {
            osg::ref_ptr<SceneUtil::StateUpdater> updater = new SceneUtil::StateUpdater;
            const osg::Vec4f incomingCloudParameters{ 0.8f, 1.1f, 32000.f, 68000.f };
            const osg::Vec4f cloudScenario{ 0.4f, 2.1f, 0.35f, 1.f };
            SceneUtil::CloudLayer outgoing;
            outgoing.mAnvilLarge = { 6.f, .2f, 8.f, .3f };
            auto layers = SceneUtil::packCloudLayers(outgoing, SceneUtil::secondaryCloudLayer(), {});
            updater->setCloudLayers(layers);
            updater->setAtmosphere(true, { 0.4f, 0.5f, 0.7f, 1.f }, { 0.25f, 0.38f, 0.48f }, { 0.07f, 0.36f, 0.76f },
                1.f, { 0.8f, 0.f, 0.6f }, { 0.58f, 0.9f, 48000.f, 22000.f }, incomingCloudParameters, cloudScenario, {},
                0.f, {}, {}, { 12500.f, 0.75f, 1500.f, 0.f }, { 4000.f, 5000.f, 0.2f, -0.1f }, { 1.f, 0.8f, 0.6f, 1.f },
                { 1.f, 0.8f, 0.6f }, { 0.3f, 0.4f, 0.55f }, { 0.45f, 0.5f, 0.6f }, { 1234.f, 567.f }, 2);
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            updater->setDefaults(state);
            const auto expectLayerPublication = [&] {
                osg::Uniform* publication = state->getUniform("atmosphere.cloudLayers");
                ASSERT_NE(publication, nullptr);
                ASSERT_EQ(publication->getNumElements(), layers.size());
                for (unsigned int index = 0; index < layers.size(); ++index)
                {
                    osg::Vec4f actual;
                    ASSERT_TRUE(publication->getElement(index, actual));
                    EXPECT_EQ(actual, layers[index]);
                }
            };
            expectLayerPublication();

            osg::Vec2f publishedOffset;
            osg::Vec4f publishedMediumReference;
            osg::Vec4f publishedOrography;
            osg::Vec4f publishedIncomingCloudParameters;
            osg::Vec4f publishedCloudScenario;
            ASSERT_TRUE(state->getUniform("atmosphere.cloudAdvectionOffsetWorldUnits")->get(publishedOffset));
            ASSERT_TRUE(state->getUniform("atmosphere.mediumReferenceParameters")->get(publishedMediumReference));
            ASSERT_TRUE(state->getUniform("atmosphere.orographicParameters")->get(publishedOrography));
            ASSERT_TRUE(state->getUniform("atmosphere.incomingCloudParameters")->get(publishedIncomingCloudParameters));
            ASSERT_TRUE(state->getUniform("atmosphere.cloudScenarioParameters")->get(publishedCloudScenario));
            EXPECT_EQ(publishedOffset, osg::Vec2f(1234.f, 567.f));
            EXPECT_EQ(publishedMediumReference, osg::Vec4f(12500.f, 0.75f, 1500.f, 0.f));
            EXPECT_EQ(publishedOrography, osg::Vec4f(4000.f, 5000.f, 0.2f, -0.1f));
            EXPECT_EQ(publishedIncomingCloudParameters, incomingCloudParameters);
            EXPECT_EQ(publishedCloudScenario, cloudScenario);

            updater->setAtmosphere(true, { 0.4f, 0.5f, 0.7f, 1.f }, { 0.25f, 0.38f, 0.48f }, { 0.07f, 0.36f, 0.76f },
                1.f, { 0.8f, 0.f, 0.6f }, { 0.58f, 0.9f, 48000.f, 22000.f }, incomingCloudParameters, cloudScenario, {},
                0.f, {}, {}, { 12600.f, 0.74f, 1600.f, 0.f }, { 4100.f, 5100.f, 0.18f, -0.08f },
                { 1.f, 0.8f, 0.6f, 1.f }, { 1.f, 0.8f, 0.6f }, { 0.3f, 0.4f, 0.55f }, { 0.45f, 0.5f, 0.6f },
                { 1240.f, 570.f }, 2);
            outgoing.mGradientLarge = { .02f, .15f, .7f, .9f };
            layers = SceneUtil::packCloudLayers(SceneUtil::secondaryCloudLayer(), outgoing, outgoing);
            updater->setCloudLayers(layers);
            updater->apply(state, nullptr);
            expectLayerPublication();
            ASSERT_TRUE(state->getUniform("atmosphere.cloudAdvectionOffsetWorldUnits")->get(publishedOffset));
            ASSERT_TRUE(state->getUniform("atmosphere.mediumReferenceParameters")->get(publishedMediumReference));
            ASSERT_TRUE(state->getUniform("atmosphere.orographicParameters")->get(publishedOrography));
            EXPECT_EQ(publishedOffset, osg::Vec2f(1240.f, 570.f));
            EXPECT_EQ(publishedMediumReference, osg::Vec4f(12600.f, 0.74f, 1600.f, 0.f));
            EXPECT_EQ(publishedOrography, osg::Vec4f(4100.f, 5100.f, 0.18f, -0.08f));
        }

        TEST(MWRenderAtmosphereTest, reshapesStockHorizonBandIntoClosedSkyBackground)
        {
            osg::ref_ptr<osg::Node> dome = createStockStyleHorizonBand();
            AtmosphereDomeVisitor reshape;
            dome->accept(reshape);

            ASSERT_TRUE(reshape.reshaped());
            for (const osg::Vec3f& direction : { osg::Vec3f(1.f, 0.f, 0.08f), osg::Vec3f(-1.f, 0.f, 0.08f),
                     osg::Vec3f(0.f, 1.f, 0.5f), osg::Vec3f(0.f, -1.f, 0.5f), osg::Vec3f(0.f, 0.f, 1.f),
                     osg::Vec3f(1.f, 0.f, -0.08f), osg::Vec3f(0.2f, -1.f, -0.5f), osg::Vec3f(0.f, 0.f, -1.f) })
                EXPECT_TRUE(intersectsDome(*dome, direction))
                    << direction.x() << ',' << direction.y() << ',' << direction.z();

            const MeshVertexBounds bounds = getVertexBounds(*dome);
            EXPECT_NEAR(bounds.mLocal.zMin(), -1600.f, 1e-3f);
            EXPECT_NEAR(bounds.mLocal.zMax(), 1600.f, 1e-3f);
        }

        /// Keep diffuse sunset highlights below channel clipping without flattening the directional color field.
        TEST(MWRenderAtmosphereTest, sunsetSkyRetainsHighlightGradientsInTheSceneColorRange)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 192, 96);
            ASSERT_TRUE(harness.isValid());
            for (const float sunHeight : { 0.16f, 0.08f, 0.f, -0.07f })
            {
                SCOPED_TRACE(sunHeight);
                harness.setSunDirection({ 1.f, 0.f, sunHeight });
                const CapturedImage image = harness.renderImage({ 1.f, 0.f, 0.08f });
                ASSERT_EQ(image.mPixels.size(), 192u * 96u);
                float maximumChannel = 0.f;
                std::size_t clippedPixels = 0;
                for (const CapturedColor& color : image.mPixels)
                {
                    ASSERT_TRUE(std::isfinite(color.mRed));
                    ASSERT_TRUE(std::isfinite(color.mGreen));
                    ASSERT_TRUE(std::isfinite(color.mBlue));
                    const float peak = std::max({ color.mRed, color.mGreen, color.mBlue });
                    maximumChannel = std::max(maximumChannel, peak);
                    clippedPixels += peak >= 1.f;
                }
                RecordProperty("maximum_diffuse_channel", std::to_string(maximumChannel));
                EXPECT_EQ(clippedPixels, 0u);
                EXPECT_LT(maximumChannel, 0.995f);
                const CapturedColor sunward = harness.render({ 1.f, 0.f, 0.03f });
                const CapturedColor away = harness.render({ -1.f, 0.f, 0.03f });
                EXPECT_GT(sunward.mRed, away.mRed + 0.1f);
                EXPECT_GT(sunward.mRed, sunward.mBlue + 0.1f);
            }
        }

        /// Retain a golden sunward core and cooler upper sky instead of flattening sunset to one saturated color.
        TEST(MWRenderAtmosphereTest, cloudFreeSunsetRetainsColorSeparation)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            ASSERT_TRUE(harness.isValid());
            harness.setSunDirection({ 1.f, 0.f, 0.08f });
            const CapturedColor horizon = harness.render({ 1.f, 0.f, 0.03f });
            const CapturedColor upperSky = harness.render({ 1.f, 0.f, 1.f });
            EXPECT_GT(horizon.mGreen, horizon.mRed * 0.6f);
            EXPECT_GT(horizon.mRed - horizon.mBlue, 0.1f);
            EXPECT_GT(horizon.mRed, upperSky.mRed + 0.15f);
            EXPECT_GT(upperSky.mBlue / upperSky.mRed, horizon.mBlue / horizon.mRed + 0.2f);
        }

        /// Separate the warm solar horizon from cool upper twilight for both solar azimuths.
        TEST(MWRenderAtmosphereTest, twilightRetainsCoolUpperSkyAboveTheWarmHorizon)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 1, 1);
            ASSERT_TRUE(harness.isValid());
            for (const float azimuth : { -1.f, 1.f })
                for (const float height : { 0.08f, 0.f, -0.07f, -0.13f })
                {
                    SCOPED_TRACE(azimuth);
                    SCOPED_TRACE(height);
                    harness.setSunDirection({ azimuth, 0.f, height });
                    const CapturedColor upper = harness.render({ azimuth, 0.f, 0.7f });
                    const CapturedColor horizon = harness.render({ azimuth, 0.f, 0.03f });
                    EXPECT_GT(upper.mBlue, upper.mRed * 1.25f);
                    EXPECT_GT(upper.mBlue, upper.mGreen);
                    EXPECT_GT(horizon.mRed, horizon.mBlue * 1.25f);
                }
        }

        /// Keep dawn and dusk color evolution continuous as the sun crosses the horizon and twilight boundaries.
        TEST(MWRenderAtmosphereTest, twilightColorEvolvesContinuouslyWithSolarElevation)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 1, 1);
            ASSERT_TRUE(harness.isValid());
            for (const osg::Vec3f direction :
                { osg::Vec3f(1.f, 0.f, 0.03f), osg::Vec3f(1.f, 0.f, 0.7f), osg::Vec3f(-1.f, 0.f, 0.1f) })
            {
                for (const float sunHeight :
                    { -0.3f, -0.18f, -0.13f, -0.07f, -0.015f, 0.f, 0.02f, 0.06f, 0.08f, 0.34f })
                {
                    SCOPED_TRACE(sunHeight);
                    harness.setSunDirection({ 1.f, 0.f, sunHeight - 0.0001f });
                    const CapturedColor below = harness.render(direction);
                    harness.setSunDirection({ 1.f, 0.f, sunHeight + 0.0001f });
                    const CapturedColor above = harness.render(direction);
                    EXPECT_NEAR(above.mRed, below.mRed, 0.005f);
                    EXPECT_NEAR(above.mGreen, below.mGreen, 0.005f);
                    EXPECT_NEAR(above.mBlue, below.mBlue, 0.005f);
                }
            }
        }

        /// Reject a sharp color crest at the horizon in the rendered sky, without filtering neighboring pixels.
        TEST(MWRenderAtmosphereTest, sunsetHorizonColorHasNoSharpCrest)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 1, 1);
            ASSERT_TRUE(harness.isValid());
            for (const float sunHeight : { 0.08f, 0.f, -0.07f })
            {
                SCOPED_TRACE(sunHeight);
                harness.setSunDirection({ 1.f, 0.f, sunHeight });
                const CapturedColor below = harness.render({ 1.f, 0.f, -0.01f });
                const CapturedColor middle = harness.render({ 1.f, 0.f, 0.f });
                const CapturedColor above = harness.render({ 1.f, 0.f, 0.01f });
                EXPECT_LT(std::abs(below.mGreen - 2.f * middle.mGreen + above.mGreen), 0.01f);
                EXPECT_LT(std::abs(below.mBlue - 2.f * middle.mBlue + above.mBlue), 0.01f);
            }
        }

        TEST(MWRenderAtmosphereTest, offscreenHarnessSeparatesSolarAndAntiSolarSky)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const CapturedColor solar = harness.render({ 1.f, 0.f, 0.08f });
            const CapturedColor antiSolar = harness.render({ -1.f, 0.f, 0.08f });
            expectAtmosphereReference(solar, { 1.f, 0.f, 0.08f });
            expectAtmosphereReference(antiSolar, { -1.f, 0.f, 0.08f });
            EXPECT_GT(solar.mRed, antiSolar.mRed + 0.1f);
            EXPECT_GT(solar.mRed / solar.mBlue, antiSolar.mRed / antiSolar.mBlue + 0.1f);
        }

        TEST(MWRenderAtmosphereTest, offscreenHarnessTracksSunriseAndSunsetAcrossCardinalDirections)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f east(1.f, 0.f, 0.08f);
            const osg::Vec3f west(-1.f, 0.f, 0.08f);
            const osg::Vec3f sunrise = getRenderedExteriorSunPosition({ -400.f, 75.f, -100.f });
            const osg::Vec3f sunset = getRenderedExteriorSunPosition({ 400.f, 75.f, -100.f });

            harness.setSunDirection(sunrise);
            const CapturedColor sunriseEast = harness.render(east);
            const CapturedColor sunriseWest = harness.render(west);
            EXPECT_GT(sunriseEast.mRed, sunriseWest.mRed + 0.1f);

            harness.setSunDirection(sunset);
            const CapturedColor sunsetEast = harness.render(east);
            const CapturedColor sunsetWest = harness.render(west);
            EXPECT_GT(sunsetWest.mRed, sunsetEast.mRed + 0.1f);
        }

        TEST(MWRenderAtmosphereTest, nightStarsNeverDarkenTheProceduralSky)
        {
            const osg::Vec4f dawnSky(0.78f, 0.42f, 0.24f, 1.f);
            const osg::ref_ptr<osg::Texture2D> starTexture = createFilteredStarTexture();
            AtmosphereRenderHarness hiddenStars(createHarnessDome());
            AtmosphereRenderHarness visibleStars(createHarnessDome());
            ASSERT_TRUE(hiddenStars.isValid());
            ASSERT_TRUE(visibleStars.isValid());

            const CapturedImage background = hiddenStars.renderNightStars(*starTexture, dawnSky, 0.f);
            const CapturedImage composited = visibleStars.renderNightStars(*starTexture, dawnSky, 0.72f);
            ASSERT_EQ(composited.mPixels.size(), background.mPixels.size());

            float greatestAddedRadiance = 0.f;
            float greatestSubtractedRadiance = 0.f;
            for (std::size_t index = 0; index < composited.mPixels.size(); ++index)
            {
                const CapturedColor& before = background.mPixels[index];
                const CapturedColor& after = composited.mPixels[index];
                EXPECT_TRUE(std::isfinite(after.mRed));
                EXPECT_TRUE(std::isfinite(after.mGreen));
                EXPECT_TRUE(std::isfinite(after.mBlue));
                greatestAddedRadiance = std::max({ greatestAddedRadiance, after.mRed - before.mRed,
                    after.mGreen - before.mGreen, after.mBlue - before.mBlue });
                greatestSubtractedRadiance = std::max({ greatestSubtractedRadiance, before.mRed - after.mRed,
                    before.mGreen - after.mGreen, before.mBlue - after.mBlue });
            }

            EXPECT_GT(greatestAddedRadiance, 0.05f);
            EXPECT_LT(greatestSubtractedRadiance, 0.001f);
        }

        TEST(MWRenderAtmosphereTest, nightStarRadiancePreservesFadeAndModdedColor)
        {
            const osg::Vec4f nightSky(0.025f, 0.035f, 0.055f, 1.f);
            const osg::ref_ptr<osg::Texture2D> starTexture = createFilteredStarTexture({ 0.12f, 0.38f, 1.f });
            AtmosphereRenderHarness hiddenStars(createHarnessDome());
            AtmosphereRenderHarness halfVisibleStars(createHarnessDome());
            AtmosphereRenderHarness fullyVisibleStars(createHarnessDome());
            ASSERT_TRUE(hiddenStars.isValid());
            ASSERT_TRUE(halfVisibleStars.isValid());
            ASSERT_TRUE(fullyVisibleStars.isValid());

            const CapturedImage background = hiddenStars.renderNightStars(*starTexture, nightSky, 0.f);
            const CapturedImage halfVisible = halfVisibleStars.renderNightStars(*starTexture, nightSky, 0.5f);
            const CapturedImage fullyVisible = fullyVisibleStars.renderNightStars(*starTexture, nightSky, 1.f);
            ASSERT_EQ(background.mPixels.size(), halfVisible.mPixels.size());
            ASSERT_EQ(background.mPixels.size(), fullyVisible.mPixels.size());

            osg::Vec3f maximumHalfContribution(0.f, 0.f, 0.f);
            osg::Vec3f maximumFullContribution(0.f, 0.f, 0.f);
            for (std::size_t index = 0; index < background.mPixels.size(); ++index)
            {
                const CapturedColor& base = background.mPixels[index];
                const CapturedColor& half = halfVisible.mPixels[index];
                const CapturedColor& full = fullyVisible.mPixels[index];
                maximumHalfContribution.x() = std::max(maximumHalfContribution.x(), half.mRed - base.mRed);
                maximumHalfContribution.y() = std::max(maximumHalfContribution.y(), half.mGreen - base.mGreen);
                maximumHalfContribution.z() = std::max(maximumHalfContribution.z(), half.mBlue - base.mBlue);
                maximumFullContribution.x() = std::max(maximumFullContribution.x(), full.mRed - base.mRed);
                maximumFullContribution.y() = std::max(maximumFullContribution.y(), full.mGreen - base.mGreen);
                maximumFullContribution.z() = std::max(maximumFullContribution.z(), full.mBlue - base.mBlue);
            }

            EXPECT_GT(maximumFullContribution.z(), maximumFullContribution.y() * 2.f);
            EXPECT_GT(maximumFullContribution.y(), maximumFullContribution.x() * 2.f);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(maximumHalfContribution[channel], maximumFullContribution[channel] * 0.5f, 0.003f);
        }

        TEST(MWRenderAtmosphereTest, legacyNightDomeRetainsAuthoredTranslucentCompositing)
        {
            const osg::Vec4f dawnSky(0.78f, 0.42f, 0.24f, 1.f);
            const osg::ref_ptr<osg::Texture2D> starTexture = createFilteredStarTexture();
            AtmosphereRenderHarness backgroundHarness(createHarnessDome());
            AtmosphereRenderHarness legacyHarness(createHarnessDome());
            ASSERT_TRUE(backgroundHarness.isValid());
            ASSERT_TRUE(legacyHarness.isValid());

            const auto legacyBlendMode = AtmosphereNightUpdater::StarBlendMode::AuthoredTranslucency;
            const CapturedImage background
                = backgroundHarness.renderNightStars(*starTexture, dawnSky, 0.f, legacyBlendMode);
            const CapturedImage legacy = legacyHarness.renderNightStars(*starTexture, dawnSky, 0.72f, legacyBlendMode);
            float greatestSubtractedRadiance = 0.f;
            for (std::size_t index = 0; index < legacy.mPixels.size(); ++index)
            {
                greatestSubtractedRadiance = std::max(
                    { greatestSubtractedRadiance, background.mPixels[index].mRed - legacy.mPixels[index].mRed,
                        background.mPixels[index].mGreen - legacy.mPixels[index].mGreen,
                        background.mPixels[index].mBlue - legacy.mPixels[index].mBlue });
            }
            EXPECT_GT(greatestSubtractedRadiance, 0.1f);
        }

        TEST(MWRenderAtmosphereTest, analyticSunPreservesAngularRadiusAcrossViewportsAndFov)
        {
            const auto measureRadius = [](int width, int height, double verticalFovDegrees) {
                AtmosphereRenderHarness harness(createHarnessDome(), width, height, verticalFovDegrees);
                if (!harness.isValid())
                    return std::optional<float>{};

                harness.setAtmosphereVolumes({}, {}, { 1.f, 0.82f, 0.62f, 1.f });
                const CapturedImage image = harness.renderImage({ 1.f, 0.f, 0.08f });
                const int centerX = width / 2;
                const int centerY = height / 2;
                const float background = image.at(centerX, centerY + height / 4).mRed;
                const float threshold = background + 1.f;
                int edgeY = centerY;
                while (edgeY + 1 < height && image.at(centerX, edgeY + 1).mRed > threshold)
                    ++edgeY;

                EXPECT_GT(image.at(centerX, centerY).mRed, threshold + 1.f);
                EXPECT_GT(edgeY, centerY);
                const float radiusPixels = static_cast<float>(edgeY - centerY) + 0.5f;
                const float halfFovRadians = osg::DegreesToRadians(static_cast<float>(verticalFovDegrees * 0.5));
                return std::optional<float>(
                    std::atan(radiusPixels / (static_cast<float>(height) * 0.5f) * std::tan(halfFovRadians)));
            };

            const std::optional<float> square = measureRadius(128, 128, 70.0);
            const std::optional<float> wide = measureRadius(256, 128, 70.0);
            const std::optional<float> dense = measureRadius(256, 256, 70.0);
            const std::optional<float> narrowFov = measureRadius(128, 128, 50.0);
            if (!square || !wide || !dense || !narrowFov)
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            constexpr float expectedRadius = 0.028f;
            constexpr float rasterTolerance = 0.008f;
            EXPECT_NEAR(*square, expectedRadius, rasterTolerance);
            EXPECT_NEAR(*wide, expectedRadius, rasterTolerance);
            EXPECT_NEAR(*dense, expectedRadius, rasterTolerance);
            EXPECT_NEAR(*narrowFov, expectedRadius, rasterTolerance);
        }

        TEST(MWRenderAtmosphereTest, reflectedCloudFieldIsDeterministicAndChangesSkyRadiance)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f viewDirection(1.f, 0.f, 0.35f);
            const CapturedImage foggedClear = harness.renderReflectedImage({}, viewDirection);
            harness.setAtmosphereVolumes({ 0.95f, 1.05f, 1200.f, 22000.f }, {}, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedImage foggedClouds = harness.renderReflectedImage({}, viewDirection);
            const CapturedImage repeatedFoggedClouds = harness.renderReflectedImage({}, viewDirection);

            harness.setFogRange(1000000.f, 2000000.f);
            harness.setAtmosphereVolumes({}, {}, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedImage clearPathClouds = harness.renderReflectedImage({}, viewDirection);
            harness.setAtmosphereVolumes({ 0.95f, 1.05f, 1200.f, 22000.f }, {}, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedImage visibleClouds = harness.renderReflectedImage({}, viewDirection);

            double foggedChange = 0.0;
            double visibleChange = 0.0;
            double repeatDifference = 0.0;
            for (std::size_t index = 0; index < foggedClear.mPixels.size(); ++index)
            {
                const CapturedColor& foggedBase = foggedClear.mPixels[index];
                const CapturedColor& fogged = foggedClouds.mPixels[index];
                const CapturedColor& repeated = repeatedFoggedClouds.mPixels[index];
                const CapturedColor& clearBase = clearPathClouds.mPixels[index];
                const CapturedColor& visible = visibleClouds.mPixels[index];
                foggedChange += std::abs(fogged.mRed - foggedBase.mRed) + std::abs(fogged.mGreen - foggedBase.mGreen)
                    + std::abs(fogged.mBlue - foggedBase.mBlue);
                visibleChange += std::abs(visible.mRed - clearBase.mRed) + std::abs(visible.mGreen - clearBase.mGreen)
                    + std::abs(visible.mBlue - clearBase.mBlue);
                repeatDifference += std::abs(fogged.mRed - repeated.mRed) + std::abs(fogged.mGreen - repeated.mGreen)
                    + std::abs(fogged.mBlue - repeated.mBlue);
                EXPECT_TRUE(std::isfinite(visible.mRed));
                EXPECT_TRUE(std::isfinite(visible.mGreen));
                EXPECT_TRUE(std::isfinite(visible.mBlue));
            }

            const double channelCount = static_cast<double>(foggedClear.mPixels.size() * 3);
            EXPECT_GT(visibleChange / channelCount, 0.006);
            EXPECT_GT(foggedChange / channelCount, 0.001);
            EXPECT_LT(foggedChange, visibleChange)
                << "Reflected clouds must converge toward the same path atmosphere as distant landscape";
            EXPECT_LT(repeatDifference / channelCount, 1e-6);
        }

        TEST(MWRenderAtmosphereTest, reflectedCloudAdvectionUsesWorldScaleMotion)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f viewDirection(1.f, 0.f, 0.35f);
            harness.setAtmosphereVolumes({ 0.95f, 1.05f, 1200.f, 22000.f }, {}, { 1.f, 0.82f, 0.62f, 0.f });
            CloudAdvectionController controller;
            harness.setCloudAdvectionOffset({ 0.f, 0.f });
            const CapturedImage initial = harness.renderReflectedImage({}, viewDirection);
            controller.update(1.f, false, 1.25f, 30.f);
            harness.setCloudAdvectionOffset(controller.getState().mOffsetWorldUnits);
            const CapturedImage oneSecondLater = harness.renderReflectedImage({}, viewDirection);
            controller.update(119.f, false, 1.25f, 30.f);
            harness.setCloudAdvectionOffset(controller.getState().mOffsetWorldUnits);
            const CapturedImage twoMinutesLater = harness.renderReflectedImage({}, viewDirection);

            const auto averageDifference = [](const CapturedImage& first, const CapturedImage& second) {
                double difference = 0.0;
                for (std::size_t index = 0; index < first.mPixels.size(); ++index)
                {
                    difference += std::abs(first.mPixels[index].mRed - second.mPixels[index].mRed)
                        + std::abs(first.mPixels[index].mGreen - second.mPixels[index].mGreen)
                        + std::abs(first.mPixels[index].mBlue - second.mPixels[index].mBlue);
                }
                return difference / static_cast<double>(first.mPixels.size() * 3);
            };

            const double oneSecondDifference = averageDifference(initial, oneSecondLater);
            const double twoMinuteDifference = averageDifference(initial, twoMinutesLater);
            EXPECT_LT(oneSecondDifference, 0.002);
            EXPECT_GT(twoMinuteDifference, 0.0005);
            EXPECT_GT(twoMinuteDifference, oneSecondDifference * 4.0);
        }

        TEST(MWRenderAtmosphereTest, reflectedCloudAdvectionMaintainsOneSignedScreenDirection)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 192, 128);
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f viewDirection(1.f, 0.f, 0.35f);
            const CapturedImage clearSky = harness.renderReflectedImage({}, viewDirection);
            harness.setAtmosphereVolumes({ 0.72f, 1.f, 48000.f, 22000.f }, {}, { 1.f, 0.82f, 0.62f, 0.f });
            harness.setCloudAdvectionOffset({ 0.f, 0.f });
            const CapturedImage initial = harness.renderReflectedImage({}, viewDirection);
            harness.setCloudAdvectionOffset({ 9100.f, 4200.f });
            const CapturedImage firstStep = harness.renderReflectedImage({}, viewDirection);
            harness.setCloudAdvectionOffset({ 18200.f, 8400.f });
            const CapturedImage secondStep = harness.renderReflectedImage({}, viewDirection);

            const auto bestHorizontalShift = [&clearSky](const CapturedImage& from, const CapturedImage& to) {
                int bestShift = 0;
                double bestError = std::numeric_limits<double>::max();
                for (int shift = -16; shift <= 16; ++shift)
                {
                    double error = 0.0;
                    std::size_t comparedChannels = 0;
                    const int firstX = std::max(8, 8 - shift);
                    const int lastX = std::min(from.mWidth - 8, from.mWidth - 8 - shift);
                    for (int y = 8; y < from.mHeight - 8; ++y)
                        for (int x = firstX; x < lastX; ++x)
                        {
                            const CapturedColor& first = from.at(x, y);
                            const CapturedColor& second = to.at(x + shift, y);
                            const CapturedColor& firstBackground = clearSky.at(x, y);
                            const CapturedColor& secondBackground = clearSky.at(x + shift, y);
                            error
                                += std::abs((first.mRed - firstBackground.mRed) - (second.mRed - secondBackground.mRed))
                                + std::abs(
                                    (first.mGreen - firstBackground.mGreen) - (second.mGreen - secondBackground.mGreen))
                                + std::abs(
                                    (first.mBlue - firstBackground.mBlue) - (second.mBlue - secondBackground.mBlue));
                            comparedChannels += 3;
                        }
                    error /= static_cast<double>(comparedChannels);
                    if (error < bestError)
                    {
                        bestError = error;
                        bestShift = shift;
                    }
                }
                return bestShift;
            };

            const int firstShift = bestHorizontalShift(initial, firstStep);
            const int secondShift = bestHorizontalShift(firstStep, secondStep);
            EXPECT_NE(firstShift, 0);
            EXPECT_EQ(firstShift > 0, secondShift > 0);
            EXPECT_LE(std::abs(firstShift - secondShift), 1);
        }

        TEST(MWRenderAtmosphereTest, shippedCloudAltitudeReachesTheNearHorizonReflection)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            harness.setFogRange(1000000.f, 2000000.f);
            const osg::Vec3f nearHorizonDirection(1.f, 0.f, 0.08f);
            harness.setAtmosphereVolumes({}, {}, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedImage clear = harness.renderReflectedImage({}, nearHorizonDirection);
            harness.setAtmosphereVolumes({ 0.58f, 0.9f, 48000.f, 22000.f }, {}, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedImage cloudy = harness.renderReflectedImage({}, nearHorizonDirection);

            double difference = 0.0;
            for (std::size_t index = 0; index < clear.mPixels.size(); ++index)
            {
                difference += std::abs(clear.mPixels[index].mRed - cloudy.mPixels[index].mRed)
                    + std::abs(clear.mPixels[index].mGreen - cloudy.mPixels[index].mGreen)
                    + std::abs(clear.mPixels[index].mBlue - cloudy.mPixels[index].mBlue);
            }
            EXPECT_GT(difference / static_cast<double>(clear.mPixels.size() * 3), 0.005);
        }

        TEST(MWRenderAtmosphereTest, analyticSunDiscClipsPerPixelAtTheHorizon)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 128, 128);
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f horizonDirection(1.f, 0.f, 0.f);
            harness.setSunDirection(horizonDirection, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedImage noDisc = harness.renderImage(horizonDirection);
            harness.setSunDirection(horizonDirection, { 1.f, 0.82f, 0.62f, 1.f });
            const CapturedImage clippedDisc = harness.renderImage(horizonDirection);

            const int centerX = clippedDisc.mWidth / 2;
            const int centerY = clippedDisc.mHeight / 2;
            const float upperDiscContribution
                = clippedDisc.at(centerX, centerY + 2).mRed - noDisc.at(centerX, centerY + 2).mRed;
            const float lowerDiscContribution
                = clippedDisc.at(centerX, centerY - 2).mRed - noDisc.at(centerX, centerY - 2).mRed;
            EXPECT_GT(upperDiscContribution, 0.5f);
            EXPECT_LT(std::abs(lowerDiscContribution), 0.02f);
        }

        TEST(MWRenderAtmosphereTest, analyticSunDiscIsSuppressedBelowTheHorizon)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f belowHorizonDirection(1.f, 0.f, -0.12f);
            harness.setSunDirection(belowHorizonDirection, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedColor noDisc = harness.render(belowHorizonDirection);
            harness.setSunDirection(belowHorizonDirection, { 1.f, 0.82f, 0.62f, 1.f });
            const CapturedColor clippedDisc = harness.render(belowHorizonDirection);
            EXPECT_NEAR(clippedDisc.mRed, noDisc.mRed, 0.002f);
            EXPECT_NEAR(clippedDisc.mGreen, noDisc.mGreen, 0.002f);
            EXPECT_NEAR(clippedDisc.mBlue, noDisc.mBlue, 0.002f);
        }

        TEST(MWRenderAtmosphereTest, refractedSunDiscRemainsVisibleWithItsGeometricCenterBelowTheHorizon)
        {
            AtmosphereRenderHarness harness(createHarnessDome(), 192, 192, 12.0);
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f geometricSunDirection(1.f, 0.f, -0.006f);
            const osg::Vec3f apparentHorizonDirection(1.f, 0.f, 0.004f);
            harness.setSunDirection(geometricSunDirection, { 1.f, 0.82f, 0.62f, 0.f });
            const CapturedColor noDisc = harness.render(apparentHorizonDirection);
            harness.setSunDirection(geometricSunDirection, { 1.f, 0.82f, 0.62f, 1.f });
            const CapturedColor refractedDisc = harness.render(apparentHorizonDirection);

            EXPECT_GT(refractedDisc.mRed - noDisc.mRed, 0.35f);
            EXPECT_GT(refractedDisc.mGreen - noDisc.mGreen, 0.15f);
        }

        /// Retain the twilight response to canonical illumination through the bounded highlight response.
        TEST(MWRenderAtmosphereTest, postSunsetScatteringUsesCanonicalIlluminationAndFadesIntoNight)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f solarHorizonDirection(1.f, 0.f, 0.03f);
            harness.setSunDirection({ 1.f, 0.f, -0.07f });
            harness.setAtmosphereLighting({ 1.f, 0.82f, 0.62f }, { 0.3f, 0.4f, 0.55f }, { 0.45f, 0.5f, 0.6f });
            const CapturedColor illuminatedTwilight = harness.render(solarHorizonDirection);
            harness.setAtmosphereLighting({}, {}, {});
            const CapturedColor unilluminatedTwilight = harness.render(solarHorizonDirection);
            harness.setAtmosphereLighting({ 1.f, 0.82f, 0.62f }, { 0.3f, 0.4f, 0.55f }, { 0.45f, 0.5f, 0.6f });
            harness.setSunDirection({ 1.f, 0.f, -0.36f });
            const CapturedColor astronomicalNight = harness.render(solarHorizonDirection);

            const float illuminatedLuminance = 0.2126f * illuminatedTwilight.mRed + 0.7152f * illuminatedTwilight.mGreen
                + 0.0722f * illuminatedTwilight.mBlue;
            const float unilluminatedLuminance = 0.2126f * unilluminatedTwilight.mRed
                + 0.7152f * unilluminatedTwilight.mGreen + 0.0722f * unilluminatedTwilight.mBlue;
            EXPECT_GT(illuminatedLuminance, unilluminatedLuminance + 0.01f);
            EXPECT_GT(illuminatedTwilight.mRed, illuminatedTwilight.mBlue);
            EXPECT_GT(illuminatedTwilight.mRed, astronomicalNight.mRed + 0.08f);
            EXPECT_GT(illuminatedTwilight.mBlue, astronomicalNight.mBlue + 0.02f);
        }

        TEST(MWRenderAtmosphereTest, analyticSunUsesHdrTintAndDisappearsBehindTheCamera)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const osg::Vec3f sunDirection(1.f, 0.f, 0.08f);
            harness.setAtmosphereVolumes({}, {}, { 1.f, 0.2f, 0.08f, 0.f });
            const CapturedColor noDisc = harness.render(sunDirection);
            harness.setAtmosphereVolumes({}, {}, { 1.f, 0.2f, 0.08f, 1.f });
            const CapturedColor warmDisc = harness.render(sunDirection);

            const float redContribution = warmDisc.mRed - noDisc.mRed;
            const float greenContribution = warmDisc.mGreen - noDisc.mGreen;
            const float blueContribution = warmDisc.mBlue - noDisc.mBlue;
            EXPECT_GT(redContribution, 2.f);
            EXPECT_GT(redContribution, greenContribution * 2.f);
            EXPECT_GT(greenContribution, blueContribution);

            const osg::Vec3f oppositeView(-1.f, 0.f, 0.08f);
            harness.setAtmosphereVolumes({}, {}, { 1.f, 0.2f, 0.08f, 0.f });
            const CapturedColor noDiscBehind = harness.render(oppositeView);
            harness.setAtmosphereVolumes({}, {}, { 1.f, 0.2f, 0.08f, 1.f });
            const CapturedColor discBehind = harness.render(oppositeView);
            EXPECT_NEAR(discBehind.mRed, noDiscBehind.mRed, 0.002f);
            EXPECT_NEAR(discBehind.mGreen, noDiscBehind.mGreen, 0.002f);
            EXPECT_NEAR(discBehind.mBlue, noDiscBehind.mBlue, 0.002f);
        }

        TEST(MWRenderAtmosphereTest, waterReflectionCameraRendersTheProceduralSkyField)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            harness.setFogRange(1000000.f, 2000000.f);
            const osg::Vec3f eyePosition(3400.f, -1700.f, 850.f);
            osg::Vec3f skyDirection(0.82f, 0.31f, 0.48f);
            skyDirection.normalize();
            const CapturedImage clear = harness.renderReflectedImage(eyePosition, skyDirection);
            harness.setAtmosphereVolumes({ 0.95f, 1.05f, 36000.f, 52000.f }, {}, { 1.f, 0.82f, 0.62f, 1.f });
            const CapturedImage cloudy = harness.renderReflectedImage(eyePosition, skyDirection);

            double cloudChange = 0.0;
            double spatialVariation = 0.0;
            std::size_t cloudAffectedPixels = 0;
            const CapturedColor& center = cloudy.at(cloudy.mWidth / 2, cloudy.mHeight / 2);
            for (std::size_t index = 0; index < clear.mPixels.size(); ++index)
            {
                const CapturedColor& base = clear.mPixels[index];
                const CapturedColor& reflectedSky = cloudy.mPixels[index];
                const double pixelCloudChange = std::abs(reflectedSky.mRed - base.mRed)
                    + std::abs(reflectedSky.mGreen - base.mGreen) + std::abs(reflectedSky.mBlue - base.mBlue);
                cloudChange += pixelCloudChange;
                if (pixelCloudChange > 0.015)
                    ++cloudAffectedPixels;
                spatialVariation += std::abs(reflectedSky.mRed - center.mRed)
                    + std::abs(reflectedSky.mGreen - center.mGreen) + std::abs(reflectedSky.mBlue - center.mBlue);
                EXPECT_TRUE(std::isfinite(reflectedSky.mRed));
                EXPECT_TRUE(std::isfinite(reflectedSky.mGreen));
                EXPECT_TRUE(std::isfinite(reflectedSky.mBlue));
            }
            const double channelCount = static_cast<double>(clear.mPixels.size() * 3);
            EXPECT_GT(center.mRed + center.mGreen + center.mBlue, 0.1f);
            EXPECT_GT(cloudChange / channelCount, 0.008)
                << "The reflection render must contain the procedural clouds, not only the clear-sky color";
            EXPECT_GT(cloudAffectedPixels, clear.mPixels.size() * 8 / 100)
                << "Organized reflected clouds must affect a material portion of the water-visible sky";
            EXPECT_GT(spatialVariation / channelCount, 0.01)
                << "The reflection render must preserve spatial sky detail instead of collapsing to a flat tint";
        }

        TEST(MWRenderAtmosphereTest, externalVfsDomePreservesDirectionalScattering)
        {
            const std::optional<std::filesystem::path> dataDirectory = getExternalAtmosphereDataDirectory();
            if (!dataDirectory)
                GTEST_SKIP() << "Set OPENMW_ATMOSPHERE_TEST_DATA to exercise the VFS-resolved stock atmosphere mesh";

            osg::ref_ptr<osg::Node> dome = loadVfsAtmosphereDome(*dataDirectory);
            ASSERT_TRUE(dome) << "No meshes/sky_atmosphere.nif in " << dataDirectory->string();
            AtmosphereDomeVisitor reshape;
            dome->accept(reshape);
            ASSERT_TRUE(reshape.reshaped()) << "The VFS-resolved atmosphere is not the supported two-ring stock mesh";
            const MeshVertexBounds vertexBounds = getVertexBounds(*dome);
            ASSERT_TRUE(vertexBounds.mWorld.valid());
            EXPECT_LT(vertexBounds.mWorld.zMin(), -1500.f);
            EXPECT_GT(vertexBounds.mWorld.zMax(), 1500.f);
            AtmosphereRenderHarness harness(dome);
            ASSERT_TRUE(harness.isValid());

            const std::array samples{
                AtmosphereSample{ { 1.f, 0.f, 0.08f } },
                AtmosphereSample{ { -1.f, 0.f, 0.08f } },
                AtmosphereSample{ { 0.f, 1.f, 0.08f } },
                AtmosphereSample{ { 0.f, 0.f, 1.f } },
                AtmosphereSample{ { 1.f, 0.f, -0.08f } },
                AtmosphereSample{ { 0.f, 0.f, -1.f } },
            };
            std::array<CapturedColor, samples.size()> colors;
            for (std::size_t index = 0; index < samples.size(); ++index)
            {
                EXPECT_TRUE(intersectsDome(*dome, samples[index].mDirection));
                colors[index] = harness.render(samples[index].mDirection);
                expectAtmosphereReference(colors[index], samples[index].mDirection);
            }

            EXPECT_GT(colors[0].mRed, colors[1].mRed + 0.1f)
                << "local mesh bounds " << vertexBounds.mLocal.xMin() << ',' << vertexBounds.mLocal.yMin() << ','
                << vertexBounds.mLocal.zMin() << " to " << vertexBounds.mLocal.xMax() << ','
                << vertexBounds.mLocal.yMax() << ',' << vertexBounds.mLocal.zMax() << "; world bounds "
                << vertexBounds.mWorld.xMin() << ',' << vertexBounds.mWorld.yMin() << ',' << vertexBounds.mWorld.zMin()
                << " to " << vertexBounds.mWorld.xMax() << ',' << vertexBounds.mWorld.yMax() << ','
                << vertexBounds.mWorld.zMax() << "; local-to-world basis/translation "
                << vertexBounds.mLocalToWorld(0, 0) << ',' << vertexBounds.mLocalToWorld(0, 1) << ','
                << vertexBounds.mLocalToWorld(0, 2) << ';' << vertexBounds.mLocalToWorld(1, 0) << ','
                << vertexBounds.mLocalToWorld(1, 1) << ',' << vertexBounds.mLocalToWorld(1, 2) << ';'
                << vertexBounds.mLocalToWorld(2, 0) << ',' << vertexBounds.mLocalToWorld(2, 1) << ','
                << vertexBounds.mLocalToWorld(2, 2) << ';' << vertexBounds.mLocalToWorld(3, 0) << ','
                << vertexBounds.mLocalToWorld(3, 1) << ',' << vertexBounds.mLocalToWorld(3, 2);
            EXPECT_GT(colors[0].mRed / colors[0].mBlue, colors[1].mRed / colors[1].mBlue + 0.1f);
            EXPECT_GT(colors[3].mBlue, colors[0].mBlue);
        }

        TEST(MWRenderAtmosphereTest, externalVfsBitterCoastTreeUsesProductionAlphaMaterialPath)
        {
            const std::vector<std::filesystem::path> dataDirectories = getExternalFoliageDataDirectories();
            if (dataDirectories.empty())
                GTEST_SKIP()
                    << "Set OPENMW_FOLIAGE_TEST_DATA to ordered semicolon-separated production data directories";

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->width = 64;
            traits->height = 64;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            traits->glContextVersion = "2.1";
            traits->glContextProfileMask = 0;
            osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
            ASSERT_TRUE(context && context->realize() && context->makeCurrent());
            SceneUtil::GetGLExtensionsOperation getExtensions;
            getExtensions(context);

            constexpr VFS::Path::NormalizedView treePath("meshes/f/flora_bc_tree_01.nif");
            osg::ref_ptr<osg::Node> tree = loadVfsFoliageModel(dataDirectories, treePath);
            ASSERT_TRUE(tree) << "No " << treePath.value() << " in the configured production data directories";

            FoliageMaterialVisitor materialVisitor;
            tree->accept(materialVisitor);
            const FoliageMaterialSummary& summary = materialVisitor.mSummary;
            EXPECT_GT(summary.mGeometryCount, 0u);
            EXPECT_EQ(summary.mBlendedGeometryCount, 0u)
                << "The active MOP tree is submitted through the opaque alpha-test path";
            EXPECT_GT(summary.mAlphaTestedGeometryCount, 0u);
            EXPECT_EQ(summary.mCoverageAdjustedGeometryCount, summary.mAlphaTestedGeometryCount)
                << "Every alpha-tested geometry in the active tree must use production mip-coverage adjustment";
            EXPECT_TRUE(std::ranges::any_of(summary.mTextureNames, [](const std::string& name) {
                return name.find("tx_bc_leaves_02.dds") != std::string::npos;
            })) << "The active tree did not resolve its production Bitter Coast leaf texture";
        }

        TEST(MWRenderAtmosphereTest, externalVfsObjectPagedBitterCoastTreesPreserveFoggedDetail)
        {
            const std::vector<std::filesystem::path> dataDirectories = getExternalFoliageDataDirectories();
            if (dataDirectories.empty())
                GTEST_SKIP()
                    << "Set OPENMW_FOLIAGE_TEST_DATA to ordered semicolon-separated production data directories";

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->width = 64;
            traits->height = 64;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            traits->glContextVersion = "2.1";
            traits->glContextProfileMask = 0;
            osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
            ASSERT_TRUE(context && context->realize() && context->makeCurrent());
            SceneUtil::GetGLExtensionsOperation getExtensions;
            getExtensions(context);

            std::vector<osg::ref_ptr<osg::Node>> trees;
            for (int variant = 1; variant <= 8; ++variant)
            {
                const std::string suffix = "0" + std::to_string(variant);
                const VFS::Path::Normalized modelPath("meshes/f/flora_bc_tree_" + suffix + ".nif");
                osg::ref_ptr<osg::Node> tree = loadVfsFoliageModel(dataDirectories, modelPath);
                ASSERT_TRUE(tree) << modelPath.value();
                trees.push_back(tree);
            }
            osg::ref_ptr<osg::Node> pagedCluster = createObjectPagedFoliageCluster(trees);
            context->releaseContext();
            context->close();

            FoliageMaterialVisitor materialVisitor;
            pagedCluster->accept(materialVisitor);
            EXPECT_GT(materialVisitor.mSummary.mGeometryCount, 0u);
            EXPECT_EQ(materialVisitor.mSummary.mBlendedGeometryCount, 0u);
            EXPECT_EQ(materialVisitor.mSummary.mCoverageAdjustedGeometryCount,
                materialVisitor.mSummary.mAlphaTestedGeometryCount);

            const FoliagePassCapture capture = renderProductionFoliagePass(*pagedCluster, 9000.f);
            ASSERT_FALSE(capture.mColors.empty());
            const std::size_t surfaceCount = static_cast<std::size_t>(std::count_if(
                capture.mDepths.begin(), capture.mDepths.end(), [](float depth) { return depth < 0.999999f; }));
            EXPECT_GT(surfaceCount, 500u) << "clusterRadius=" << pagedCluster->getBound().radius();
        }

        TEST(MWRenderAtmosphereTest, externalVfsBitterCoastTreePreservesRadianceAcrossAlphaCoverageAdjustment)
        {
            const std::vector<std::filesystem::path> dataDirectories = getExternalFoliageDataDirectories();
            if (dataDirectories.empty())
                GTEST_SKIP()
                    << "Set OPENMW_FOLIAGE_TEST_DATA to ordered semicolon-separated production data directories";

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->width = 64;
            traits->height = 64;
            traits->pbuffer = true;
            traits->doubleBuffer = false;
            traits->glContextVersion = "2.1";
            traits->glContextProfileMask = 0;
            osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
            ASSERT_TRUE(context && context->realize() && context->makeCurrent());
            SceneUtil::GetGLExtensionsOperation getExtensions;
            getExtensions(context);

            constexpr VFS::Path::NormalizedView treePath("meshes/f/flora_bc_tree_01.nif");
            osg::ref_ptr<osg::Node> unadjustedTree = loadVfsFoliageModel(dataDirectories, treePath, false);
            osg::ref_ptr<osg::Node> adjustedTree = loadVfsFoliageModel(dataDirectories, treePath, true);
            context->releaseContext();
            context->close();
            ASSERT_TRUE(unadjustedTree && adjustedTree);

            const FoliagePassCapture unadjusted = renderProductionFoliagePass(*unadjustedTree, 20000.f);
            const FoliagePassCapture adjusted = renderProductionFoliagePass(*adjustedTree, 20000.f);
            ASSERT_FALSE(unadjusted.mColors.empty());
            ASSERT_EQ(adjusted.mColors.size(), unadjusted.mColors.size());

            std::size_t adjustedSurfaceCount = 0;
            std::size_t unadjustedSurfaceCount = 0;
            std::size_t nonOpaqueAdjustedSurfaceCount = 0;
            std::size_t sharedSurfaceCount = 0;
            double sharedSurfaceRadianceDifference = 0.0;
            for (std::size_t index = 0; index < adjusted.mColors.size(); ++index)
            {
                const bool adjustedSurface = adjusted.mDepths[index] < 0.999999f;
                const bool unadjustedSurface = unadjusted.mDepths[index] < 0.999999f;
                adjustedSurfaceCount += adjustedSurface;
                unadjustedSurfaceCount += unadjustedSurface;
                nonOpaqueAdjustedSurfaceCount += adjustedSurface && adjusted.mColors[index].w() < 0.98f;
                if (adjustedSurface && unadjustedSurface)
                {
                    for (int channel = 0; channel < 3; ++channel)
                        sharedSurfaceRadianceDifference
                            += std::abs(adjusted.mColors[index][channel] - unadjusted.mColors[index][channel]);
                    ++sharedSurfaceCount;
                }
            }

            EXPECT_GT(unadjustedSurfaceCount, 0u) << "treeRadius=" << adjustedTree->getBound().radius();
            EXPECT_GE(adjustedSurfaceCount, unadjustedSurfaceCount);
            EXPECT_EQ(nonOpaqueAdjustedSurfaceCount, 0u)
                << "Opaque alpha-tested geometry must not expose texture coverage through framebuffer alpha";
            ASSERT_GT(sharedSurfaceCount, 0u);
            EXPECT_LT(sharedSurfaceRadianceDifference / static_cast<double>(sharedSurfaceCount), 1e-5)
                << "Alpha coverage adjustment may change fragment acceptance but not accepted-fragment radiance";
        }
    }
    /// Preserve cached world samples through positive, negative, and discontinuous observer movement.
    TEST(MWRenderAtmosphereTest, cloudTerrainCacheRetainsWorldSamples)
    {
        SceneUtil::AtmosphereTerrainGrid terrain;
        int samples = 0;
        const auto sampleHeight = [&](const osg::Vec2f& position) {
            ++samples;
            return position.x() * 0.02f - position.y() * 0.03f;
        };
        terrain.update({ 100.f, 100.f }, sampleHeight);
        EXPECT_EQ(samples, SceneUtil::AtmosphereTerrainGrid::sSide * SceneUtil::AtmosphereTerrainGrid::sSide);
        const float spacing = SceneUtil::AtmosphereTerrainGrid::sSpacingWorldUnits;
        terrain.update({ spacing - 100.f, spacing - 100.f }, sampleHeight);
        EXPECT_EQ(samples, SceneUtil::AtmosphereTerrainGrid::sSide * SceneUtil::AtmosphereTerrainGrid::sSide);
        terrain.update({ spacing + 100.f, 100.f }, sampleHeight);
        EXPECT_EQ(samples, SceneUtil::AtmosphereTerrainGrid::sSide * (SceneUtil::AtmosphereTerrainGrid::sSide + 1));
        for (const osg::Vec2f center : { osg::Vec2f(-100.f, -100.f), osg::Vec2f(3000000.f, -4000000.f) })
        {
            terrain.update(center, sampleHeight);
            for (int y = 0; y < SceneUtil::AtmosphereTerrainGrid::sSide; ++y)
                for (int x = 0; x < SceneUtil::AtmosphereTerrainGrid::sSide; ++x)
                {
                    const float worldX = terrain.mOriginSpacing.x() + x * terrain.mOriginSpacing.z();
                    const float worldY = terrain.mOriginSpacing.y() + y * terrain.mOriginSpacing.z();
                    const int index = y * SceneUtil::AtmosphereTerrainGrid::sSide + x;
                    const float height = terrain.mHeights[index / 4][index % 4];
                    EXPECT_FLOAT_EQ(height, worldX * 0.02f - worldY * 0.03f);
                    EXPECT_GE(height, terrain.mHeightBounds.x());
                    EXPECT_LE(height, terrain.mHeightBounds.y());
                }
        }
        terrain.update(
            { 3000000.f, -4000000.f }, [](const osg::Vec2f&) { return 123.f; }, true);
        EXPECT_EQ(terrain.mHeightBounds, osg::Vec2f(123.f, 123.f));
    }

    /// Reject invalid terrain evidence without partially replacing a valid published cache.
    TEST(MWRenderAtmosphereTest, cloudTerrainCacheRejectsNonFiniteSamples)
    {
        SceneUtil::AtmosphereTerrainGrid terrain;
        terrain.update({ 0.f, 0.f }, [](const osg::Vec2f&) { return 100.f; });
        const auto heights = terrain.mHeights;
        EXPECT_THROW(terrain.update(
                         { 200000.f, 0.f }, [](const osg::Vec2f&) { return std::numeric_limits<float>::quiet_NaN(); }),
            std::runtime_error);
        EXPECT_EQ(terrain.mHeights, heights);
        EXPECT_EQ(terrain.mHeightBounds, osg::Vec2f(100.f, 100.f));
    }
}
