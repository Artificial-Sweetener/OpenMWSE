#include <components/sceneutil/clusteredlighting.hpp>
#include <components/shader/materialmode.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/testing/util.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <osg/BufferObject>
#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/Shader>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
    using namespace testing;
    using namespace Shader;

    constexpr std::string_view sBindlessRequire = "#extension GL_ARB_bindless_texture : require";
    constexpr std::string_view sVersion120 = "#version 120";
    constexpr ProgramCapability sMaterialCapabilities = ProgramCapability::Instanced | ProgramCapability::ArrayMaterial;
    constexpr ProgramCapability sObjectCapabilities = sMaterialCapabilities | ProgramCapability::UberMaterial;

    constexpr std::string_view sVertexTemplate
        = "#version 120\n"
          "#define INSTANCED @gpuInstanced\n"
          "#define LEGACY_BINDINGS @gpuLegacyBindings\n"
          "#define ARRAY_BINDINGS @gpuArrayBindings\n"
          "#define BINDLESS_BINDINGS @gpuBindlessBindings\n"
          "#define LIGHT_LIST @clusteredClassic\n"
          "#define UBER @gpuUber\n"
          "#define PPL @forcePPL\n"
          "void main() { gl_Position = ftransform(); }\n";

    constexpr std::string_view sFragmentTemplate
        = "#version 120\n"
          "void main() { gl_FragColor = vec4(1.0); }\n";

    std::string shaderSource(const osg::Program& program, unsigned int index)
    {
        return program.getShader(index)->getShaderSource();
    }

    std::string programSource(const osg::Program& program)
    {
        std::string result;
        for (unsigned int i = 0; i < program.getNumShaders(); ++i)
            result += shaderSource(program, i);
        return result;
    }

    struct InstancedProgramTest : Test
    {
        ShaderManager mManager;

        InstancedProgramTest()
        {
            mManager.setShaderPath(TestingOpenMW::outputDir());
            ShaderManager::DefineMap globals{ { "classicFalloff", "0" } };
            mManager.setGlobalDefines(globals);
        }

        static std::string writeTemplate(std::string_view suffix, std::string_view content)
        {
            const std::string name
                = "lib/" + std::string(UnitTest::GetInstance()->current_test_info()->name()) + std::string(suffix);
            const std::filesystem::path path = TestingOpenMW::outputFilePathWithSubDir(std::filesystem::path(name));
            std::ofstream stream(path);
            stream << content;
            stream.close();
            return name;
        }

        // Registers both shaders so twins resolve.
        osg::ref_ptr<osg::Program> buildProgram(std::string_view vertexSuffix, std::string_view fragmentSuffix,
            const ShaderManager::DefineMap& extra = {}, ProgramCapability capabilities = sMaterialCapabilities,
            const ShaderManager::SamplerBindingMap& samplers = {})
        {
            const std::string vertexName = writeTemplate(vertexSuffix, sVertexTemplate);
            const std::string fragmentName = writeTemplate(fragmentSuffix, sFragmentTemplate);
            ShaderManager::DefineMap defines = getDefaultDefines();
            defines.erase("classicFalloff");
            for (const auto& [key, value] : extra)
                defines[key] = value;
            const osg::ref_ptr<osg::Shader> vertex = mManager.getShader(vertexName, defines);
            const osg::ref_ptr<osg::Shader> fragment = mManager.getShader(fragmentName, defines);
            if (vertex == nullptr || fragment == nullptr)
                return nullptr;
            return mManager.getProgram(vertex, fragment, nullptr, samplers, capabilities);
        }
    };

    TEST_F(InstancedProgramTest, modes_get_distinct_programs)
    {
        const osg::ref_ptr<osg::Program> program = buildProgram("_objects.vert", "_terrain.frag");
        ASSERT_TRUE(program);

        const osg::ref_ptr<osg::Program> legacy = mManager.getInstancedProgram(*program, MaterialMode::Legacy);
        const osg::ref_ptr<osg::Program> array = mManager.getInstancedProgram(*program, MaterialMode::Array);
        const osg::ref_ptr<osg::Program> bindless = mManager.getInstancedProgram(*program, MaterialMode::Bindless);
        ASSERT_TRUE(legacy);
        ASSERT_TRUE(array);
        ASSERT_TRUE(bindless);

        EXPECT_NE(legacy.get(), program.get());
        EXPECT_NE(legacy.get(), array.get());
        EXPECT_NE(legacy.get(), bindless.get());
        EXPECT_NE(array.get(), bindless.get());

        EXPECT_EQ(legacy.get(), mManager.getInstancedProgram(*program, MaterialMode::Legacy).get());
        EXPECT_EQ(array.get(), mManager.getInstancedProgram(*program, MaterialMode::Array).get());
        EXPECT_EQ(bindless.get(), mManager.getInstancedProgram(*program, MaterialMode::Bindless).get());
    }

    TEST_F(InstancedProgramTest, twins_keep_version_120)
    {
        const osg::ref_ptr<osg::Program> program = buildProgram("_objects.vert", "_terrain.frag");
        ASSERT_TRUE(program);

        for (const MaterialMode mode : { MaterialMode::Legacy, MaterialMode::Array, MaterialMode::Bindless })
        {
            const osg::ref_ptr<osg::Program> twin = mManager.getInstancedProgram(*program, mode);
            ASSERT_TRUE(twin);
            EXPECT_THAT(shaderSource(*twin, 0), StartsWith(sVersion120));
            EXPECT_THAT(shaderSource(*twin, 1), StartsWith(sVersion120));
            EXPECT_THAT(programSource(*twin), Not(HasSubstr(sBindlessRequire)));
            EXPECT_THAT(programSource(*twin), Not(HasSubstr("GPU_MATERIAL_ARRAY")));
        }
    }

    TEST_F(InstancedProgramTest, modes_select_bindings)
    {
        const osg::ref_ptr<osg::Program> program = buildProgram("_objects.vert", "_terrain.frag");
        ASSERT_TRUE(program);

        EXPECT_THAT(shaderSource(*program, 0), HasSubstr("#define INSTANCED 0"));
        EXPECT_THAT(shaderSource(*program, 0), HasSubstr("#define LEGACY_BINDINGS 1"));
        EXPECT_THAT(shaderSource(*program, 0), HasSubstr("#define LIGHT_LIST 0"));

        const osg::ref_ptr<osg::Program> legacy = mManager.getInstancedProgram(*program, MaterialMode::Legacy);
        const osg::ref_ptr<osg::Program> array = mManager.getInstancedProgram(*program, MaterialMode::Array);
        const osg::ref_ptr<osg::Program> bindless = mManager.getInstancedProgram(*program, MaterialMode::Bindless);
        ASSERT_TRUE(legacy);
        ASSERT_TRUE(array);
        ASSERT_TRUE(bindless);

        for (const osg::Program* twin : { legacy.get(), array.get(), bindless.get() })
        {
            EXPECT_THAT(shaderSource(*twin, 0), HasSubstr("#define INSTANCED 1"));
            EXPECT_THAT(shaderSource(*twin, 0), HasSubstr("#define LIGHT_LIST 0"));
        }
        EXPECT_THAT(shaderSource(*legacy, 0), HasSubstr("#define LEGACY_BINDINGS 1"));
        EXPECT_THAT(shaderSource(*legacy, 0), HasSubstr("#define ARRAY_BINDINGS 0"));
        EXPECT_THAT(shaderSource(*legacy, 0), HasSubstr("#define BINDLESS_BINDINGS 0"));
        EXPECT_THAT(shaderSource(*array, 0), HasSubstr("#define LEGACY_BINDINGS 0"));
        EXPECT_THAT(shaderSource(*array, 0), HasSubstr("#define ARRAY_BINDINGS 1"));
        EXPECT_THAT(shaderSource(*array, 0), HasSubstr("#define BINDLESS_BINDINGS 0"));
        EXPECT_THAT(shaderSource(*bindless, 0), HasSubstr("#define LEGACY_BINDINGS 0"));
        EXPECT_THAT(shaderSource(*bindless, 0), HasSubstr("#define ARRAY_BINDINGS 0"));
        EXPECT_THAT(shaderSource(*bindless, 0), HasSubstr("#define BINDLESS_BINDINGS 1"));
    }

    TEST_F(InstancedProgramTest, clustered_lighting_keeps_cluster_bodies)
    {
        const osg::ref_ptr<osg::Program> program
            = buildProgram("_objects.vert", "_terrain.frag", { { "lightingMethodClustered", "1" } });
        ASSERT_TRUE(program);

        const osg::ref_ptr<osg::Program> twin = mManager.getInstancedProgram(*program, MaterialMode::Bindless);
        ASSERT_TRUE(twin);
        EXPECT_THAT(shaderSource(*twin, 0), HasSubstr("#define LIGHT_LIST 0"));
    }

    TEST_F(InstancedProgramTest, refreshesLightingMode)
    {
        ShaderManager::DefineMap globals{ { "lightingMethodClustered", "1" } };
        mManager.setGlobalDefines(globals);
        const auto program = buildProgram("_objects.vert", "_objects.frag");
        ASSERT_TRUE(program);
        std::vector<osg::ref_ptr<osg::Program>> twins;
        for (auto mode : { MaterialMode::Legacy, MaterialMode::Array, MaterialMode::Bindless })
        {
            twins.push_back(mManager.getInstancedProgram(*program, mode));
            ASSERT_TRUE(twins.back());
        }
        for (const char* classicFalloff : { "0", "1", "0" })
        {
            globals["classicFalloff"] = classicFalloff;
            mManager.setGlobalDefines(globals);
            for (const auto& twin : twins)
                EXPECT_THAT(shaderSource(*twin, 0), HasSubstr(std::string("#define LIGHT_LIST ") + classicFalloff));
        }
    }

    TEST_F(InstancedProgramTest, keepsLocalLightingMode)
    {
        const auto program = buildProgram("_objects.vert", "_objects.frag", { { "classicFalloff", "0" } });
        ASSERT_TRUE(program);
        const auto twin = mManager.getInstancedProgram(*program, MaterialMode::Bindless);
        ASSERT_TRUE(twin);
        ShaderManager::DefineMap globals{ { "classicFalloff", "1" } };
        mManager.setGlobalDefines(globals);
        EXPECT_THAT(shaderSource(*twin, 0), HasSubstr("#define LIGHT_LIST 0"));
    }

    TEST_F(InstancedProgramTest, array_needs_array_capability)
    {
        const osg::ref_ptr<osg::Program> program
            = buildProgram("_objects.vert", "_water.frag", {}, ProgramCapability::Instanced);
        ASSERT_TRUE(program);

        EXPECT_FALSE(mManager.getInstancedProgram(*program, MaterialMode::Array));
        EXPECT_TRUE(mManager.getInstancedProgram(*program, MaterialMode::Bindless));
        EXPECT_TRUE(mManager.getInstancedProgram(*program, MaterialMode::Legacy));
    }

    TEST_F(InstancedProgramTest, no_capability_gets_no_twin)
    {
        const osg::ref_ptr<osg::Program> program
            = buildProgram("_particle.vert", "_terrain.frag", {}, ProgramCapability::None);
        ASSERT_TRUE(program);

        EXPECT_FALSE(mManager.getInstancedProgram(*program, MaterialMode::Legacy));
        EXPECT_FALSE(mManager.getInstancedProgram(*program, MaterialMode::Array));
        EXPECT_FALSE(mManager.getInstancedProgram(*program, MaterialMode::Bindless));
    }

    TEST_F(InstancedProgramTest, uber_merges_map_sets)
    {
        const ShaderManager::DefineMap darkDefines = { { "diffuseMap", "1" }, { "darkMap", "1" }, { "parallax", "1" } };
        const ShaderManager::DefineMap detailDefines
            = { { "diffuseMap", "1" }, { "detailMap", "1" }, { "parallax", "1" } };
        const osg::ref_ptr<osg::Program> dark = buildProgram("_objects.vert", "_objects.frag", darkDefines,
            sObjectCapabilities, { { "diffuseMap", 0 }, { "darkMap", 1 }, { "shadowMap0", 7 } });
        const osg::ref_ptr<osg::Program> detail = buildProgram("_objects.vert", "_objects.frag", detailDefines,
            sObjectCapabilities, { { "diffuseMap", 0 }, { "detailMap", 1 }, { "shadowMap0", 7 } });
        ASSERT_TRUE(dark);
        ASSERT_TRUE(detail);
        EXPECT_NE(dark.get(), detail.get());

        const osg::ref_ptr<osg::Program> uber = mManager.getInstancedProgram(*dark, MaterialMode::Bindless);
        ASSERT_TRUE(uber);
        EXPECT_EQ(uber.get(), mManager.getInstancedProgram(*detail, MaterialMode::Bindless).get());
        EXPECT_THAT(shaderSource(*uber, 0), HasSubstr("#define UBER 1"));
        EXPECT_FALSE(mManager.getSamplerUnit(*uber, "diffuseMap"));
        EXPECT_FALSE(mManager.getSamplerUnit(*uber, "darkMap"));
        EXPECT_FALSE(mManager.getSamplerUnit(*uber, "detailMap"));
        EXPECT_EQ(mManager.getSamplerUnit(*uber, "shadowMap0"), 7);

        const osg::ref_ptr<osg::Program> legacy = mManager.getInstancedProgram(*dark, MaterialMode::Legacy);
        ASSERT_TRUE(legacy);
        EXPECT_NE(legacy.get(), mManager.getInstancedProgram(*detail, MaterialMode::Legacy).get());
        EXPECT_THAT(shaderSource(*legacy, 0), HasSubstr("#define UBER 0"));
        EXPECT_EQ(mManager.getSamplerUnit(*legacy, "darkMap"), 1);
    }

    TEST_F(InstancedProgramTest, array_keeps_sampler_bindings)
    {
        const osg::ref_ptr<osg::Program> dark
            = buildProgram("_objects.vert", "_objects.frag", { { "diffuseMap", "1" }, { "darkMap", "1" } },
                sObjectCapabilities, { { "diffuseMap", 0 }, { "darkMap", 1 } });
        const osg::ref_ptr<osg::Program> detail
            = buildProgram("_objects.vert", "_objects.frag", { { "diffuseMap", "1" }, { "detailMap", "1" } },
                sObjectCapabilities, { { "diffuseMap", 0 }, { "detailMap", 1 } });
        ASSERT_TRUE(dark);
        ASSERT_TRUE(detail);
        const osg::ref_ptr<osg::Program> darkArray = mManager.getInstancedProgram(*dark, MaterialMode::Array);
        const osg::ref_ptr<osg::Program> detailArray = mManager.getInstancedProgram(*detail, MaterialMode::Array);
        ASSERT_TRUE(darkArray);
        ASSERT_TRUE(detailArray);
        EXPECT_NE(darkArray, detailArray);
        EXPECT_NE(darkArray->compare(*detailArray), 0);
        EXPECT_EQ(darkArray->compare(*ShaderManager::cloneProgram(darkArray)), 0);
        EXPECT_EQ(darkArray->getShader(0), detailArray->getShader(0));
        EXPECT_EQ(darkArray->getShader(1), detailArray->getShader(1));
        EXPECT_EQ(mManager.getSamplerUnit(*darkArray, "darkMap"), 1);
        EXPECT_FALSE(mManager.getSamplerUnit(*darkArray, "detailMap"));
        EXPECT_EQ(mManager.getSamplerUnit(*detailArray, "detailMap"), 1);
        EXPECT_FALSE(mManager.getSamplerUnit(*detailArray, "darkMap"));
        EXPECT_EQ(mManager.getInstancedProgram(*dark, MaterialMode::Array), darkArray);
        EXPECT_EQ(mManager.getInstancedProgram(*detail, MaterialMode::Array), detailArray);
    }

    TEST_F(InstancedProgramTest, bindless_keeps_shadow_bindings)
    {
        const osg::ref_ptr<osg::Program> first = buildProgram(
            "_objects.vert", "_objects.frag", {}, sObjectCapabilities, { { "diffuseMap", 0 }, { "shadowMap0", 6 } });
        const osg::ref_ptr<osg::Program> second = buildProgram(
            "_objects.vert", "_objects.frag", {}, sObjectCapabilities, { { "diffuseMap", 0 }, { "shadowMap0", 7 } });
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        const osg::ref_ptr<osg::Program> firstTwin = mManager.getInstancedProgram(*first, MaterialMode::Bindless);
        const osg::ref_ptr<osg::Program> secondTwin = mManager.getInstancedProgram(*second, MaterialMode::Bindless);
        ASSERT_TRUE(firstTwin);
        ASSERT_TRUE(secondTwin);
        EXPECT_NE(firstTwin, secondTwin);
        EXPECT_EQ(mManager.getSamplerUnit(*firstTwin, "shadowMap0"), 6);
        EXPECT_EQ(mManager.getSamplerUnit(*secondTwin, "shadowMap0"), 7);
    }

    TEST_F(InstancedProgramTest, cache_keeps_capabilities)
    {
        const osg::ref_ptr<osg::Program> classic
            = buildProgram("_objects.vert", "_objects.frag", {}, ProgramCapability::None);
        const osg::ref_ptr<osg::Program> instanced = buildProgram("_objects.vert", "_objects.frag");
        ASSERT_TRUE(classic);
        ASSERT_TRUE(instanced);
        EXPECT_NE(classic, instanced);
        EXPECT_NE(classic->compare(*instanced), 0);
        EXPECT_FALSE(mManager.getInstancedProgram(*classic, MaterialMode::Array));
        EXPECT_TRUE(mManager.getInstancedProgram(*instanced, MaterialMode::Array));
    }

    TEST_F(InstancedProgramTest, uber_keeps_lighting_split)
    {
        // The visitor resolves forcePPL before twinning.
        const ShaderManager::DefineMap litDefines
            = { { "diffuseMap", "1" }, { "normalMap", "1" }, { "forcePPL", "1" } };
        const ShaderManager::DefineMap darkDefines = { { "diffuseMap", "1" }, { "darkMap", "1" } };
        const osg::ref_ptr<osg::Program> lit
            = buildProgram("_objects.vert", "_objects.frag", litDefines, sObjectCapabilities);
        const osg::ref_ptr<osg::Program> dark
            = buildProgram("_objects.vert", "_objects.frag", darkDefines, sObjectCapabilities);
        ASSERT_TRUE(lit);
        ASSERT_TRUE(dark);

        const osg::ref_ptr<osg::Program> litUber = mManager.getInstancedProgram(*lit, MaterialMode::Bindless);
        const osg::ref_ptr<osg::Program> darkUber = mManager.getInstancedProgram(*dark, MaterialMode::Bindless);
        ASSERT_TRUE(litUber);
        ASSERT_TRUE(darkUber);
        EXPECT_NE(litUber.get(), darkUber.get());
        EXPECT_THAT(shaderSource(*litUber, 0), HasSubstr("#define PPL 1"));
        EXPECT_THAT(shaderSource(*darkUber, 0), HasSubstr("#define PPL 0"));
    }

    // OPENMW_TEST_SHADER_DIR selects shipped templates.
    struct ShaderTemplateTest : Test
    {
        std::filesystem::path mDir;
        std::filesystem::path mDumpDir;
        ShaderManager mManager;

        void SetUp() override
        {
            const char* dir = std::getenv("OPENMW_TEST_SHADER_DIR");
            if (dir == nullptr)
                GTEST_SKIP() << "OPENMW_TEST_SHADER_DIR is not set";
            mDir = dir;
            if (const char* dump = std::getenv("OPENMW_TEST_SHADER_DUMP_DIR"))
            {
                mDumpDir = dump;
                std::filesystem::create_directories(mDumpDir);
            }
            mManager.setShaderPath(mDir);
            ShaderManager::DefineMap globals = collectDefaults();
            mManager.setGlobalDefines(globals);
        }

        // Unset tokens use zero.
        ShaderManager::DefineMap collectDefaults() const
        {
            ShaderManager::DefineMap defines = getDefaultDefines();
            const std::map<std::string, std::string> known = {
                { "maxLights", "8" },
                { "alphaFunc", "516" },
                { "numViews", "1" },
                { "distorionRTRatio", "1" },
                { "groundcoverFadeStart", "1000.0" },
                { "groundcoverFadeEnd", "2000.0" },
                { "groundcoverStompMode", "0" },
                { "groundcoverStompIntensity", "0" },
                { "rippleMapSize", "512.0" },
                { "rippleMapWorldScale", "2.5" },
                { "shadow_texture_unit_list", "" },
                { "lightingMethodClustered", "0" },
                { "legacyLightList", "1" },
            };
            for (const auto& [key, value] : known)
                defines[key] = value;
            const std::regex token("@([A-Za-z_][A-Za-z0-9_]*)");
            const std::regex iterator("@foreach\\s+([A-Za-z_][A-Za-z0-9_]*)");
            std::set<std::string> iterators;
            std::set<std::string> tokens;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(mDir))
            {
                if (!entry.is_regular_file())
                    continue;
                std::ifstream stream(entry.path());
                std::stringstream buffer;
                buffer << stream.rdbuf();
                const std::string source = buffer.str();
                for (auto it = std::sregex_iterator(source.begin(), source.end(), iterator);
                    it != std::sregex_iterator(); ++it)
                    iterators.insert((*it)[1].str());
                for (auto it = std::sregex_iterator(source.begin(), source.end(), token); it != std::sregex_iterator();
                    ++it)
                    tokens.insert((*it)[1].str());
            }
            for (const std::string& name : tokens)
            {
                if (name == "link" || name == "foreach" || name == "endforeach" || iterators.contains(name))
                    continue;
                if (!defines.contains(name))
                    defines[name] = "0";
            }
            return defines;
        }

        void dump(const std::string& label, const osg::Program& program) const
        {
            if (mDumpDir.empty())
                return;
            for (unsigned int i = 0; i < program.getNumShaders(); ++i)
            {
                const osg::Shader* shader = program.getShader(i);
                const std::string stage = shader->getType() == osg::Shader::VERTEX ? "vert" : "frag";
                std::string name = label;
                name += '_';
                name += std::to_string(i);
                name += '.';
                name += stage;
                std::ofstream stream(mDumpDir / name);
                stream << shader->getShaderSource();
            }
        }

        /// Return shader text with branches disabled by literal preprocessor conditions removed.
        static std::string activeVersion120Source(const std::string& source)
        {
            struct Branch
            {
                bool mParentActive;
                bool mActive;
                int mLiteralCondition;
            };

            std::vector<Branch> branches;
            std::istringstream input(source);
            std::ostringstream output;
            std::string line;
            while (std::getline(input, line))
            {
                const std::size_t first = line.find_first_not_of(" \t");
                const std::string_view directive
                    = first == std::string::npos ? std::string_view() : std::string_view(line).substr(first);
                const bool parentActive = branches.empty() || branches.back().mActive;
                if (directive.starts_with("#if "))
                {
                    const std::string_view expression = directive.substr(4);
                    const int literal = expression == "0" ? 0 : expression == "1" ? 1 : -1;
                    branches.push_back({ parentActive, parentActive && literal != 0, literal });
                    continue;
                }
                if (directive.starts_with("#ifdef ") || directive.starts_with("#ifndef "))
                {
                    branches.push_back({ parentActive, parentActive, -1 });
                    continue;
                }
                if (directive.starts_with("#elif ") && !branches.empty())
                {
                    Branch& branch = branches.back();
                    branch.mActive = branch.mParentActive && branch.mLiteralCondition != 1;
                    branch.mLiteralCondition = -1;
                    continue;
                }
                if (directive == "#else" && !branches.empty())
                {
                    Branch& branch = branches.back();
                    branch.mActive = branch.mParentActive && branch.mLiteralCondition != 1;
                    continue;
                }
                if (directive == "#endif" && !branches.empty())
                {
                    branches.pop_back();
                    continue;
                }
                if (branches.empty() || branches.back().mActive)
                    output << line << '\n';
            }
            return output.str();
        }

        /// Require the active classic shader body to remain valid GLSL 1.20 source.
        static void expectVersion120(const std::string& source, const std::string& label)
        {
            EXPECT_THAT(source, StartsWith(sVersion120)) << label;
            const std::regex forbidden(
                "\\b(uint|uvec[234]|flat|sampler2DArray)\\b|layout\\s*\\(\\s*std430|sampler2D\\s*\\(");
            EXPECT_FALSE(std::regex_search(activeVersion120Source(source), forbidden)) << label;
        }

        osg::ref_ptr<osg::Program> buildProgram(
            const std::string& name, const ShaderManager::DefineMap& defines, ProgramCapability capabilities)
        {
            const osg::ref_ptr<osg::Shader> vertex = mManager.getShader(name + ".vert", defines);
            const osg::ref_ptr<osg::Shader> fragment = mManager.getShader(name + ".frag", defines);
            if (vertex == nullptr || fragment == nullptr)
                return nullptr;
            return mManager.getProgram(vertex, fragment, nullptr, {}, capabilities);
        }
    };

    TEST_F(ShaderTemplateTest, classic_alpha_compiles)
    {
        const osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 8;
        traits->height = 8;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        const osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (context == nullptr || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "Offscreen GL context unavailable";
        osg::State& state = *context->getState();
        if (!osg::isGLExtensionSupported(state.getContextID(), "GL_EXT_gpu_shader4"))
            GTEST_SKIP() << "GL_EXT_gpu_shader4 unavailable";
        for (const char* gpuShader4 : { "0", "1" })
            for (const char* coverage : { "0", "1" })
                for (const char* name : { "objects", "terrain", "groundcover", "shadowcasting" })
                {
                    SCOPED_TRACE(std::string(name) + " gpuShader4=" + gpuShader4 + " coverage=" + coverage);
                    const ShaderManager::DefineMap defines = { { "diffuseMap", "1" }, { "diffuseMapUV", "0" },
                        { "useGPUShader4", gpuShader4 }, { "adjustCoverage", coverage }, { "alphaFunc", "516" } };
                    const osg::ref_ptr<osg::Program> program = buildProgram(name, defines, sMaterialCapabilities);
                    ASSERT_TRUE(program);
                    dump(std::string(name) + "_classic_" + gpuShader4 + coverage, *program);
                    program->compileGLObjects(state);
                    const osg::Program::PerContextProgram* pcp = program->getPCP(state);
                    std::string log;
                    pcp->getInfoLog(log);
                    EXPECT_TRUE(pcp->isLinked()) << log;
                }
        mManager.releaseGLObjects(&state);
        context->releaseContext();
        context->close();
    }

    TEST_F(ShaderTemplateTest, bindless_variants_compile)
    {
        const osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 8;
        traits->height = 8;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        const osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (context == nullptr || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "Offscreen GL context unavailable";
        osg::State& state = *context->getState();
        if (!osg::isGLExtensionSupported(state.getContextID(), "GL_ARB_bindless_texture")
            || (!osg::isGLExtensionSupported(state.getContextID(), "GL_NV_gpu_shader5")
                && !osg::isGLExtensionSupported(state.getContextID(), "GL_EXT_nonuniform_qualifier")))
            GTEST_SKIP() << "Nonuniform bindless requirements unavailable";
        for (const char* classicFalloff : { "0", "1" })
        {
            for (const char* name : { "objects", "terrain", "groundcover", "shadowcasting", "depthclipped",
                     "gpuscene_prepass", "bs/default", "bs/nolighting" })
            {
                const std::string label = std::string(name) + "_" + classicFalloff;
                SCOPED_TRACE(label);
                const ShaderManager::DefineMap defines
                    = { { "diffuseMap", "1" }, { "diffuseMapUV", "0" }, { "lightingMethodClustered", "1" },
                          { "classicFalloff", classicFalloff }, { "useGPUShader4", "1" }, { "adjustCoverage", "1" } };
                const osg::ref_ptr<osg::Program> classic = buildProgram(name, defines, sMaterialCapabilities);
                ASSERT_TRUE(classic);
                const osg::ref_ptr<osg::Program> program
                    = mManager.getInstancedProgram(*classic, MaterialMode::Bindless);
                ASSERT_TRUE(program);
                program->compileGLObjects(state);
                const osg::Program::PerContextProgram* pcp = program->getPCP(state);
                std::string log;
                pcp->getInfoLog(log);
                EXPECT_TRUE(pcp->isLinked()) << log;
            }
        }
        mManager.releaseGLObjects(&state);
        context->releaseContext();
        context->close();
    }

    TEST_F(ShaderTemplateTest, switchesLightingPrograms)
    {
        const osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 8;
        traits->height = 8;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "4.3";
        traits->glContextProfileMask = 0x2;
        const osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "Offscreen GL context unavailable";
        osg::State& state = *context->getState();
        std::vector<MaterialMode> modes{ MaterialMode::Legacy, MaterialMode::Array };
        if (osg::isGLExtensionSupported(state.getContextID(), "GL_ARB_bindless_texture")
            && (osg::isGLExtensionSupported(state.getContextID(), "GL_NV_gpu_shader5")
                || osg::isGLExtensionSupported(state.getContextID(), "GL_EXT_nonuniform_qualifier")))
            modes.push_back(MaterialMode::Bindless);
        auto globals = mManager.getGlobalDefines();
        globals["lightingMethodClustered"] = "1";
        globals["legacyLightList"] = "0";
        globals["useGPUShader4"] = "1";
        mManager.setGlobalDefines(globals);
        std::vector<osg::ref_ptr<osg::Program>> programs;
        for (const char* name : { "objects", "terrain", "groundcover", "bs/default" })
        {
            const auto classic
                = buildProgram(name, { { "diffuseMap", "1" }, { "diffuseMapUV", "0" } }, sMaterialCapabilities);
            ASSERT_TRUE(classic);
            programs.push_back(classic);
            for (auto mode : modes)
            {
                programs.push_back(mManager.getInstancedProgram(*classic, mode));
                ASSERT_TRUE(programs.back());
            }
        }
        for (const char* classicFalloff : { "0", "1", "0" })
        {
            SCOPED_TRACE(classicFalloff);
            globals["classicFalloff"] = classicFalloff;
            mManager.setGlobalDefines(globals);
            for (const auto& program : programs)
            {
                program->compileGLObjects(state);
                const auto* pcp = program->getPCP(state);
                std::string log;
                pcp->getInfoLog(log);
                EXPECT_TRUE(pcp->isLinked()) << log;
            }
        }
        mManager.releaseGLObjects(&state);
        context->releaseContext();
        context->close();
    }

    TEST_F(ShaderTemplateTest, classicClustersSelectReceivers)
    {
        using SceneUtil::LightReceiver;
        const osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 8;
        traits->height = 8;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = "4.3";
        traits->glContextProfileMask = 0x2;
        const osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            GTEST_SKIP() << "Offscreen GL context unavailable";
        osg::State& state = *context->getState();
        auto& ext = *state.get<osg::GLExtensions>();
        const auto shader = mManager.getShader("core/lighting/classic.comp", {}, osg::Shader::COMPUTE);
        ASSERT_TRUE(shader);
        const auto program = mManager.getProgram(nullptr, shader);
        ASSERT_TRUE(program);
        state.applyAttribute(program);
        auto* pcp = program->getPCP(state);
        std::string log;
        pcp->getInfoLog(log);
        ASSERT_TRUE(pcp->isLinked()) << log;
        GLuint buffers[5]{};
        ext.glGenBuffers(5, buffers);
        const GLuint bindings[] = { 1, 3, 4, 5, 6 };
        const auto upload = [&](unsigned int slot, const void* data, std::size_t size) {
            ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[slot]);
            ext.glBufferData(GL_SHADER_STORAGE_BUFFER, size, data, GL_DYNAMIC_DRAW);
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindings[slot], buffers[slot]);
        };
        constexpr std::uint32_t sentinel = 0xfeedface;
        std::vector<LightReceiver> objects(130);
        for (std::size_t i = 1; i < objects.size(); ++i)
            objects[i].mBound = osg::Vec4f(static_cast<float>(i % 3) * 40, 0, -200, 35);
        objects[3].mBound = objects[1].mBound;
        objects[4].mBound = osg::Vec4f(9000, 0, -200, 1);
        objects[4].mMeta[2] = 1;
        objects[5].mMeta[1] = 2;
        const std::vector<std::uint32_t> ignored{ 101, 103 };
        for (const unsigned int limit : { 2u, 16u, 64u })
        {
            SCOPED_TRACE(limit);
            for (const unsigned int lightCount : { 5u, 97u, 0u, 7u })
            {
                SCOPED_TRACE(lightCount);
                std::vector<LightReceiver> lights(std::max(lightCount, 1u));
                for (std::size_t i = 0; i < lightCount; ++i)
                    lights[i]
                        = { osg::Vec4f(static_cast<float>(i) * 1.137f - 12, 0, -205, 17.f + static_cast<float>(i % 7)),
                              { static_cast<std::uint32_t>(100 + i), 0, 0, 0 } };
                std::vector<std::uint32_t> grid(objects.size() * 2 + 2, sentinel);
                std::vector<std::uint32_t> indices(objects.size() * limit + 1, sentinel);
                upload(0, objects.data(), objects.size() * sizeof(LightReceiver));
                upload(1, grid.data(), grid.size() * sizeof(std::uint32_t));
                upload(2, indices.data(), indices.size() * sizeof(std::uint32_t));
                upload(3, lights.data(), lights.size() * sizeof(LightReceiver));
                upload(4, ignored.data(), ignored.size() * sizeof(std::uint32_t));
                ext.glUniform1ui(pcp->getUniformLocation("lightCount"), lightCount);
                ext.glUniform1ui(pcp->getUniformLocation("receiverCount"), static_cast<GLuint>(objects.size()));
                ext.glUniform1ui(pcp->getUniformLocation("lightLimit"), limit);
                ext.glDispatchCompute(static_cast<GLuint>((objects.size() + 63) / 64), 1, 1);
                ext.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
                ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
                ext.glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, grid.size() * sizeof(std::uint32_t), grid.data());
                ext.glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[2]);
                ext.glGetBufferSubData(
                    GL_SHADER_STORAGE_BUFFER, 0, indices.size() * sizeof(std::uint32_t), indices.data());
                EXPECT_EQ(glGetError(), GL_NO_ERROR);
                EXPECT_EQ(grid[0], 0u);
                EXPECT_EQ(grid[1], 0u);
                EXPECT_EQ(grid[objects.size() * 2], sentinel);
                EXPECT_EQ(grid.back(), sentinel);
                EXPECT_EQ(indices.back(), sentinel);
                for (std::size_t object = 1; object < objects.size(); ++object)
                {
                    SCOPED_TRACE(object);
                    const auto& receiver = objects[object];
                    const osg::Vec3f center(receiver.mBound.x(), receiver.mBound.y(), receiver.mBound.z());
                    std::vector<std::uint32_t> expected;
                    const auto distance = [&](std::uint32_t light) {
                        const auto& bound = lights[light].mBound;
                        return (center - osg::Vec3f(bound.x(), bound.y(), bound.z())).length2();
                    };
                    for (std::uint32_t light = 0; light < lightCount; ++light)
                    {
                        const float reach = receiver.mBound.w() + lights[light].mBound.w();
                        bool skip = false;
                        for (std::uint32_t i = 0; i < receiver.mMeta[1]; ++i)
                            skip |= ignored[receiver.mMeta[0] + i] == lights[light].mMeta[0];
                        if (!skip && distance(light) <= reach * reach)
                            expected.push_back(light);
                    }
                    if (expected.size() > limit)
                    {
                        std::sort(expected.begin(), expected.end(), [&](std::uint32_t a, std::uint32_t b) {
                            return lights[a].mBound.w() * distance(b) > lights[b].mBound.w() * distance(a);
                        });
                        expected.resize(limit);
                    }
                    EXPECT_EQ(grid[object * 2 + 1], expected.size());
                    if (expected.empty())
                        EXPECT_EQ(grid[object * 2], receiver.mMeta[2]);
                    else
                    {
                        EXPECT_EQ(grid[object * 2], object * limit);
                        for (std::size_t i = 0; i < expected.size(); ++i)
                            EXPECT_EQ(indices[object * limit + i], expected[i]);
                    }
                }
            }
        }
        const ShaderManager::DefineMap defines{ { "lightingMethodClustered", "1" }, { "classicFalloff", "1" },
            { "gpuMaterial", "0" } };
        const auto instance = mManager.getShader("lib/core/instance.glsl", defines, osg::Shader::VERTEX);
        const auto lightBindings = mManager.getShader("lib/light/bindings.glsl", defines, osg::Shader::FRAGMENT);
        ASSERT_TRUE(instance);
        ASSERT_TRUE(lightBindings);
        const osg::ref_ptr<osg::Program> draw = new osg::Program;
        draw->addShader(instance);
        draw->addShader(new osg::Shader(osg::Shader::VERTEX, R"glsl(#version 430 compatibility
            mat4 instanceWorld();
            void main() {
                vec2 p[4] = vec2[4](vec2(-1,-1), vec2(1,-1), vec2(-1,1), vec2(1,1));
                gl_Position = instanceWorld() * vec4(p[gl_VertexID], 0, 1);
            }
        )glsl"));
        draw->addShader(new osg::Shader(
            osg::Shader::FRAGMENT, "#version 430 compatibility\n" + lightBindings->getShaderSource() + R"glsl(
            flat in uint gpuLightCluster;
            out vec4 color;
            void main() {
                LightGrid selected = clusterLights(int(gpuLightCluster));
                color = vec4(selected.count > 0u ? 1 : 0, 0, selected.count == 0u ? 1 : 0, 1);
            }
        )glsl"));
        draw->addBindAttribLocation("instanceSlot", 7);
        state.applyAttribute(draw);
        auto* drawProgram = draw->getPCP(state);
        drawProgram->getInfoLog(log);
        ASSERT_TRUE(drawProgram->isLinked()) << log;
        struct Instance
        {
            std::array<osg::Vec4f, 5> mVectors{};
            std::array<std::uint32_t, 4> mMeta{};
            std::array<std::uint32_t, 4> mMaterial{};
        };
        static_assert(sizeof(Instance) == 112);
        std::array<Instance, 2> instances;
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            instances[i].mVectors[0] = osg::Vec4f(.5f, 0, 0, i == 0 ? -.5f : .5f);
            instances[i].mVectors[1] = osg::Vec4f(0, 1, 0, 0);
            instances[i].mVectors[2] = osg::Vec4f(0, 0, 1, 0);
            instances[i].mMaterial[1] = i == 0 ? 3 : 5;
        }
        const std::array<std::uint32_t, 4> slots{ 99, 99, 0, 1 };
        const std::array<std::uint32_t, 6> elements{ 0, 1, 2, 2, 1, 3 };
        std::array<std::uint32_t, 15> commands{ 0, 0, 0, 0, 0, 6, 1, 0, 0, 2, 6, 1, 0, 0, 3 };
        std::array<GLuint, 5> drawBuffers{};
        GLuint vao = 0;
        ext.glGenVertexArrays(1, &vao);
        ext.glBindVertexArray(vao);
        ext.glGenBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
        const auto fill = [&](std::size_t index, GLenum target, const auto& data) {
            ext.glBindBuffer(target, drawBuffers[index]);
            ext.glBufferData(target, sizeof(data), data.data(), GL_DYNAMIC_DRAW);
        };
        fill(0, GL_SHADER_STORAGE_BUFFER, instances);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, drawBuffers[0]);
        fill(2, GL_ARRAY_BUFFER_ARB, slots);
        ext.glEnableVertexAttribArray(7);
        ext.glVertexAttribIPointer(7, 1, GL_UNSIGNED_INT, 0, nullptr);
        ext.glVertexAttribDivisor(7, 1);
        fill(3, GL_ELEMENT_ARRAY_BUFFER_ARB, elements);
        glViewport(0, 0, 8, 8);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        for (unsigned int view = 0; view < 3; ++view)
        {
            SCOPED_TRACE(view);
            // Sparse slots exercise offsets and inheritance.
            std::array<std::uint32_t, 6> receivers{ 0, 0, 0, view == 0 ? 1u : 2u, 0, view == 0 ? 2u : 4u };
            fill(1, GL_SHADER_STORAGE_BUFFER, receivers);
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, drawBuffers[1]);
            commands[11] = view == 2 ? 0 : 1;
            fill(4, GL_DRAW_INDIRECT_BUFFER, commands);
            glClear(GL_COLOR_BUFFER_BIT);
            ext.glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                reinterpret_cast<const void*>(5 * sizeof(std::uint32_t)), 2, 5 * sizeof(std::uint32_t));
            std::array<unsigned char, 4> left{}, right{};
            glReadPixels(1, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left.data());
            glReadPixels(6, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right.data());
            EXPECT_EQ(left[0], view == 0 ? 255 : 0);
            EXPECT_EQ(left[2], view == 0 ? 0 : 255);
            EXPECT_EQ(right[0], view == 1 ? 255 : 0);
            EXPECT_EQ(right[2], view == 0 ? 255 : 0);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
        ext.glBindVertexArray(0);
        ext.glDeleteVertexArrays(1, &vao);
        ext.glDeleteBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
        draw->releaseGLObjects(&state);
        ext.glDeleteBuffers(5, buffers);
        mManager.releaseGLObjects(&state);
        context->releaseContext();
        context->close();
    }

    TEST_F(ShaderTemplateTest, twins_link_bodies_and_keep_120_objects)
    {
        const char* maps[] = { "diffuseMap", "darkMap", "detailMap", "decalMap", "emissiveMap", "normalMap", "envMap",
            "specularMap", "bumpMap", "glossMap" };
        const std::vector<std::tuple<std::string, ShaderManager::DefineMap, ProgramCapability>> templates = {
            { "objects", {}, sObjectCapabilities },
            { "objects",
                { { "diffuseMap", "1" }, { "darkMap", "1" }, { "detailMap", "1" }, { "decalMap", "1" },
                    { "emissiveMap", "1" }, { "normalMap", "1" }, { "envMap", "1" }, { "specularMap", "1" },
                    { "bumpMap", "1" }, { "glossMap", "1" } },
                sObjectCapabilities },
            { "objects", { { "diffuseMap", "1" }, { "normalMap", "1" }, { "parallax", "1" } }, sObjectCapabilities },
            { "objects", { { "diffuseMap", "1" }, { "diffuseParallax", "1" } }, sObjectCapabilities },
            { "terrain", { { "normalMap", "1" }, { "blendMap", "1" } }, sMaterialCapabilities },
            { "groundcover", { { "diffuseMap", "1" }, { "normalMap", "1" } }, sObjectCapabilities },
            { "bs/default", { { "diffuseMap", "1" }, { "normalMap", "1" }, { "emissiveMap", "1" } },
                sObjectCapabilities },
            { "bs/nolighting", { { "diffuseMap", "1" } }, sObjectCapabilities },
            { "water", {}, ProgramCapability::Instanced },
            { "sky", {}, ProgramCapability::Instanced },
            { "shadowcasting", {}, sMaterialCapabilities },
            { "depthclipped", {}, sMaterialCapabilities },
            { "gpuscene_prepass", {}, sMaterialCapabilities },
        };
        int variant = 0;
        for (const auto& [name, extra, capabilities] : templates)
        {
            ShaderManager::DefineMap defines = extra;
            for (const char* map : maps)
                if (!defines.contains(map))
                    defines[map] = "0";
            std::string label = name + "_" + std::to_string(variant++);
            std::replace(label.begin(), label.end(), '/', '_');
            const osg::ref_ptr<osg::Program> classic = buildProgram(name, defines, capabilities);
            ASSERT_TRUE(classic) << label;
            dump(label + "_classic", *classic);
            expectVersion120(shaderSource(*classic, 0), label + " classic vertex");
            expectVersion120(shaderSource(*classic, 1), label + " classic fragment");
            EXPECT_THAT(programSource(*classic), Not(HasSubstr("#version 430"))) << label;

            for (const auto& [mode, modeName] : { std::make_pair(MaterialMode::Legacy, "legacy"),
                     std::make_pair(MaterialMode::Array, "array"), std::make_pair(MaterialMode::Bindless, "bindless") })
            {
                const osg::ref_ptr<osg::Program> twin = mManager.getInstancedProgram(*classic, mode);
                if (mode == MaterialMode::Array && twin == nullptr)
                    continue;
                ASSERT_TRUE(twin) << label << ' ' << modeName;
                dump(label + "_" + modeName, *twin);
                expectVersion120(shaderSource(*twin, 0), label + ' ' + modeName + " vertex");
                expectVersion120(shaderSource(*twin, 1), label + ' ' + modeName + " fragment");
                EXPECT_THAT(programSource(*twin), HasSubstr("#version 430 compatibility")) << label << ' ' << modeName;
                EXPECT_THAT(programSource(*twin), HasSubstr("layout(std430, binding = 8)")) << label << ' ' << modeName;
                // Only fragment material bodies need the extension.
                const bool bindlessBody = mode == MaterialMode::Bindless && name != "gpuscene_prepass";
                EXPECT_EQ(bindlessBody, programSource(*twin).find(sBindlessRequire) != std::string::npos)
                    << label << ' ' << modeName;
                EXPECT_EQ(bindlessBody,
                    programSource(*twin).find("#extension GL_NV_gpu_shader5 : require") != std::string::npos)
                    << label << ' ' << modeName;
                if (bindlessBody)
                {
                    EXPECT_THAT(programSource(*twin), HasSubstr("#extension GL_EXT_nonuniform_qualifier : require"));
                    EXPECT_THAT(programSource(*twin), HasSubstr("nonuniformEXT(sampler2D(handle))"));
                    EXPECT_THAT(programSource(*twin), HasSubstr("texture(GPU_SAMPLER(mapHandle(map)), uv)"));
                    EXPECT_THAT(programSource(*twin), HasSubstr("textureSize(GPU_SAMPLER(mapHandle(map)), 0)"));
                }
            }
        }
    }
}
