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
          "#define LIGHT_LIST @gpuLightList\n"
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

        InstancedProgramTest() { mManager.setShaderPath(TestingOpenMW::outputDir()); }

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
            const ShaderManager::DefineMap& extra = {}, ProgramCapability capabilities = sMaterialCapabilities)
        {
            const std::string vertexName = writeTemplate(vertexSuffix, sVertexTemplate);
            const std::string fragmentName = writeTemplate(fragmentSuffix, sFragmentTemplate);
            ShaderManager::DefineMap defines = getDefaultDefines();
            for (const auto& [key, value] : extra)
                defines[key] = value;
            const osg::ref_ptr<osg::Shader> vertex = mManager.getShader(vertexName, defines);
            const osg::ref_ptr<osg::Shader> fragment = mManager.getShader(fragmentName, defines);
            if (vertex == nullptr || fragment == nullptr)
                return nullptr;
            return mManager.getProgram(vertex, fragment, nullptr, {}, capabilities);
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
            EXPECT_THAT(shaderSource(*twin, 0), HasSubstr("#define LIGHT_LIST 1"));
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
        // Same template, different map sets, parallax on both.
        const ShaderManager::DefineMap darkDefines = { { "diffuseMap", "1" }, { "darkMap", "1" }, { "parallax", "1" } };
        const ShaderManager::DefineMap detailDefines
            = { { "diffuseMap", "1" }, { "detailMap", "1" }, { "parallax", "1" } };
        const osg::ref_ptr<osg::Program> dark
            = buildProgram("_objects.vert", "_objects.frag", darkDefines, sObjectCapabilities);
        const osg::ref_ptr<osg::Program> detail
            = buildProgram("_objects.vert", "_objects.frag", detailDefines, sObjectCapabilities);
        ASSERT_TRUE(dark);
        ASSERT_TRUE(detail);
        EXPECT_NE(dark.get(), detail.get());

        const osg::ref_ptr<osg::Program> uber = mManager.getInstancedProgram(*dark, MaterialMode::Bindless);
        ASSERT_TRUE(uber);
        EXPECT_EQ(uber.get(), mManager.getInstancedProgram(*detail, MaterialMode::Bindless).get());
        EXPECT_THAT(shaderSource(*uber, 0), HasSubstr("#define UBER 1"));

        const osg::ref_ptr<osg::Program> legacy = mManager.getInstancedProgram(*dark, MaterialMode::Legacy);
        ASSERT_TRUE(legacy);
        EXPECT_NE(legacy.get(), mManager.getInstancedProgram(*detail, MaterialMode::Legacy).get());
        EXPECT_THAT(shaderSource(*legacy, 0), HasSubstr("#define UBER 0"));
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

    // Compiles the shipped templates when OPENMW_TEST_SHADER_DIR points at files/shaders.
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

        // Every @token in the tree gets a compilable default.
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

        static void expectVersion120(const std::string& source, const std::string& label)
        {
            EXPECT_THAT(source, StartsWith(sVersion120)) << label;
            const std::regex forbidden(
                "\\b(uint|uvec[234]|flat|sampler2DArray)\\b|layout\\s*\\(\\s*std430|sampler2D\\s*\\(");
            EXPECT_FALSE(std::regex_search(source, forbidden)) << label;
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
            }
        }
    }
}
