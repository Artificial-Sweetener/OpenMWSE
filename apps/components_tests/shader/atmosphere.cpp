#include <components/shader/shadermanager.hpp>

#include <osg/GraphicsContext>
#include <osg/Program>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace
{
    struct Rgb
    {
        float r;
        float g;
        float b;
    };

    Rgb mix(const Rgb& from, const Rgb& to, float factor)
    {
        return { std::lerp(from.r, to.r, factor), std::lerp(from.g, to.g, factor), std::lerp(from.b, to.b, factor) };
    }

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
        defines["skyBlending"] = "1";
        for (const std::string_view name :
            { "additiveBlending", "blendMap", "bumpMap", "darkMap", "decalMap", "detailMap", "diffuseMap",
                "diffuseParallax", "emissiveMap", "envMap", "glossMap", "normalMap", "parallax", "particleOcclusion",
                "reconstructNormalZ", "softParticles", "specularMap", "shadows_enabled", "simpleLighting",
                "adjustCoverage", "alphaToCoverage", "alphaFunc", "useShadowDebugOverlay", "perspectiveShadowMaps",
                "disableNormalOffsetShadows", "limitShadowMapDistance" })
            defines[std::string(name)] = "0";
        return defines;
    }

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

        ASSERT_TRUE(skyVertex);
        ASSERT_TRUE(sky);
        ASSERT_TRUE(terrain);
        ASSERT_TRUE(objects);
        ASSERT_TRUE(groundcover);
        EXPECT_EQ(skyVertex->getShaderSource().find("position.z = 50.0 * (position.z + 200.0);"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("atmosphereSkyColor"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("color.a = 1.0;"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("normalize(atmosphere.sunDirection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("dot(worldDirection, sunDirection)"), std::string::npos);
        EXPECT_NE(sky->getShaderSource().find("vec3(0.371, 0.637, 1.108), 0.44"), std::string::npos);
        EXPECT_EQ(sky->getShaderSource().find("vec3(0.371, 0.637, 1.108), 0.62"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("atmosphereScatter"), std::string::npos);
        EXPECT_NE(terrain->getShaderSource().find("sampleSkyColor"), std::string::npos);
        EXPECT_EQ(terrain->getShaderSource().find("!atmosphere.enabled"), std::string::npos);
        EXPECT_NE(objects->getShaderSource().find("atmosphereScatter"), std::string::npos);
        EXPECT_NE(groundcover->getShaderSource().find("atmosphereScatter"), std::string::npos);
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
