#ifndef OPENMW_COMPONENTS_SHADERMANAGER_H
#define OPENMW_COMPONENTS_SHADERMANAGER_H

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <osg/Program>
#include <osg/Shader>
#include <osg/ref_ptr>

#include "materialmode.hpp"

namespace osgViewer
{
    class Viewer;
}

namespace Shader
{
    struct HotReloadManager;

    /// Reserve the complete shared water-optics sampler range before any material records its base unit.
    inline constexpr int waterOpticsTextureUnitCount = 3;

    // What a program's templates implement.
    enum class ProgramCapability : unsigned int
    {
        None = 0,
        Instanced = 1 << 0,
        ArrayMaterial = 1 << 1,
        UberMaterial = 1 << 2,
    };

    constexpr ProgramCapability operator|(ProgramCapability lhs, ProgramCapability rhs)
    {
        return static_cast<ProgramCapability>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr bool hasCapability(ProgramCapability set, ProgramCapability flag)
    {
        return (static_cast<unsigned int>(set) & static_cast<unsigned int>(flag)) != 0;
    }

    /// @brief Reads shader template files and turns them into a concrete shader, based on a list of define's.
    /// @par Shader templates can get the value of a define with the syntax @define.
    class ShaderManager
    {
    public:
        friend HotReloadManager;
        ShaderManager();
        ~ShaderManager();

        void setShaderPath(const std::filesystem::path& path);

        typedef std::map<std::string, std::string> DefineMap;

        /// Create or retrieve a shader instance.
        /// @param templateName The path of the shader template.
        /// @param defines Define values that can be retrieved by the shader template.
        /// @param shaderType The type of shader (usually vertex or fragment shader).
        /// @note May return nullptr on failure.
        /// @note Thread safe, use getShaderInternal within the ShaderManager if you've already locked the mutex.
        osg::ref_ptr<osg::Shader> getShader(std::string templateName, const DefineMap& defines = {},
            std::optional<osg::Shader::Type> type = std::nullopt);

        typedef std::map<std::string, int> SamplerBindingMap;

        /// @param samplers Sampler uniforms to bind once per linked program instead of on every stateset.
        /// @param capabilities What the templates implement for getInstancedProgram.
        osg::ref_ptr<osg::Program> getProgram(const std::string& templateName, const DefineMap& defines = {},
            const osg::Program* programTemplate = nullptr, const SamplerBindingMap& samplers = {},
            ProgramCapability capabilities = ProgramCapability::None);

        osg::ref_ptr<osg::Program> getProgram(osg::ref_ptr<osg::Shader> vertexShader,
            osg::ref_ptr<osg::Shader> fragmentShader, const osg::Program* programTemplate = nullptr,
            const SamplerBindingMap& samplers = {}, ProgramCapability capabilities = ProgramCapability::None);

        static constexpr unsigned int sInstanceSlotAttribute = 6;
        static constexpr unsigned int sVertexMaterialAttribute = 7;

        /// Compiles a GPU-instanced material variant.
        /// @note Returns nullptr when the program lacks the capabilities the mode needs.
        osg::ref_ptr<osg::Program> getInstancedProgram(
            const osg::Program& program, MaterialMode mode, bool full = false);

        /// Defines that select the instance and material bodies.
        static void applyInstancedDefines(DefineMap& defines, MaterialMode mode);

        std::optional<int> getSamplerUnit(const osg::Program& program, std::string_view name) const;

        const osg::Program* getProgramTemplate() const { return mProgramTemplate; }
        void setProgramTemplate(const osg::Program* program) { mProgramTemplate = program; }

        /// Clone an osg::Program including bindUniformBlocks that osg::Program::clone does not copy for some reason.
        static osg::ref_ptr<osg::Program> cloneProgram(const osg::Program*);

        /// Get (a copy of) the DefineMap used to construct all shaders
        DefineMap getGlobalDefines();

        /// Set the DefineMap used to construct all shaders
        /// @param defines The DefineMap to use
        /// @note This will change the source code for any shaders already created, potentially causing problems if
        /// they're being used to render a frame. It is recommended that any associated Viewers have their threading
        /// stopped while this function is running if any shaders are in use.
        void setGlobalDefines(DefineMap& globalDefines);

        void releaseGLObjects(osg::State* state);

        bool createSourceFromTemplate(std::string& source, std::vector<std::string>& linkedShaderTemplateNames,
            const std::string& templateName, ShaderManager::DefineMap defines);

        void setMaxTextureUnits(int maxTextureUnits) { mMaxTextureUnits = maxTextureUnits; }
        int getMaxTextureUnits() const { return mMaxTextureUnits; }
        int getAvailableTextureUnits() const { return mMaxTextureUnits - mReservedTextureUnits; }

        enum class Slot
        {
            OpaqueDepthTexture,
            OpaqueColorTexture,
            AtmosphereNoise,
            SkyTexture,
            TexturedCloudOpacity,
            ShadowMaps,
            WaterOptics,
            SLOT_COUNT
        };

        /// Reserve or grow one relocatable contiguous global sampler range without moving other owners.
        int reserveGlobalTextureUnits(Slot slot, int count = 1);

        void update(osgViewer::Viewer& viewer);
        void setHotReloadEnabled(bool value);
        void triggerShaderReload();

    private:
        osg::ref_ptr<osg::Shader> getShaderInternal(std::string templateName, const DefineMap& defines = {},
            std::optional<osg::Shader::Type> type = std::nullopt);
        osg::ref_ptr<osg::Program> getProgramInternal(const osg::ref_ptr<osg::Shader>& vertexShader,
            const osg::ref_ptr<osg::Shader>& fragmentShader, const osg::Program* programTemplate,
            const SamplerBindingMap& samplers, ProgramCapability capabilities);
        void getLinkedShaders(osg::ref_ptr<osg::Shader> shader, const std::vector<std::string>& linkedShaderNames,
            const DefineMap& defines);
        void addLinkedShaders(osg::ref_ptr<osg::Shader> shader, osg::ref_ptr<osg::Program> program);

        std::filesystem::path mPath;

        DefineMap mGlobalDefines;

        // <name, code>
        typedef std::map<std::string, std::string> TemplateMap;
        TemplateMap mShaderTemplates;

        typedef std::pair<std::string, DefineMap> MapKey;
        typedef std::map<MapKey, osg::ref_ptr<osg::Shader>> ShaderMap;
        ShaderMap mShaders;

        typedef std::multimap<std::pair<osg::ref_ptr<osg::Shader>, osg::ref_ptr<osg::Shader>>,
            osg::ref_ptr<osg::Program>>
            ProgramMap;
        ProgramMap mPrograms;

        std::map<const osg::Shader*, MapKey> mShaderKeys;
        std::map<std::tuple<osg::ref_ptr<const osg::Program>, MaterialMode, bool>, osg::ref_ptr<osg::Program>>
            mInstancedPrograms;

        typedef std::vector<osg::ref_ptr<osg::Shader>> ShaderList;
        typedef std::map<osg::ref_ptr<osg::Shader>, ShaderList> LinkedShadersMap;
        LinkedShadersMap mLinkedShaders;

        std::mutex mMutex;

        osg::ref_ptr<const osg::Program> mProgramTemplate;

        int mMaxTextureUnits = 0;
        int mReservedTextureUnits = 0;
        std::unique_ptr<HotReloadManager> mHotReloadManager;
        struct ReservedTextureUnits
        {
            int index = -1;
            int count = 0;
        };
        std::array<ReservedTextureUnits, static_cast<int>(Slot::SLOT_COUNT)> mReservedTextureUnitsBySlot = {};
    };

    bool parseForeachDirective(std::string& source, const std::string& templateName, size_t foundPos);
    bool parseLinkDirective(
        std::string& source, std::string& linkTarget, const std::string& templateName, size_t foundPos);

    bool parseDefines(std::string& source, const ShaderManager::DefineMap& defines,
        const ShaderManager::DefineMap& globalDefines, const std::string& templateName);

    bool parseDirectives(std::string& source, std::vector<std::string>& linkedShaderTemplateNames,
        const ShaderManager::DefineMap& defines, const ShaderManager::DefineMap& globalDefines,
        const std::string& templateName);

    /// Supply shared shader defaults, including one-sided lighting for ordinary scene surfaces.
    ShaderManager::DefineMap getDefaultDefines();
}

#endif
