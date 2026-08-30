#include <components/sceneutil/offscreencontext.hpp>
#include <components/shader/shadermanager.hpp>

#include <osg/GraphicsContext>
#include <osg/Program>

#include <gtest/gtest.h>

namespace
{
    /// Return the global definitions needed by the linked core vertex shader.
    Shader::ShaderManager::DefineMap makeGlobalDefines()
    {
        Shader::ShaderManager::DefineMap defines = Shader::getDefaultDefines();
        defines["lightingMethodClustered"] = "0";
        defines["maxLights"] = "8";
        defines["simpleLighting"] = "0";
        return defines;
    }

    /// Compile one production shadow-caster program in a real graphics context.
    osg::ref_ptr<osg::Program> compileShadowCaster(Shader::ShaderManager& manager, osg::State& state, bool opaque)
    {
        const std::string opaqueValue = opaque ? "1" : "0";
        const Shader::ShaderManager::DefineMap vertexDefines{ { "opaqueShadowCaster", opaqueValue } };
        const Shader::ShaderManager::DefineMap fragmentDefines{ { "alphaFunc", std::to_string(GL_GEQUAL) },
            { "alphaToCoverage", "0" }, { "adjustCoverage", "1" }, { "useGPUShader4", "0" },
            { "opaqueShadowCaster", opaqueValue } };
        osg::ref_ptr<osg::Program> program = manager.getProgram(manager.getShader("shadowcasting.vert", vertexDefines),
            manager.getShader("shadowcasting.frag", fragmentDefines));
        if (program)
            program->compileGLObjects(state);
        return program;
    }
}

TEST(ShadowCastingShaderTest, OpaqueAndAlphaTestedProgramsLinkInARealContext)
{
    osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ .mWidth = 32, .mHeight = 32 });
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->realize());
    ASSERT_TRUE(context->makeCurrent());

    Shader::ShaderManager manager;
    manager.setShaderPath(std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/shaders");
    Shader::ShaderManager::DefineMap globalDefines = makeGlobalDefines();
    manager.setGlobalDefines(globalDefines);

    for (const bool opaque : { false, true })
    {
        const osg::ref_ptr<osg::Program> program = compileShadowCaster(manager, *context->getState(), opaque);
        ASSERT_TRUE(program);
        const osg::Program::PerContextProgram* linkedProgram = program->getPCP(*context->getState());
        ASSERT_NE(linkedProgram, nullptr);
        std::string informationLog;
        linkedProgram->getInfoLog(informationLog);
        EXPECT_TRUE(linkedProgram->isLinked()) << informationLog;
    }

    context->releaseContext();
    context->close();
}
