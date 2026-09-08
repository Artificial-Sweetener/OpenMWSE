#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <components/files/configurationmanager.hpp>
#include <components/fx/technique.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/settings/settings.hpp>
#include <components/testing/util.hpp>

#include <osg/Program>

namespace
{
    constexpr VFS::Path::NormalizedView techniquePropertiesPath("shaders/technique_properties.omwfx");

    TestingOpenMW::VFSTestFile techniqueProperties(R"(
    fragment main {}
    vertex main {}
    technique {
        passes = main;
        version = "0.1a";
        description = "description";
        author = "author";
        glsl_version = 330;
        glsl_profile = "compatability";
        glsl_extensions = GL_EXT_gpu_shader4, GL_ARB_uniform_buffer_object;
        flags = disable_sunglare;
        hdr = true;
    }
)");

    constexpr VFS::Path::NormalizedView rendertargetPropertiesPath("shaders/rendertarget_properties.omwfx");

    TestingOpenMW::VFSTestFile rendertargetProperties{ R"(
    render_target rendertarget {
        width_ratio = 0.5;
        height_ratio = 0.5;
        internal_format = r16f;
        source_type = float;
        source_format = red;
        mipmaps = true;
        wrap_s = clamp_to_edge;
        wrap_t = repeat;
        min_filter = linear;
        mag_filter = nearest;
    }
    fragment downsample2x(target=rendertarget) {

        omw_In vec2 omw_TexCoord;

        void main()
        {
            omw_FragColor.r = omw_GetLastShader(omw_TexCoord).r;
        }
    }
    fragment main { }
    technique { passes = downsample2x, main; }
)" };

    constexpr VFS::Path::NormalizedView uniformPropertiesPath("shaders/uniform_properties.omwfx");

