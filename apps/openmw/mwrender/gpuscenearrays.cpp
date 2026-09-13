#include "gpusceneinternal.hpp"

namespace MWRender
{
    namespace
    {
        class CompileOperation : public osg::GraphicsOperation
        {
        public:
            explicit CompileOperation(GpuScene& scene)
                : osg::GraphicsOperation("GpuSceneCompile", true)
                , mScene(scene)
            {
            }

            void operator()(osg::GraphicsContext* context) override
            {
                mScene.compile(*context->getState());
                setKeep(false);
            }

        private:
            GpuScene& mScene;
        };

        class ProbeOperation : public osg::GraphicsOperation
        {
        public:
            explicit ProbeOperation(GpuScene& scene)
                : osg::GraphicsOperation("GpuSceneProbe", true)
                , mScene(scene)
            {
            }

            void operator()(osg::GraphicsContext* context) override
            {
                mScene.probeArrays(*context->getState());
                setKeep(false);
            }

        private:
            GpuScene& mScene;
        };

        constexpr std::string_view sProbeVertex = R"GLSL(#version 430 compatibility
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

        constexpr std::string_view sProbeFragment = R"GLSL(#version 430 compatibility
uniform sampler2DArray probeMap;
void main()
{
    gl_FragColor = texture(probeMap, vec3(gl_FragCoord.xy * 0.25, 0.0));
}
)GLSL";

