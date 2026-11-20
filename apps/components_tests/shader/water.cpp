#include <components/sceneutil/offscreencontext.hpp>
#include <components/shader/shadermanager.hpp>

#include <osg/GraphicsContext>
#include <osg/Program>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
    /// Return representative production globals for compiling the compatibility water program without shadows.
    Shader::ShaderManager::DefineMap waterGlobalDefines()
    {
        Shader::ShaderManager::DefineMap defines = Shader::getDefaultDefines();
        defines["lightingMethodClustered"] = "0";
        defines["maxLights"] = "8";
        defines["simpleLighting"] = "0";
        defines["shadows_enabled"] = "0";
        defines["shadow_texture_unit_list"] = "";
        defines["useShadowDebugOverlay"] = "0";
        defines["perspectiveShadowMaps"] = "0";
        defines["disableNormalOffsetShadows"] = "0";
        defines["shadowNormalOffset"] = "0.0";
        defines["limitShadowMapDistance"] = "0";
        defines["shadowFilteringQuality"] = "0";
        return defines;
    }

    /// Return the production defines for one water geometry permutation.
    Shader::ShaderManager::DefineMap waterProgramDefines(bool meshSurface, bool geometricWaves)
    {
        return { { "waterRefraction", "1" }, { "rainRippleDetail", "1" }, { "rippleMapWorldScale", "4.0" },
            { "rippleMapSize", "512.0" }, { "sunlightScattering", "1" }, { "wobblyShores", "1" },
            { "waterGeometricWaves", geometricWaves ? "1" : "0" }, { "waterMeshSurface", meshSurface ? "1" : "0" } };
    }

    /// Compile one production water permutation in a real graphics context.
    osg::ref_ptr<osg::Program> compileWaterProgram(
        Shader::ShaderManager& manager, osg::State& state, bool meshSurface, bool geometricWaves)
    {
        osg::ref_ptr<osg::Program> program
            = manager.getProgram("water", waterProgramDefines(meshSurface, geometricWaves));
        if (program)
            program->compileGLObjects(state);
        return program;
    }

}

TEST(WaterShaderTest, WorldAndAuthoredMeshPermutationsLinkInARealContext)
{
    osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ .mWidth = 32, .mHeight = 32 });
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->realize());
    ASSERT_TRUE(context->makeCurrent());

    Shader::ShaderManager manager;
    manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
    Shader::ShaderManager::DefineMap globals = waterGlobalDefines();
    manager.setGlobalDefines(globals);

    for (const bool meshSurface : { false, true })
    {
        for (const bool geometricWaves : { false, true })
        {
            const osg::ref_ptr<osg::Program> program
                = compileWaterProgram(manager, *context->getState(), meshSurface, geometricWaves);
            ASSERT_TRUE(program);
            const osg::Program::PerContextProgram* linkedProgram = program->getPCP(*context->getState());
            ASSERT_NE(linkedProgram, nullptr);
            std::string informationLog;
            linkedProgram->getInfoLog(informationLog);
            EXPECT_TRUE(linkedProgram->isLinked())
                << (meshSurface ? "mesh-water" : "world-water") << (geometricWaves ? " with waves" : " without waves")
                << ": " << informationLog;
        }
    }

    context->releaseContext();
    context->close();
}

/// Compile the instanced shared-capture reconstruction program with its required GPU-shader capability.
TEST(WaterShaderTest, PlanarReflectionProxyLinksInARealContext)
{
    osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ .mWidth = 32, .mHeight = 32 });
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->realize());
    ASSERT_TRUE(context->makeCurrent());

    Shader::ShaderManager manager;
    manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
    Shader::ShaderManager::DefineMap globals = waterGlobalDefines();
    globals["useGPUShader4"] = "1";
    manager.setGlobalDefines(globals);
    const osg::ref_ptr<osg::Program> program = manager.getProgram("planarreflectionproxy");
    ASSERT_TRUE(program);
    program->compileGLObjects(*context->getState());
    const osg::Program::PerContextProgram* linkedProgram = program->getPCP(*context->getState());
    ASSERT_NE(linkedProgram, nullptr);
    std::string informationLog;
    linkedProgram->getInfoLog(informationLog);
    EXPECT_TRUE(linkedProgram->isLinked()) << informationLog;

    context->releaseContext();
    context->close();
}