    TestingOpenMW::VFSTestFile uniformProperties{ R"(
    uniform_vec4 uVec4 {
        default = vec4(0,0,0,0);
        min = vec4(0,1,0,0);
        max = vec4(0,0,1,0);
        step = 0.5;
        header = "header";
        static = true;
        description = "description";
    }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView missingSamplerSourcePath("shaders/missing_sampler_source.omwfx");

    TestingOpenMW::VFSTestFile missingSamplerSource{ R"(
    sampler_1d mysampler1d { }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView repeatedSharedBlockPath("shaders/repeated_shared_block.omwfx");

    TestingOpenMW::VFSTestFile repeatedSharedBlock{ R"(
    shared {
        float myfloat = 1.0;
    }
    shared {}
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView invalidNumberInfPath("shaders/invalid_number_inf.omwfx");

    TestingOpenMW::VFSTestFile invalidNumberInf{ R"(
    uniform_vec4 uVec4 {
        step = inf;
    }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView invalidNumberNegativeInfPath("shaders/invalid_number_negative_inf.omwfx");

    TestingOpenMW::VFSTestFile invalidNumberNegativeInf{ R"(
    uniform_vec4 uVec4 {
        step = -inf;
    }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView invalidNumberUnsignedLongPath("shaders/invalid_number_ulong.omwfx");

    TestingOpenMW::VFSTestFile invalidNumberUnsignedLong{ R"(
    uniform_vec4 uVec4 {
        step = 18446744073709551615;
    }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView invalidNumberHexFloatPath("shaders/invalid_number_hex.omwfx");

    TestingOpenMW::VFSTestFile invalidNumberHexFloat{ R"(
    uniform_vec4 uVec4 {
        step = 0x1.fffffep+12;
    }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView invalidNumberDoublePath("shaders/invalid_number_double.omwfx");

    TestingOpenMW::VFSTestFile invalidNumberDouble{ R"(
    uniform_vec4 uVec4 {
        step = 1.79769e+50;
    }
    fragment main { }
    technique { passes = main; }
)" };

    constexpr VFS::Path::NormalizedView includeSuccessPath("shaders/include_success.omwfx");
    constexpr VFS::Path::NormalizedView includeValuePath("shaders/lib/include_value.glsl");
    constexpr VFS::Path::NormalizedView includeMissingPath("shaders/include_missing.omwfx");
    constexpr VFS::Path::NormalizedView includeCyclePath("shaders/include_cycle.omwfx");
    constexpr VFS::Path::NormalizedView includeCycleAPath("shaders/lib/include_cycle_a.glsl");
    constexpr VFS::Path::NormalizedView includeCycleBPath("shaders/lib/include_cycle_b.glsl");
    constexpr VFS::Path::NormalizedView includeUnsafePath("shaders/include_unsafe.omwfx");

    TestingOpenMW::VFSTestFile includeSuccess{ R"(
    shared {
        #include "lib/include_value.glsl" // Shared shader source remains authoritative.
    }
    fragment main { }
    technique { passes = main; }
)" };

    TestingOpenMW::VFSTestFile includeValue{ R"(
    float includedValue()
    {
        return 1.0;
    }
)" };

    TestingOpenMW::VFSTestFile includeMissing{ R"(
    #include "lib/does_not_exist.glsl"
    fragment main { }
    technique { passes = main; }
)" };

    TestingOpenMW::VFSTestFile includeCycle{ R"(
    #include "lib/include_cycle_a.glsl"
    fragment main { }
    technique { passes = main; }
)" };

    TestingOpenMW::VFSTestFile includeCycleA{ R"(
    #include "include_cycle_b.glsl"
)" };

    TestingOpenMW::VFSTestFile includeCycleB{ R"(
    #include "include_cycle_a.glsl"
)" };

    TestingOpenMW::VFSTestFile includeUnsafe{ R"(
    #include "../outside.glsl"
    fragment main { }
    technique { passes = main; }
)" };

    using namespace testing;
    using namespace Fx;

    struct TechniqueTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS;
        Resource::ImageManager mImageManager;
        std::unique_ptr<Technique> mTechnique;

        TechniqueTest()
            : mVFS(TestingOpenMW::createTestVFS({
                { techniquePropertiesPath, &techniqueProperties },
                { rendertargetPropertiesPath, &rendertargetProperties },
                { uniformPropertiesPath, &uniformProperties },
                { missingSamplerSourcePath, &missingSamplerSource },
                { repeatedSharedBlockPath, &repeatedSharedBlock },
                { invalidNumberInfPath, &invalidNumberInf },
                { invalidNumberNegativeInfPath, &invalidNumberNegativeInf },
                { invalidNumberUnsignedLongPath, &invalidNumberUnsignedLong },
                { invalidNumberHexFloatPath, &invalidNumberHexFloat },
                { invalidNumberDoublePath, &invalidNumberDouble },
                { includeSuccessPath, &includeSuccess },
                { includeValuePath, &includeValue },
                { includeMissingPath, &includeMissing },
                { includeCyclePath, &includeCycle },
                { includeCycleAPath, &includeCycleA },
                { includeCycleBPath, &includeCycleB },
                { includeUnsafePath, &includeUnsafe },
            }))
            , mImageManager(mVFS.get(), 0)
        {
        }

        void compile(const std::string& name)
        {
            mTechnique = std::make_unique<Technique>(
                *mVFS.get(), mImageManager, Technique::makeFileName(name), name, 1, 1, true, true);
            mTechnique->compile();
        }

        void expectFailure(const std::string& name, std::string_view errorString)
        {
            internal::CaptureStdout();

            compile(name);

            std::string output = internal::GetCapturedStdout();
            Log(Debug::Error) << output;
            EXPECT_THAT(output, HasSubstr(errorString));
        }
    };

    TEST_F(TechniqueTest, technique_properties)
    {
        std::unordered_set<std::string> targetExtensions = { "GL_EXT_gpu_shader4", "GL_ARB_uniform_buffer_object" };

        compile("technique_properties");

        EXPECT_EQ(mTechnique->getVersion(), "0.1a");
        EXPECT_EQ(mTechnique->getDescription(), "description");
        EXPECT_EQ(mTechnique->getAuthor(), "author");
        EXPECT_EQ(mTechnique->getGLSLVersion(), 330);
        EXPECT_EQ(mTechnique->getGLSLProfile(), "compatability");
        EXPECT_EQ(mTechnique->getGLSLExtensions(), targetExtensions);
        EXPECT_EQ(mTechnique->getFlags(), Technique::Flag_Disable_SunGlare);
        EXPECT_EQ(mTechnique->getHDR(), true);
        EXPECT_EQ(mTechnique->getPasses().size(), 1);
        EXPECT_EQ(mTechnique->getPasses().front()->getName(), "main");
    }

    TEST_F(TechniqueTest, rendertarget_properties)
    {
        compile("rendertarget_properties");

        EXPECT_EQ(mTechnique->getRenderTargetsMap().size(), 1);

        const std::string_view name = mTechnique->getRenderTargetsMap().begin()->first;
        auto& rt = mTechnique->getRenderTargetsMap().begin()->second;
        auto& texture = rt.mTarget;

        EXPECT_EQ(name, "rendertarget");
        EXPECT_EQ(rt.mMipMap, true);
        EXPECT_EQ(rt.mSize.mWidthRatio, 0.5f);
        EXPECT_EQ(rt.mSize.mHeightRatio, 0.5f);
        EXPECT_EQ(texture->getWrap(osg::Texture::WRAP_S), osg::Texture::CLAMP_TO_EDGE);
        EXPECT_EQ(texture->getWrap(osg::Texture::WRAP_T), osg::Texture::REPEAT);
        EXPECT_EQ(texture->getFilter(osg::Texture::MIN_FILTER), osg::Texture::LINEAR);
        EXPECT_EQ(texture->getFilter(osg::Texture::MAG_FILTER), osg::Texture::NEAREST);
        EXPECT_EQ(texture->getSourceType(), static_cast<GLenum>(GL_FLOAT));
        EXPECT_EQ(texture->getSourceFormat(), static_cast<GLenum>(GL_RED));
        EXPECT_EQ(texture->getInternalFormat(), static_cast<GLint>(GL_R16F));

        EXPECT_EQ(mTechnique->getPasses().size(), 2);
        EXPECT_EQ(mTechnique->getPasses()[0]->getTarget(), "rendertarget");
    }

    /// Preserve imported shader defines when a technique creates independent stages for a render pass.
    TEST(TechniqueOutputTest, passStagesRetainImportedDefines)
    {
        TestingOpenMW::VFSTestFile file(R"(
            shared {
                #pragma import_defines(TEST_VARIANT)
            }
            vertex main { void main() { omw_Position = omw_Vertex; } }
            fragment main { void main() { omw_FragColor = vec4(1.0); } }
            technique { passes = main; }
        )");
        auto vfs = TestingOpenMW::createTestVFS({ { VFS::Path::NormalizedView("shaders/variants.omwfx"), &file } });
        Resource::ImageManager images(vfs.get(), 0);
        Technique technique(
            *vfs, images, VFS::Path::NormalizedView("shaders/variants.omwfx"), "variants", 1, 1, false, true);
        ASSERT_TRUE(technique.compile()) << technique.getLastError();
        ASSERT_EQ(technique.getPasses().size(), 1u);
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        technique.getPasses()[0]->prepareStateSet(state, "variants");
        auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
        ASSERT_NE(program, nullptr);
        ASSERT_EQ(program->getNumShaders(), 2u);
        EXPECT_EQ(program->getShaderDefines().count("TEST_VARIANT"), 1u);
        for (unsigned int index = 0; index < program->getNumShaders(); ++index)
            EXPECT_EQ(program->getShader(index)->getShaderDefines().count("TEST_VARIANT"), 1u);
        program->addBindUniformBlock("TestBlock", 2);
        for (const auto copy : { osg::CopyOp::SHALLOW_COPY, osg::CopyOp::DEEP_COPY_STATEATTRIBUTES })
        {
            osg::ref_ptr<osg::Program> clone = dynamic_cast<osg::Program*>(program->clone(copy));
            ASSERT_TRUE(clone);
            EXPECT_EQ(clone->getShaderDefines(), program->getShaderDefines());
            EXPECT_EQ(clone->getUniformBlockBindingList(), program->getUniformBlockBindingList());
        }
    }

    TEST(TechniqueOutputTest, validatesSimultaneousOutputContracts)
    {
        const auto check = [](std::string_view header, std::string_view secondarySize, std::string_view error) {
            const std::string source = "render_target color { width_ratio=0.5; height_ratio=0.5; }\n"
                "render_target metadata { " + std::string(secondarySize) + " }\n"
                "fragment integrate(" + std::string(header) + ") { void main() { omw_FragColor=vec4(1.0); } }\n"
                "fragment display {}\ntechnique { passes=integrate,display; }";
            TestingOpenMW::VFSTestFile file(source);
            auto vfs = TestingOpenMW::createTestVFS({ { VFS::Path::NormalizedView("shaders/outputs.omwfx"), &file } });
            Resource::ImageManager images(vfs.get(), 0);
            Technique technique(
                *vfs, images, VFS::Path::NormalizedView("shaders/outputs.omwfx"), "outputs", 64, 64, false, true);
            EXPECT_EQ(technique.compile(), error.empty()) << technique.getLastError();
            if (error.empty())
            {
                ASSERT_EQ(technique.getPasses().size(), 2u);
                EXPECT_EQ(technique.getPasses()[0]->getSecondaryTarget(), "metadata");
            }
            else
                EXPECT_THAT(technique.getLastError(), HasSubstr(error));
        };
        constexpr std::string_view size = "width_ratio=0.5; height_ratio=0.5;";
        check("target=color,secondary_target=metadata", size, "");
        check("secondary_target=metadata", size, "requires a pixel pass with a primary target");
        check("target=color,secondary_target=missing", size, "not defined");
        check("target=color,secondary_target=color", size, "must differ");
        check("target=color,secondary_target=metadata", "width=32; height=32;", "matching size definitions");
        check("target=color,secondary_target=metadata,rt1=metadata", size, "cannot also be pass inputs");
        check("target=color,secondary_target=metadata,rt1=color", size, "cannot also be pass inputs");
    }

    TEST(DispatchNodeTest, frameCopyOwnsAndRebindsRenderTargets)
    {
        DispatchNode source;
        DispatchNode::SubPass producer;
        producer.mTargetName = "history";
        producer.mRenderTexture = new osg::Texture2D;
        producer.mRenderTarget = new osg::FrameBufferObject;
        producer.mRenderTarget->setAttachment(
            osg::Camera::COLOR_BUFFER0, osg::FrameBufferAttachment(producer.mRenderTexture));
        producer.mSecondaryTargetName = "metadata";
        producer.mSecondaryTexture = new osg::Texture2D;
        producer.mRenderTarget->setAttachment(
            osg::Camera::COLOR_BUFFER1, osg::FrameBufferAttachment(producer.mSecondaryTexture));

        DispatchNode::SubPass consumer;
        consumer.mInputTargetNames[0] = "history";
        consumer.mInputTextureUnits[0] = 7;
        consumer.mStateSet->setTextureAttribute(7, producer.mRenderTexture);
        consumer.mInputTargetNames[1] = "metadata";
        consumer.mInputTextureUnits[1] = 8;
        consumer.mStateSet->setTextureAttribute(8, producer.mSecondaryTexture);
        source.mPasses.emplace_back(std::move(producer));
        source.mPasses.emplace_back(std::move(consumer));

        DispatchNode frame(source);
        ASSERT_EQ(frame.mPasses.size(), 2);
        EXPECT_NE(frame.mRootStateSet, source.mRootStateSet);
        frame.mRootStateSet->addUniform(new osg::Uniform("frameHistoryValid", true));
        EXPECT_EQ(source.mRootStateSet->getUniform("frameHistoryValid"), nullptr);
        EXPECT_NE(frame.mPasses[0].mRenderTexture, source.mPasses[0].mRenderTexture);
        EXPECT_EQ(frame.mPasses[1].mStateSet->getTextureAttribute(7, osg::StateAttribute::TEXTURE),
            frame.mPasses[0].mRenderTexture);
        EXPECT_EQ(frame.mPasses[0].mRenderTarget->getAttachment(osg::Camera::COLOR_BUFFER0).getTexture(),
            frame.mPasses[0].mRenderTexture);
        EXPECT_NE(frame.mPasses[0].mSecondaryTexture, source.mPasses[0].mSecondaryTexture);
        EXPECT_EQ(frame.mPasses[1].mStateSet->getTextureAttribute(8, osg::StateAttribute::TEXTURE),
            frame.mPasses[0].mSecondaryTexture);
        EXPECT_EQ(frame.mPasses[0].mRenderTarget->getAttachment(osg::Camera::COLOR_BUFFER1).getTexture(),
            frame.mPasses[0].mSecondaryTexture);
        EXPECT_EQ(frame.mPasses[0].findOutput("metadata"), frame.mPasses[0].mSecondaryTexture);
        EXPECT_EQ(frame.mPasses[0].findOutput("missing"), nullptr);
    }

    TEST_F(TechniqueTest, uniform_properties)
    {
        compile("uniform_properties");

        EXPECT_EQ(mTechnique->getUniformMap().size(), 1);

        const auto& uniform = mTechnique->getUniformMap().front();

        EXPECT_TRUE(uniform->mStatic);
        EXPECT_DOUBLE_EQ(uniform->mStep, 0.5);
        EXPECT_EQ(uniform->getDefault<osg::Vec4f>(), osg::Vec4f(0, 0, 0, 0));
        EXPECT_EQ(uniform->getMin<osg::Vec4f>(), osg::Vec4f(0, 1, 0, 0));
        EXPECT_EQ(uniform->getMax<osg::Vec4f>(), osg::Vec4f(0, 0, 1, 0));
        EXPECT_EQ(uniform->mHeader, "header");
        EXPECT_EQ(uniform->mDescription, "description");
        EXPECT_EQ(uniform->mName, "uVec4");
    }

    TEST_F(TechniqueTest, fail_with_missing_source_for_sampler)
    {
        expectFailure("missing_sampler_source", "sampler_1d 'mysampler1d' requires a filename");
    }

    TEST_F(TechniqueTest, fail_with_repeated_shared_block)
    {
        expectFailure("repeated_shared_block", "repeated 'shared' block");
    }

    TEST_F(TechniqueTest, fail_with_invalid_float)
    {
        expectFailure("invalid_number_inf", "expected float value");
        expectFailure("invalid_number_negative_inf", "expected float value");
        expectFailure("invalid_number_hex", "expected float value");
    }

    TEST_F(TechniqueTest, fail_with_out_of_range)
    {
        expectFailure("invalid_number_ulong", "number out of range");
        expectFailure("invalid_number_double", "number out of range");
    }

    TEST_F(TechniqueTest, expands_shader_includes_from_the_virtual_filesystem)
    {
        compile("include_success");

        ASSERT_TRUE(mTechnique->isValid());
        EXPECT_EQ(mTechnique->getPasses().size(), 1);
    }

    TEST_F(TechniqueTest, fails_when_shader_include_is_missing)
    {
        expectFailure(
            "include_missing", "shaders/lib/does_not_exist.glsl' requested by 'shaders/include_missing.omwfx'");
    }

    TEST_F(TechniqueTest, fails_when_shader_includes_form_a_cycle)
    {
        expectFailure("include_cycle", "cyclic shader include involving 'shaders/lib/include_cycle_a.glsl'");
    }

    TEST_F(TechniqueTest, fails_when_shader_include_escapes_its_directory)
    {
        expectFailure("include_unsafe", "unsafe shader include path '../outside.glsl'");
    }
}