        /// Object defines an array material variant needs.
        Shader::ShaderManager::DefineMap arrayDefines()
        {
            Shader::ShaderManager::DefineMap defines;
            for (const char* name : Shader::sMaterialMapNames)
            {
                defines[name] = "1";
                defines[std::string(name) + "UV"] = "0";
            }
            for (const char* name : { "additiveBlending", "alphaToCoverage", "diffuseParallax", "parallax",
                     "particleOcclusion", "reconstructNormalZ", "softParticles" })
                defines[name] = "0";
            defines["alphaFunc"] = std::to_string(GL_GREATER);
            defines["adjustCoverage"] = "1";
            defines["useGPUShader4"] = "1";
            defines["gpuUber"] = "1";
            Shader::ShaderManager::applyInstancedDefines(defines, Shader::MaterialMode::Array);
            return defines;
        }
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createCompileOperation()
    {
        return new CompileOperation(*this);
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createProbeOperation()
    {
        if (mArraysProbed.load(std::memory_order_relaxed))
            return nullptr;
        return new ProbeOperation(*this);
    }

    osg::ref_ptr<osg::GraphicsOperation> GpuScene::createRetireOperation()
    {
        if (!mSupported || mRetireOperation != nullptr)
            return nullptr;
        mRetireOperation = new RetireOperation(*this);
        return mRetireOperation;
    }

    void GpuScene::finishFrame(osg::State& state)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTextures->resolve(state);
        }
        if (mGl != nullptr)
            mGl->fenceRetired();
    }

    bool GpuScene::linkArrayShaders(osg::State& state)
    {
        const auto linked = [&](osg::Program* program) {
            if (program == nullptr)
                return false;
            state.applyAttribute(program);
            osg::Program::PerContextProgram* pcp = program->getPCP(state);
            return pcp != nullptr && pcp->isLinked();
        };
        const Shader::ShaderManager::DefineMap defines = arrayDefines();
        osg::ref_ptr<osg::Shader> vertex = mShaderManager.getShader("objects.vert", defines);
        osg::ref_ptr<osg::Shader> fragment = mShaderManager.getShader("objects.frag", defines);
        if (vertex == nullptr || fragment == nullptr)
            return false;
        osg::ref_ptr<osg::Program> objects = mShaderManager.getProgram(vertex, fragment);
        if (objects != nullptr && !objects->getAttribBindingList().contains("instanceSlot"))
            objects->addBindAttribLocation("instanceSlot", Shader::ShaderManager::sInstanceSlotAttribute);
        if (!linked(objects))
            return false;
        const auto casting = std::find_if(mCastingProgramList.begin(), mCastingProgramList.end(),
            [](const osg::ref_ptr<osg::Program>& program) { return program != nullptr; });
        if (casting == mCastingProgramList.end())
            return true;
        return linked(mShaderManager.getInstancedProgram(**casting, Shader::MaterialMode::Array));
    }

    bool GpuScene::sampleArrayLayer(osg::State& state)
    {
        osg::GLExtensions& ext = *mGl->mExt;
        if (ext.glTexStorage3D == nullptr || ext.glTexSubImage3D == nullptr || ext.glGenFramebuffers == nullptr
            || ext.glGenRenderbuffers == nullptr || ext.glGenVertexArrays == nullptr)
            return false;
        constexpr GLsizei size = 4;
        constexpr std::size_t pixelBytes = static_cast<std::size_t>(size) * size * 4;
        std::array<std::uint8_t, pixelBytes> pattern{};
        for (std::size_t i = 0; i < pattern.size(); ++i)
            pattern[i] = static_cast<std::uint8_t>(i * 11 + 3);
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX, std::string(sProbeVertex)));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT, std::string(sProbeFragment)));

        GLuint layers = 0;
        GLuint frame = 0;
        GLuint color = 0;
        GLuint vao = 0;
        glGenTextures(1, &layers);
        ext.glGenFramebuffers(1, &frame);
        ext.glGenRenderbuffers(1, &color);
        ext.glGenVertexArrays(1, &vao);
        clearErrors();
        state.setActiveTextureUnit(sArrayUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, layers);
        if (ext.glBindSampler != nullptr)
            ext.glBindSampler(sArrayUnit, 0);
        ext.glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, size, size, 1);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        resetUnpack();
        ext.glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
        state.applyAttribute(program);
        osg::Program::PerContextProgram* pcp = program->getPCP(state);
        bool valid = pcp != nullptr && pcp->isLinked() && glGetError() == GL_NO_ERROR;
        if (valid)
        {
            std::array<std::uint8_t, pixelBytes> read{};
            const osg::ref_ptr<osg::Uniform> map = new osg::Uniform("probeMap", static_cast<int>(sArrayUnit));
            pcp->apply(*map);
            glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT);
            GLint previousDraw = 0;
            GLint previousRead = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &previousDraw);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &previousRead);
            ext.glBindRenderbuffer(GL_RENDERBUFFER_EXT, color);
            ext.glRenderbufferStorage(GL_RENDERBUFFER_EXT, GL_RGBA8, size, size);
            ext.glBindRenderbuffer(GL_RENDERBUFFER_EXT, 0);
            ext.glBindFramebuffer(GL_FRAMEBUFFER_EXT, frame);
            ext.glFramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
            valid = ext.glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
            if (valid)
            {
                for (const GLenum mode : { GL_SCISSOR_TEST, GL_CULL_FACE, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
                         GL_DITHER, GL_COLOR_LOGIC_OP, GL_ALPHA_TEST, GL_SAMPLE_ALPHA_TO_COVERAGE_ARB })
                    glDisable(mode);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glViewport(0, 0, size, size);
                const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                    ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                    : 0;
                state.bindVertexArrayObject(vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                state.bindVertexArrayObject(previousVao);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
                valid = glGetError() == GL_NO_ERROR && read == pattern;
            }
            ext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, static_cast<GLuint>(previousDraw));
            ext.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, static_cast<GLuint>(previousRead));
            glPopAttrib();
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        glDeleteTextures(1, &layers);
        ext.glDeleteFramebuffers(1, &frame);
        ext.glDeleteRenderbuffers(1, &color);
        ext.glDeleteVertexArrays(1, &vao);
        program->releaseGLObjects(&state);
        clearErrors();
        // Probe objects invalidate OSG state tracking.
        state.setLastAppliedProgramObject(nullptr);
        state.dirtyAllAttributes();
        return valid;
    }

    void GpuScene::probeArrays(osg::State& state)
    {
        if (!checkLimits() || mArraysProbed.load(std::memory_order_relaxed))
            return;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mMaterialMode != Shader::MaterialMode::Array)
            {
                mArraysProbed.store(true, std::memory_order_relaxed);
                return;
            }
        }
        const bool valid = linkArrayShaders(state) && sampleArrayLayer(state);
        mArraysProbed.store(true, std::memory_order_relaxed);
        if (valid)
        {
            Log(Debug::Info) << "GPU paging uses texture arrays.";
            return;
        }
        Log(Debug::Warning) << "GPU texture arrays unavailable; using classic rendering.";
        mSupported = false;
        mDemoted.store(true, std::memory_order_relaxed);
    }
}
