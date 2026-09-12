#include <apps/openmw/mwrender/gpuscene.hpp>

#include <components/debug/gldebug.hpp>
#include <components/nifosg/controller.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/GLDefines>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/GraphicsThread>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/PrimitiveSet>
#include <osg/Program>
#include <osg/Shader>
#include <osg/State>
#include <osg/StateSet>
#include <osg/Stats>
#include <osg/Texture2D>
#include <osg/Transform>
#include <osg/Uniform>
#include <osg/Viewport>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/UpdateVisitor>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using MWRender::GpuScene;

    constexpr int sSide = 8;
    /// Pools open with two layers.
    constexpr GLint sPoolLayers = 2;
    constexpr int sBudgetSide = 1024;
    constexpr unsigned int sBudgetMib = 1;
    /// Traits value for a compatibility context.
    constexpr unsigned int sCompatibility = 2;
    /// One layer fills the 64 MiB cap.
    constexpr int sCapSide = 4096;
    /// Grace frames after the last use.
    constexpr unsigned int sGrace = 3;
    /// The cull that marks the chunk.
    constexpr unsigned int sMarkFrame = 8;

    void forceMode(const char* mode)
    {
#ifdef _WIN32
        _putenv_s("OPENMW_GPU_MATERIAL_MODE", mode == nullptr ? "" : mode);
#else
        if (mode == nullptr)
            unsetenv("OPENMW_GPU_MATERIAL_MODE");
        else
            setenv("OPENMW_GPU_MATERIAL_MODE", mode, 1);
#endif
    }

    osg::ref_ptr<osg::Image> makeImage(int side, unsigned char seed)
    {
        const std::size_t bytes = static_cast<std::size_t>(side) * side * 4;
        unsigned char* data = new unsigned char[bytes];
        for (std::size_t i = 0; i < bytes; ++i)
            data[i] = static_cast<unsigned char>(seed + i * 7);
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(side, side, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        return image;
    }

    osg::ref_ptr<osg::Image> makeMipImage(int side, unsigned int levels = 0, unsigned char value = 0x5a)
    {
        if (levels == 0)
            levels = osg::Image::computeNumberOfMipmapLevels(side, side);
        osg::Image::MipmapDataType offsets;
        std::size_t bytes = 0;
        for (unsigned int level = 0; level < levels; ++level)
        {
            if (level > 0)
                offsets.push_back(static_cast<unsigned int>(bytes));
            const std::size_t extent = static_cast<std::size_t>(std::max(side >> level, 1));
            bytes += extent * extent * 4;
        }
        unsigned char* data = new unsigned char[bytes];
        std::memset(data, value, bytes);
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(side, side, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        image->setMipmapLevels(offsets);
        return image;
    }

    osg::ref_ptr<osg::Image> makeAlphaImage(unsigned char value)
    {
        const std::size_t bytes = static_cast<std::size_t>(sSide) * sSide;
        unsigned char* data = new unsigned char[bytes];
        std::memset(data, value, bytes);
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(sSide, sSide, 1, GL_ALPHA, GL_ALPHA, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        return image;
    }

    /// Fills a large image without a pattern.
    osg::ref_ptr<osg::Image> makeFlatImage(int width, int height)
    {
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
        unsigned char* data = new unsigned char[bytes];
        std::memset(data, 0x3c, bytes);
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(width, height, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        return image;
    }

    osg::ref_ptr<osg::Texture2D> makeTexture(osg::Image* image)
    {
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setUseHardwareMipMapGeneration(false);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        return texture;
    }

    /// The smallest geometry the packer accepts.
    osg::ref_ptr<osg::Geometry> makeTriangle()
    {
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
        positions->push_back(osg::Vec3f(0.f, 0.f, 0.f));
        positions->push_back(osg::Vec3f(1.f, 0.f, 0.f));
        positions->push_back(osg::Vec3f(0.f, 1.f, 0.f));
        osg::ref_ptr<osg::DrawElementsUShort> indices = new osg::DrawElementsUShort(GL_TRIANGLES);
        for (unsigned short index = 0; index < 3; ++index)
            indices->push_back(index);
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(positions);
        geometry->addPrimitiveSet(indices);
        return geometry;
    }

    class NoopNodeCallback : public osg::NodeCallback
    {
    public:
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override { traverse(node, nv); }
    };

    /// The shader manager cannot instance raw shaders.
    osg::ref_ptr<osg::Program> makeRawProgram()
    {
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX, "void main() { gl_Position = vec4(0.0); }"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT, "void main() { }"));
        return program;
    }

    /// Defines a plain diffuse object shader needs.
    Shader::ShaderManager::DefineMap objectDefines()
    {
        Shader::ShaderManager::DefineMap defines;
        for (const char* name : { "diffuseMap", "darkMap", "detailMap", "decalMap", "emissiveMap", "normalMap",
                 "envMap", "specularMap", "bumpMap", "glossMap" })
        {
            defines[name] = "0";
            defines[std::string(name) + "UV"] = "0";
        }
        defines["diffuseMap"] = "1";
        for (const char* name : { "additiveBlending", "alphaToCoverage", "diffuseParallax", "parallax",
                 "particleOcclusion", "reconstructNormalZ", "softParticles", "adjustCoverage" })
            defines[name] = "0";
        defines["alphaFunc"] = std::to_string(GL_ALWAYS);
        defines["lightingMethodClustered"] = "1";
        defines["useGPUShader4"] = "1";
        return defines;
    }

    double statValue(const GpuScene& scene, const char* name)
    {
        const osg::ref_ptr<osg::Stats> stats = new osg::Stats("gpu");
        scene.reportStats(1, *stats);
        double value = -1.0;
        stats->getAttribute(1, name, value);
        return value;
    }

    void runFrame(GpuScene& scene, unsigned int frame)
    {
        const osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
        stamp->setFrameNumber(frame);
        osgUtil::UpdateVisitor visitor;
        visitor.setFrameStamp(stamp);
        scene.getNode()->accept(visitor);
    }

    /// Culls one node like the renderer.
    void cullNode(osg::Node& node, osg::Camera& camera, unsigned int frame)
    {
        const osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
        stamp->setFrameNumber(frame);
        const osg::ref_ptr<osgUtil::RenderStage> stage = new osgUtil::RenderStage;
        stage->setCamera(&camera);
        const osg::ref_ptr<osgUtil::CullVisitor> cull = new osgUtil::CullVisitor;
        cull->setFrameStamp(stamp);
        cull->setRenderStage(stage);
        cull->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        cull->pushViewport(new osg::Viewport(0, 0, sSide, sSide));
        cull->pushProjectionMatrix(new osg::RefMatrix);
        cull->pushModelViewMatrix(new osg::RefMatrix, osg::Transform::ABSOLUTE_RF);
        node.accept(*cull);
        cull->popModelViewMatrix();
        cull->popProjectionMatrix();
        cull->popViewport();
    }

    /// Reads every layer of the uploaded pool.
    bool readPool(std::vector<unsigned char>& out, GLint expected = GL_RGBA8, GLenum readFormat = GL_RGBA,
        std::size_t pixelBytes = 4, std::array<GLint, 4>* swizzle = nullptr)
    {
        const std::size_t layerBytes = static_cast<std::size_t>(sSide) * sSide * pixelBytes;
        for (GLuint name = 1; name <= 256; ++name)
        {
            if (glIsTexture(name) == GL_FALSE)
                continue;
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            while (glGetError() != GL_NO_ERROR)
            {
            }
            glBindTexture(GL_TEXTURE_2D_ARRAY, name);
            if (glGetError() != GL_NO_ERROR)
                continue;
            GLint width = 0;
            GLint height = 0;
            GLint depth = 0;
            GLint format = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &depth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
            if (width != sSide || height != sSide || depth != sPoolLayers || format != expected)
                continue;
            if (swizzle != nullptr)
                glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_RGBA, swizzle->data());
            out.assign(layerBytes * sPoolLayers, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, readFormat, GL_UNSIGNED_BYTE, out.data());
            const bool read = glGetError() == GL_NO_ERROR;
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            return read;
        }
        return false;
    }

    /// Drives GpuScene against a real offscreen context.
    class GpuSceneArrayTest : public ::testing::Test
    {
    public:
        /// One context serves every test here.
        static void SetUpTestSuite()
        {
            const osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->readDISPLAY();
            traits->setUndefinedScreenDetailsToDefaultScreen();
            traits->width = 8;
            traits->height = 8;
            traits->windowDecoration = false;
            traits->doubleBuffer = false;
            traits->pbuffer = true;
            traits->glContextVersion = "4.3";
            traits->glContextProfileMask = sCompatibility;
            const osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
            if (context == nullptr || !context->realize() || !context->makeCurrent())
                return;
            SceneUtil::GetGLExtensionsOperation()(context);
            sContext = context;
        }

        static void TearDownTestSuite()
        {
            if (sContext == nullptr)
                return;
            sContext->releaseContext();
            sContext->close();
            sContext = nullptr;
        }

    protected:
        void SetUp() override
        {
            if (sContext == nullptr)
                GTEST_SKIP() << "offscreen GL context unavailable";
            forceMode(nullptr);
            mBudget = Settings::terrain().mObjectPagingGpuTextureBudget;
            mShaderManager.setShaderPath(std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "shaders");
            Shader::ShaderManager::DefineMap globals = Shader::getDefaultDefines();
            for (const auto& [name, value] : SceneUtil::ShadowManager::getShadowsDisabledDefines())
                globals[name] = value;
            // Light defines the renderer supplies globally.
            globals["maxLights"] = "16";
            globals["simpleLighting"] = "0";
            mShaderManager.setGlobalDefines(globals);
            const Shader::ShaderManager::DefineMap defines = objectDefines();
            const osg::ref_ptr<osg::Shader> vertex = mShaderManager.getShader("objects.vert", defines);
            const osg::ref_ptr<osg::Shader> fragment = mShaderManager.getShader("objects.frag", defines);
            ASSERT_TRUE(vertex);
            ASSERT_TRUE(fragment);
            mProgram = mShaderManager.getProgram(vertex, fragment, nullptr, { { "diffuseMap", 0 } },
                Shader::ProgramCapability::Instanced | Shader::ProgramCapability::ArrayMaterial
                    | Shader::ProgramCapability::UberMaterial);
            ASSERT_TRUE(mProgram);
        }

        void TearDown() override
        {
            forceMode(nullptr);
            Settings::terrain().mObjectPagingGpuTextureBudget.set(mBudget);
        }

        std::unique_ptr<GpuScene> makeScene()
        {
            auto scene = std::make_unique<GpuScene>(mShaderManager);
            scene->setDefaultMaterial(new SceneUtil::Material);
            return scene;
        }

        osg::ref_ptr<osg::StateSet> makeState(osg::Texture2D& texture)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setAttributeAndModes(mProgram);
            state->addUniform(new osg::Uniform("diffuseMap", 0));
            state->setTextureAttributeAndModes(0, &texture);
            return state;
        }

        static GpuScene::Surface surfaceOf(GpuScene& scene, const osg::StateSet& state, bool live = false)
        {
            const std::vector<const osg::StateSet*> path{ &state };
            return scene.getSurface(path, 0, live ? GpuScene::MaterialUpdate::State : GpuScene::MaterialUpdate::None);
        }

        static osg::ref_ptr<osg::GraphicsContext> sContext;

        Shader::ShaderManager mShaderManager;
        osg::ref_ptr<osg::Program> mProgram;
        int mBudget = -1;
    };

    osg::ref_ptr<osg::GraphicsContext> GpuSceneArrayTest::sContext;

    TEST_F(GpuSceneArrayTest, FallsBackWithoutBindlessRequirements)
    {
        if (!osg::isGLExtensionSupported(sContext->getState()->getContextID(), "GL_ARB_bindless_texture"))
            GTEST_SKIP() << "GL_ARB_bindless_texture unavailable";
        const std::string disabled = osg::getGLExtensionDisableString();
        for (const char* missing : { " GL_NV_gpu_shader5 GL_EXT_nonuniform_qualifier", " GL_ARB_bindless_texture" })
        {
            SCOPED_TRACE(missing);
            osg::setGLExtensionDisableString(disabled + missing);
            const std::unique_ptr<GpuScene> scene = makeScene();
            osg::setGLExtensionDisableString(disabled);
            ASSERT_TRUE(scene->isSupported());
            const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 2));
            const osg::ref_ptr<osg::StateSet> state = makeState(*texture);
            ASSERT_NE(surfaceOf(*scene, *state).mBucket, GpuScene::sInvalid);
            EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
            EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
            scene->compile(*sContext->getState());
            EXPECT_EQ(statValue(*scene, "GPU texture handles"), 0.0);
            scene->releaseGLObjects(sContext->getState());
        }
    }

    TEST_F(GpuSceneArrayTest, SelectsExtBindless)
    {
        const unsigned int context = sContext->getState()->getContextID();
        if (!osg::isGLExtensionSupported(context, "GL_ARB_bindless_texture")
            || !osg::isGLExtensionSupported(context, "GL_EXT_nonuniform_qualifier"))
            GTEST_SKIP() << "EXT bindless requirements unavailable";
        const std::string disabled = osg::getGLExtensionDisableString();
        osg::setGLExtensionDisableString(disabled + " GL_NV_gpu_shader5");
        ::testing::internal::CaptureStdout();
        const std::unique_ptr<GpuScene> scene = makeScene();
        const std::string output = ::testing::internal::GetCapturedStdout();
        osg::setGLExtensionDisableString(disabled);
        ASSERT_TRUE(scene->isSupported());
        EXPECT_NE(output.find("mode=bindless-ext"), std::string::npos);
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 2));
        const osg::ref_ptr<osg::StateSet> state = makeState(*texture);
        ASSERT_NE(surfaceOf(*scene, *state).mBucket, GpuScene::sInvalid);
        scene->compile(*sContext->getState());
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 0.0);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
        EXPECT_EQ(statValue(*scene, "GPU texture handles"), 1.0);
        scene->releaseGLObjects(sContext->getState());
    }

    TEST_F(GpuSceneArrayTest, LogsDriverMessages)
    {
        if (!osg::isGLExtensionSupported(sContext->getState()->getContextID(), "GL_KHR_debug"))
            GTEST_SKIP() << "GL_KHR_debug unavailable";
        using DebugCallback = void(GL_APIENTRY*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);
        void(GL_APIENTRY * insert)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*) = nullptr;
        void(GL_APIENTRY * callback)(DebugCallback, const void*) = nullptr;
        osg::setGLExtensionFuncPtr(insert, "glDebugMessageInsert");
        osg::setGLExtensionFuncPtr(callback, "glDebugMessageCallback");
        ASSERT_NE(insert, nullptr);
        ASSERT_NE(callback, nullptr);
        const GLboolean synchronous = glIsEnabled(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        Debug::EnableGLDebugOperation()(sContext);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        ::testing::internal::CaptureStdout();
        insert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_ERROR, 9319, GL_DEBUG_SEVERITY_LOW, -1, "GPU error test");
        insert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 9320, GL_DEBUG_SEVERITY_HIGH, -1, "GPU high test");
        insert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 9321, GL_DEBUG_SEVERITY_LOW, -1, "GPU noise test");
        for (int i = 0; i < 3; ++i)
            insert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_ERROR, 9319, GL_DEBUG_SEVERITY_LOW, -1, "GPU error test");
        insert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_ERROR, 9319, GL_DEBUG_SEVERITY_LOW, -1, "GPU distinct test");
        const std::string output = ::testing::internal::GetCapturedStdout();
        callback(nullptr, nullptr);
        if (!synchronous)
            glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        EXPECT_NE(output.find("id=9319"), std::string::npos);
        EXPECT_NE(output.find("GPU error test"), std::string::npos);
        EXPECT_NE(output.find("id=9320"), std::string::npos);
        EXPECT_NE(output.find("severity="), std::string::npos);
        EXPECT_NE(output.find("GPU distinct test"), std::string::npos);
        if (!Debug::shouldDebugOpenGL())
        {
            EXPECT_EQ(output.find("GPU noise test"), std::string::npos);
            EXPECT_NE(output.find("count=4"), std::string::npos);
            EXPECT_EQ(output.find("count=3"), std::string::npos);
        }
    }

    TEST_F(GpuSceneArrayTest, UsesProgramSampler)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 2));
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        state->setAttributeAndModes(mProgram);
        state->setTextureAttributeAndModes(0, texture);

        ASSERT_NE(surfaceOf(*scene, *state).mBucket, GpuScene::sInvalid);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
    }

    TEST_F(GpuSceneArrayTest, PacksCompatibleLayers)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Image> first = makeMipImage(sSide, 2, 3);
        const osg::ref_ptr<osg::Image> second = makeMipImage(sSide, 2, 129);
        const osg::ref_ptr<osg::Texture2D> firstTexture = makeTexture(first);
        const osg::ref_ptr<osg::Texture2D> secondTexture = makeTexture(second);
        const osg::ref_ptr<osg::StateSet> firstState = makeState(*firstTexture);
        const osg::ref_ptr<osg::StateSet> secondState = makeState(*secondTexture);

        const GpuScene::Surface left = surfaceOf(*scene, *firstState);
        const GpuScene::Surface right = surfaceOf(*scene, *secondState);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        ASSERT_NE(right.mBucket, GpuScene::sInvalid);
        EXPECT_EQ(left.mBucket, right.mBucket);
        EXPECT_NE(left.mMaterial, right.mMaterial);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);

        scene->compile(*sContext->getState());

        std::vector<unsigned char> texels;
        ASSERT_TRUE(readPool(texels));
        const std::size_t layerBytes = static_cast<std::size_t>(sSide) * sSide * 4;
        ASSERT_EQ(texels.size(), layerBytes * sPoolLayers);
        EXPECT_EQ(std::memcmp(texels.data(), first->data(), layerBytes), 0);
        EXPECT_EQ(std::memcmp(texels.data() + layerBytes, second->data(), layerBytes), 0);
        scene->releaseGLObjects(sContext->getState());
    }

    TEST_F(GpuSceneArrayTest, PacksAlphaLayers)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Image> first = makeAlphaImage(37);
        const osg::ref_ptr<osg::Image> second = makeAlphaImage(181);
        const osg::ref_ptr<osg::Texture2D> firstTexture = makeTexture(first);
        const osg::ref_ptr<osg::Texture2D> secondTexture = makeTexture(second);
        const osg::ref_ptr<osg::StateSet> firstState = makeState(*firstTexture);
        const osg::ref_ptr<osg::StateSet> secondState = makeState(*secondTexture);

        const GpuScene::Surface left = surfaceOf(*scene, *firstState);
        const GpuScene::Surface right = surfaceOf(*scene, *secondState);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        ASSERT_NE(right.mBucket, GpuScene::sInvalid);
        EXPECT_EQ(left.mBucket, right.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);

        scene->compile(*sContext->getState());

        std::vector<unsigned char> texels;
        std::array<GLint, 4> swizzle{};
        ASSERT_TRUE(readPool(texels, GL_R8, GL_RED, 1, &swizzle));
        const std::size_t layerBytes = static_cast<std::size_t>(sSide) * sSide;
        ASSERT_EQ(texels.size(), layerBytes * sPoolLayers);
        EXPECT_EQ(std::memcmp(texels.data(), first->data(), layerBytes), 0);
        EXPECT_EQ(std::memcmp(texels.data() + layerBytes, second->data(), layerBytes), 0);
        EXPECT_EQ(swizzle, (std::array<GLint, 4>{ GL_ONE, GL_ONE, GL_ONE, GL_RED }));
        scene->releaseGLObjects(sContext->getState());
    }

    TEST_F(GpuSceneArrayTest, SplitsSamplerBuckets)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Image> image = makeImage(sSide, 17);
        const osg::ref_ptr<osg::Texture2D> repeated = makeTexture(image);
        repeated->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        const osg::ref_ptr<osg::Texture2D> clamped = makeTexture(image);
        clamped->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        const osg::ref_ptr<osg::StateSet> repeatedState = makeState(*repeated);
        const osg::ref_ptr<osg::StateSet> clampedState = makeState(*clamped);

        const GpuScene::Surface left = surfaceOf(*scene, *repeatedState);
        const GpuScene::Surface right = surfaceOf(*scene, *clampedState);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        ASSERT_NE(right.mBucket, GpuScene::sInvalid);
        EXPECT_NE(left.mBucket, right.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
        EXPECT_EQ(statValue(*scene, "GPU material buckets"), 2.0);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
    }

    TEST_F(GpuSceneArrayTest, SeparatesPoolStorage)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> small = makeTexture(makeImage(sSide, 23));
        const osg::ref_ptr<osg::Texture2D> large = makeTexture(makeImage(sSide * 2, 23));
        const osg::ref_ptr<osg::StateSet> smallState = makeState(*small);
        const osg::ref_ptr<osg::StateSet> largeState = makeState(*large);

        const GpuScene::Surface left = surfaceOf(*scene, *smallState);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
        const GpuScene::Surface right = surfaceOf(*scene, *largeState);
        ASSERT_NE(right.mBucket, GpuScene::sInvalid);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 2.0);
        EXPECT_NE(left.mBucket, right.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
    }

    TEST_F(GpuSceneArrayTest, RejectsAnimatedTexture)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> stable = makeTexture(makeImage(sSide, 31));
        const osg::ref_ptr<osg::Image> animated = makeImage(sSide, 47);
        animated->setDataVariance(osg::Object::DYNAMIC);
        const osg::ref_ptr<osg::Texture2D> live = makeTexture(animated);
        const osg::ref_ptr<osg::StateSet> stableState = makeState(*stable);
        const osg::ref_ptr<osg::StateSet> liveState = makeState(*live);

        const GpuScene::Surface left = surfaceOf(*scene, *stableState);
        const GpuScene::Surface right = surfaceOf(*scene, *liveState);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        ASSERT_NE(right.mBucket, GpuScene::sInvalid);
        EXPECT_NE(left.mBucket, right.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 1.0);
    }

    TEST_F(GpuSceneArrayTest, RejectsOversizedTexture)
    {
        forceMode("array");
        Settings::terrain().mObjectPagingGpuTextureBudget.set(64);
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> packed = makeTexture(makeImage(sSide, 107));
        const osg::ref_ptr<osg::StateSet> packedState = makeState(*packed);
        const GpuScene::Surface pooled = surfaceOf(*scene, *packedState);
        ASSERT_NE(pooled.mBucket, GpuScene::sInvalid);
        ASSERT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);

        // One layer already exceeds the cap.
        const osg::ref_ptr<osg::Texture2D> huge = makeTexture(makeFlatImage(sCapSide, sCapSide));
        const osg::ref_ptr<osg::StateSet> hugeState = makeState(*huge);
        const GpuScene::Surface spilled = surfaceOf(*scene, *hugeState);
        ASSERT_NE(spilled.mBucket, GpuScene::sInvalid);
        EXPECT_NE(spilled.mBucket, pooled.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 1.0);
    }

    TEST_F(GpuSceneArrayTest, RecoversFailedPool)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        GLint maxSide = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSide);
        ASSERT_GT(maxSide, 0);
        // GL refuses storage wider than this.
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeFlatImage(maxSide + 1, 1));
        const osg::ref_ptr<osg::StateSet> state = makeState(*texture);
        const GpuScene::Surface first = surfaceOf(*scene, *state);
        ASSERT_NE(first.mMaterial, GpuScene::sInvalid);
        ASSERT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);

        osg::State& glState = *sContext->getState();
        scene->compile(glState);

        const GpuScene::Surface second = surfaceOf(*scene, *state);
        ASSERT_NE(second.mMaterial, GpuScene::sInvalid);
        EXPECT_NE(second.mMaterial, first.mMaterial);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 1.0);
        scene->releaseGLObjects(&glState);
    }

    TEST_F(GpuSceneArrayTest, RetiresUnusedSurface)
    {
        forceMode("legacy");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 53));
        const osg::ref_ptr<osg::StateSet> state = makeState(*texture);

        const GpuScene::Surface first = surfaceOf(*scene, *state);
        ASSERT_NE(first.mMaterial, GpuScene::sInvalid);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 1.0);

        for (unsigned int frame = 1; frame <= 5; ++frame)
            runFrame(*scene, frame);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);

        const GpuScene::Surface second = surfaceOf(*scene, *state);
        ASSERT_NE(second.mMaterial, GpuScene::sInvalid);
        EXPECT_NE(second.mMaterial, first.mMaterial);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 1.0);
    }

    TEST_F(GpuSceneArrayTest, KeepsMarkedSurface)
    {
        forceMode("legacy");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 61));
        const osg::ref_ptr<osg::Geometry> geometry = makeTriangle();
        geometry->setStateSet(makeState(*texture));
        const std::unique_ptr<const MWRender::GpuTemplate> tmpl = scene->extract(*geometry, ~0u, false);
        ASSERT_TRUE(tmpl);

        std::vector<GpuScene::Instance> instances;
        scene->appendInstances(
            *tmpl, osg::Matrixf::identity(), 1.f, { 0.f, std::numeric_limits<float>::max() }, false, instances);
        ASSERT_FALSE(instances.empty());
        osg::ref_ptr<osg::Node> chunk
            = scene->createChunk(std::move(instances), osg::BoundingSphere(osg::Vec3f(), 1.f));
        ASSERT_TRUE(chunk);

        // Chunk users outlast the grace period.
        for (unsigned int frame = 1; frame <= sMarkFrame; ++frame)
            runFrame(*scene, frame);
        ASSERT_TRUE(scene->isTemplateLive(*tmpl));

        const osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        cullNode(*chunk, *camera, sMarkFrame);
        chunk = nullptr;
        // The mark alone carries the material.
        for (unsigned int frame = sMarkFrame + 1; frame <= sMarkFrame + sGrace; ++frame)
            runFrame(*scene, frame);
        EXPECT_TRUE(scene->isTemplateLive(*tmpl));

        for (unsigned int frame = sMarkFrame + sGrace + 1; frame <= sMarkFrame + sGrace + 2; ++frame)
            runFrame(*scene, frame);
        EXPECT_FALSE(scene->isTemplateLive(*tmpl));
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
    }

    TEST_F(GpuSceneArrayTest, ReleasesRejectedMaterial)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 71));
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        state->setAttributeAndModes(makeRawProgram());
        state->addUniform(new osg::Uniform("diffuseMap", 0));
        state->setTextureAttributeAndModes(0, texture);

        const GpuScene::Surface surface = surfaceOf(*scene, *state);
        EXPECT_EQ(surface.mBucket, GpuScene::sInvalid);
        EXPECT_EQ(surface.mMaterial, GpuScene::sInvalid);
        EXPECT_EQ(statValue(*scene, "GPU texture bytes"), 0.0);
    }

    TEST_F(GpuSceneArrayTest, DropsRetiredTemplate)
    {
        forceMode("legacy");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 73));
        const osg::ref_ptr<osg::Geometry> geometry = makeTriangle();
        geometry->setStateSet(makeState(*texture));

        const std::unique_ptr<const MWRender::GpuTemplate> tmpl = scene->extract(*geometry, ~0u, false);
        ASSERT_TRUE(tmpl);
        ASSERT_FALSE(tmpl->mGeometries.empty());
        EXPECT_TRUE(scene->isTemplateLive(*tmpl));

        for (unsigned int frame = 1; frame <= 5; ++frame)
            runFrame(*scene, frame);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 0.0);
        EXPECT_FALSE(scene->isTemplateLive(*tmpl));
    }

    TEST_F(GpuSceneArrayTest, RejectsStaleChunk)
    {
        forceMode("legacy");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 109));
        const osg::ref_ptr<osg::Geometry> geometry = makeTriangle();
        geometry->setStateSet(makeState(*texture));
        const std::unique_ptr<const MWRender::GpuTemplate> tmpl = scene->extract(*geometry, ~0u, false);
        ASSERT_TRUE(tmpl);
        std::vector<GpuScene::Instance> instances;
        scene->appendInstances(
            *tmpl, osg::Matrixf::identity(), 1.f, { 0.f, std::numeric_limits<float>::max() }, false, instances);
        ASSERT_FALSE(instances.empty());

        for (unsigned int frame = 1; frame <= 5; ++frame)
            runFrame(*scene, frame);
        ASSERT_FALSE(scene->isTemplateLive(*tmpl));

        // Paging re-extracts the template after this.
        EXPECT_FALSE(scene->createChunk(std::move(instances), osg::BoundingSphere(osg::Vec3f(), 1.f)));
    }

    TEST_F(GpuSceneArrayTest, IsolatesLiveMaterial)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 79));
        const osg::ref_ptr<osg::StateSet> firstState = makeState(*texture);
        const osg::ref_ptr<osg::StateSet> secondState = makeState(*texture);

        const GpuScene::Surface left = surfaceOf(*scene, *firstState, true);
        const GpuScene::Surface right = surfaceOf(*scene, *secondState, true);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        ASSERT_NE(right.mBucket, GpuScene::sInvalid);
        EXPECT_NE(left.mBucket, right.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 2.0);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 0.0);

        const osg::ref_ptr<osg::StateSet> thirdState = makeState(*texture);
        const osg::ref_ptr<osg::StateSet> fourthState = makeState(*texture);
        const GpuScene::Surface third = surfaceOf(*scene, *thirdState);
        const GpuScene::Surface fourth = surfaceOf(*scene, *fourthState);
        ASSERT_NE(third.mBucket, GpuScene::sInvalid);
        EXPECT_EQ(third.mBucket, fourth.mBucket);
        EXPECT_NE(third.mBucket, left.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 2.0);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
    }

    TEST_F(GpuSceneArrayTest, KeepsPendingLiveState)
    {
        forceMode("bindless");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 89));
        const osg::ref_ptr<osg::StateSet> state = makeState(*texture);
        ASSERT_NE(surfaceOf(*scene, *state).mMaterial, GpuScene::sInvalid);
        if (statValue(*scene, "GPU array pools") != 0.0 || statValue(*scene, "GPU legacy materials") != 0.0)
            GTEST_SKIP() << "bindless textures unavailable";

        const osg::ref_ptr<NifOsg::UVController> updater = new NifOsg::UVController;
        const osg::ref_ptr<const osg::StateSet> first = scene->liveStateSet(*updater);
        ASSERT_TRUE(first);

        const osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        osg::Group& root = *scene->getNode();
        for (unsigned int i = 0; i < root.getNumChildren(); ++i)
            root.getChild(i)->setNodeMask(0);
        cullNode(root, *camera, 1);
        EXPECT_EQ(scene->liveStateSet(*updater), first);
    }

    TEST_F(GpuSceneArrayTest, RebindsSwappedMap)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> first = makeTexture(makeImage(sSide, 83));
        const osg::ref_ptr<osg::Texture2D> second = makeTexture(makeImage(sSide, 89));
        const osg::ref_ptr<osg::StateSet> state = makeState(*first);
        ASSERT_NE(surfaceOf(*scene, *state, true).mBucket, GpuScene::sInvalid);
        EXPECT_EQ(statValue(*scene, "GPU legacy materials"), 1.0);
        EXPECT_EQ(statValue(*scene, "GPU bucket edits"), 0.0);

        // A flip controller swaps this map.
        state->setTextureAttributeAndModes(0, second);
        scene->refreshSamplers();
        osg::State& glState = *sContext->getState();
        scene->compile(glState);
        EXPECT_EQ(statValue(*scene, "GPU bucket edits"), 1.0);
        scene->releaseGLObjects(&glState);
    }

    TEST_F(GpuSceneArrayTest, ReplacesFilterSamplers)
    {
        forceMode("array");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> first = makeTexture(makeImage(sSide, 97));
        const osg::ref_ptr<osg::Texture2D> second = makeTexture(makeImage(sSide, 101));
        const osg::ref_ptr<osg::StateSet> firstState = makeState(*first);
        const osg::ref_ptr<osg::StateSet> secondState = makeState(*second);
        const GpuScene::Surface left = surfaceOf(*scene, *firstState);
        const GpuScene::Surface right = surfaceOf(*scene, *secondState);
        ASSERT_NE(left.mBucket, GpuScene::sInvalid);
        ASSERT_EQ(left.mBucket, right.mBucket);
        EXPECT_EQ(statValue(*scene, "GPU array pools"), 1.0);
        EXPECT_EQ(statValue(*scene, "GPU bucket edits"), 0.0);

        // The filtering setting changes textures this way.
        first->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        second->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        scene->refreshSamplers();
        osg::State& glState = *sContext->getState();
        scene->compile(glState);
        EXPECT_EQ(statValue(*scene, "GPU bucket edits"), 1.0);

        scene->refreshSamplers();
        scene->compile(glState);
        EXPECT_EQ(statValue(*scene, "GPU bucket edits"), 1.0);
        scene->releaseGLObjects(&glState);
    }

    TEST_F(GpuSceneArrayTest, ReplacesFilterHandles)
    {
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 103));
        const osg::ref_ptr<osg::StateSet> state = makeState(*texture);
        ASSERT_NE(surfaceOf(*scene, *state).mMaterial, GpuScene::sInvalid);
        if (statValue(*scene, "GPU array pools") != 0.0 || statValue(*scene, "GPU legacy materials") != 0.0)
            GTEST_SKIP() << "bindless textures unavailable";

        osg::State& glState = *sContext->getState();
        scene->compile(glState);
        ASSERT_EQ(statValue(*scene, "GPU texture handles"), 1.0);

        // The filtering setting changes textures this way.
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        scene->refreshSamplers();
        scene->compile(glState);
        EXPECT_EQ(statValue(*scene, "GPU texture handles"), 2.0);
        scene->releaseGLObjects(&glState);
    }

    TEST_F(GpuSceneArrayTest, DemotesOverBudget)
    {
        forceMode("legacy");
        Settings::terrain().mObjectPagingGpuTextureBudget.set(static_cast<int>(sBudgetMib));
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeMipImage(sBudgetSide));
        const osg::ref_ptr<osg::StateSet> state = makeState(*texture);
        ASSERT_NE(surfaceOf(*scene, *state).mMaterial, GpuScene::sInvalid);

        osg::State& glState = *sContext->getState();
        const unsigned int context = glState.getContextID();
        // A draw uploads the source like this.
        texture->apply(glState);
        ASSERT_NE(texture->getTextureObject(context), nullptr);

        scene->compile(glState);
        const double budget = statValue(*scene, "GPU texture budget");
        const double bytes = statValue(*scene, "GPU texture bytes");
        EXPECT_EQ(budget, static_cast<double>(sBudgetMib << 20));
        EXPECT_GT(bytes, budget);
        EXPECT_EQ(statValue(*scene, "GPU mip changes"), 0.0);

        scene->compile(glState);
        EXPECT_GT(statValue(*scene, "GPU mip changes"), 0.0);
        // Both copies count until the fence passes.
        EXPECT_GT(statValue(*scene, "GPU texture bytes"), bytes);
        EXPECT_NE(texture->getTextureObject(context), nullptr);
        const double edits = statValue(*scene, "GPU bucket edits");
        EXPECT_GT(edits, 0.0);

        // The filtering setting changes textures this way.
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        scene->refreshSamplers();
        scene->compile(glState);
        EXPECT_GT(statValue(*scene, "GPU bucket edits"), edits);

        const osg::ref_ptr<osg::GraphicsOperation> retire = scene->createRetireOperation();
        ASSERT_TRUE(retire);
        (*retire)(sContext.get());
        scene->releaseGLObjects(&glState);
        EXPECT_LT(statValue(*scene, "GPU texture bytes"), bytes);
        EXPECT_EQ(texture->getTextureObject(context), nullptr);
    }

    TEST_F(GpuSceneArrayTest, KeepsOrderedTemplatesClassic)
    {
        forceMode("legacy");
        const std::unique_ptr<GpuScene> scene = makeScene();
        ASSERT_TRUE(scene->isSupported());

        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeImage(sSide, 139));
        const osg::ref_ptr<osg::Group> root = new osg::Group;
        const osg::ref_ptr<osg::Group> plain = new osg::Group;
        plain->setStateSet(makeState(*texture));
        plain->addChild(makeTriangle());
        root->addChild(plain);

        const osg::ref_ptr<osg::Group> blended = new osg::Group;
        blended->setStateSet(makeState(*texture));
        blended->getStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
        blended->addChild(makeTriangle());
        root->addChild(blended);

        const std::unique_ptr<const MWRender::GpuTemplate> result = scene->extract(*root, ~0u, false);
        ASSERT_NE(result, nullptr);
        EXPECT_TRUE(result->mLegacy);
        EXPECT_EQ(result->mGeometries.size(), 1u);
    }
}
