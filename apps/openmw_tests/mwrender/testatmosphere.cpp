#include <gtest/gtest.h>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>

#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Group>
#include <osg/Image>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Viewport>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgViewer/Viewer>

#include "apps/openmw/mwrender/atmosphere.hpp"
#include "apps/openmw/mwrender/skyutil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace MWRender
{
    namespace
    {
        constexpr int captureSize = 128;

        struct AtmosphereSample
        {
            osg::Vec3f mDirection;
        };

        struct CapturedColor
        {
            float mRed;
            float mGreen;
            float mBlue;
        };

        CapturedColor mix(const CapturedColor& from, const CapturedColor& to, float factor)
        {
            return { std::lerp(from.mRed, to.mRed, factor), std::lerp(from.mGreen, to.mGreen, factor),
                std::lerp(from.mBlue, to.mBlue, factor) };
        }

        CapturedColor referenceMgeSky(const osg::Vec3f& requestedDirection)
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
            const CapturedColor weatherBlended = mix(directionalSky, result, 1.f);
            return { std::clamp(weatherBlended.mRed, 0.f, 1.f), std::clamp(weatherBlended.mGreen, 0.f, 1.f),
                std::clamp(weatherBlended.mBlue, 0.f, 1.f) };
        }

        void expectMgeReference(const CapturedColor& actual, const osg::Vec3f& direction)
        {
            const CapturedColor expected = referenceMgeSky(direction);
            constexpr float tolerance = 0.035f;
            EXPECT_NEAR(actual.mRed, expected.mRed, tolerance);
            EXPECT_NEAR(actual.mGreen, expected.mGreen, tolerance);
            EXPECT_NEAR(actual.mBlue, expected.mBlue, tolerance);
        }

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

        osg::ref_ptr<osg::Node> createHarnessDome()
        {
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;

            constexpr int longitudeSegments = 64;
            constexpr int latitudeSegments = 16;
            constexpr float radius = 5000.f;
            for (int latitude = 0; latitude <= latitudeSegments; ++latitude)
            {
                const float elevation = static_cast<float>(latitude) / latitudeSegments * osg::PI_2;
                for (int longitude = 0; longitude <= longitudeSegments; ++longitude)
                {
                    const float azimuth = static_cast<float>(longitude) / longitudeSegments * 2.f * osg::PI;
                    vertices->push_back({ radius * std::cos(elevation) * std::cos(azimuth),
                        radius * std::cos(elevation) * std::sin(azimuth), radius * std::sin(elevation) });
                    colors->push_back({ 1.f, 1.f, 1.f, 1.f });
                }
            }

            geometry->setVertexArray(vertices);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
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

        class AtmosphereRenderHarness
        {
        public:
            explicit AtmosphereRenderHarness(osg::ref_ptr<osg::Node> dome)
            {
                osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
                traits->width = captureSize;
                traits->height = captureSize;
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
                camera->setViewport(new osg::Viewport(0, 0, captureSize, captureSize));
                camera->setProjectionMatrixAsPerspective(70.0, 1.0, 1.0, 20000.0);
                camera->setClearColor(osg::Vec4(0.f, 0.f, 0.f, 1.f));
                camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                mRoot = new osg::Group;
                osg::ref_ptr<osg::Group> sceneRoot = new osg::Group;
                mCameraRelative = new CameraRelativeTransform;
                mCameraRelative->addChild(dome);
                sceneRoot->addChild(mCameraRelative);
                mRoot->addChild(sceneRoot);

                osg::ref_ptr<SceneUtil::SharedUniformStateUpdater> sharedState
                    = new SceneUtil::SharedUniformStateUpdater(0.8f);
                sharedState->setNear(1.f);
                sharedState->setFar(20000.f);
                sharedState->setScreenRes(captureSize, captureSize);
                mRoot->addUpdateCallback(sharedState);

                osg::ref_ptr<SceneUtil::PerViewUniformStateUpdater> perViewState
                    = new SceneUtil::PerViewUniformStateUpdater(nullptr);
                perViewState->setProjectionMatrix(static_cast<osg::Matrixf>(camera->getProjectionMatrix()));
                mRoot->addCullCallback(perViewState);

                mAtmosphereState = new SceneUtil::StateUpdater;
                mAtmosphereState->setFogColor({ 0.887052f, 0.638956f, 0.517662f, 1.f });
                mAtmosphereState->setFogStart(1000.f);
                mAtmosphereState->setFogEnd(20000.f);
                mAtmosphereState->setUnderwaterFogColor({ 0.f, 0.f, 0.f, 1.f });
                mAtmosphereState->setUnderwaterFogStart(1000.f);
                mAtmosphereState->setUnderwaterFogEnd(20000.f);
                mAtmosphereState->setAtmosphere(true, { 0.232298f, 0.361908f, 0.519572f, 1.f }, { 0.25f, 0.38f, 0.48f },
                    { 0.07f, 0.36f, 0.76f }, 1.f, normalized({ 1.f, 0.f, 0.08f }));
                sceneRoot->addUpdateCallback(mAtmosphereState);

                Shader::ShaderManager manager;
                manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
                auto defines = getAtmosphereHarnessDefines();
                manager.setGlobalDefines(defines);
                osg::ref_ptr<osg::Program> program = manager.getProgram("sky");
                if (!program)
                    return;

                osg::StateSet* state = mCameraRelative->getOrCreateStateSet();
                state->setAttributeAndModes(
                    program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                state->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                state->addUniform(new osg::Uniform("pass", 0), osg::StateAttribute::OVERRIDE);
                state->addUniform(new osg::Uniform("diffuseColor", osg::Vec4f(1.f, 1.f, 1.f, 1.f)));

                mViewer.setSceneData(mRoot);
                mViewer.realize();
                mValid = mContext->makeCurrent();
            }

            ~AtmosphereRenderHarness()
            {
                if (mContext)
                {
                    mContext->releaseContext();
                    mContext->close();
                }
            }

            bool isValid() const { return mValid; }

            void setSunDirection(const osg::Vec3f& direction)
            {
                mAtmosphereState->setAtmosphere(true, { 0.232298f, 0.361908f, 0.519572f, 1.f }, { 0.25f, 0.38f, 0.48f },
                    { 0.07f, 0.36f, 0.76f }, 1.f, normalized(direction));
            }

            CapturedColor render(const osg::Vec3f& direction)
            {
                const osg::Vec3f up
                    = std::abs(direction.z()) > 0.99f ? osg::Vec3f(0.f, 1.f, 0.f) : osg::Vec3f(0.f, 0.f, 1.f);
                mViewer.getCamera()->setViewMatrixAsLookAt(osg::Vec3f(), direction, up);
                mViewer.frame();
                mContext->makeCurrent();

                osg::ref_ptr<osg::Image> pixel = new osg::Image;
                pixel->readPixels(captureSize / 2, captureSize / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE);
                const unsigned char* color = pixel->data();
                return { color[0] / 255.f, color[1] / 255.f, color[2] / 255.f };
            }

        private:
            static osg::Vec3f normalized(const osg::Vec3f& value) { return value / value.length(); }

            osgViewer::Viewer mViewer;
            osg::ref_ptr<osg::GraphicsContext> mContext;
            osg::ref_ptr<osg::Group> mRoot;
            osg::ref_ptr<CameraRelativeTransform> mCameraRelative;
            osg::ref_ptr<SceneUtil::StateUpdater> mAtmosphereState;
            bool mValid = false;
        };

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

        struct MeshVertexBounds
        {
            osg::BoundingBox mLocal;
            osg::BoundingBox mWorld;
            osg::Matrix mLocalToWorld;
        };

        MeshVertexBounds getVertexBounds(osg::Node& node)
        {
            class VertexBoundsVisitor : public osg::NodeVisitor
            {
            public:
                VertexBoundsVisitor()
                    : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                {
                }

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

        TEST(MWRenderAtmosphereTest, separatesRenderedAndPhysicalNightSunDirections)
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

        TEST(MWRenderAtmosphereTest, offscreenHarnessSeparatesSolarAndAntiSolarSky)
        {
            AtmosphereRenderHarness harness(createHarnessDome());
            if (!harness.isValid())
                GTEST_SKIP() << "No offscreen compatibility OpenGL context is available";

            const CapturedColor solar = harness.render({ 1.f, 0.f, 0.08f });
            const CapturedColor antiSolar = harness.render({ -1.f, 0.f, 0.08f });
            expectMgeReference(solar, { 1.f, 0.f, 0.08f });
            expectMgeReference(antiSolar, { -1.f, 0.f, 0.08f });
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
                expectMgeReference(colors[index], samples[index].mDirection);
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
    }
}
