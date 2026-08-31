#ifndef COMPONENTS_SCENEUTIL_SHADOW_H
#define COMPONENTS_SCENEUTIL_SHADOW_H

#include <components/shader/shadermanager.hpp>

namespace osg
{
    class StateSet;
    class Group;
}

namespace osgShadow
{
    class ShadowSettings;
    class ShadowedScene;
}

namespace Settings
{
    struct ShadowsCategory;
}

namespace SceneUtil
{
    class MWShadowTechnique;
    class ShadowManager
    {
    public:
        static Shader::ShaderManager::DefineMap getShadowsDisabledDefines();

        static const ShadowManager& instance();

        explicit ShadowManager(osg::ref_ptr<osg::Group> sceneRoot, osg::ref_ptr<osg::Group> rootNode,
            unsigned int outdoorShadowCastingMask, unsigned int indoorShadowCastingMask, unsigned int worldMask,
            const Settings::ShadowsCategory& settings, Shader::ShaderManager& shaderManager);
        ~ShadowManager();

        /// Apply runtime shadow policy and reserve the configured shadow-map texture units.
        void setupShadowSettings(const Settings::ShadowsCategory& settings, Shader::ShaderManager& shaderManager);
        void setOutdoorShadowCastingMask(unsigned int mask);
        void setIndoorShadowCastingMask(unsigned int mask);

        void setupShaders(Shader::ShaderManager& shaderManager) const;

        void disableShadowsForStateSet(osg::StateSet& stateset) const;

        Shader::ShaderManager::DefineMap getShadowDefines(const Settings::ShadowsCategory& settings) const;

        void enableIndoorMode(const Settings::ShadowsCategory& settings);

        void enableOutdoorMode();

        /// Temporarily bypass or restore configured shadow rendering for a diagnostic comparison frame.
        void setDiagnosticShadowRenderingEnabled(bool enabled) const;

        /// Temporarily replace the outdoor caster mask for a bounded diagnostic comparison.
        void setDiagnosticOutdoorShadowCastingMask(unsigned int mask) const;

        /// Restore the configured outdoor caster mask after a diagnostic comparison.
        void restoreDiagnosticOutdoorShadowCastingMask() const;

    protected:
        static ShadowManager* sInstance;

        bool mEnableShadows;

        osg::ref_ptr<osgShadow::ShadowedScene> mShadowedScene;
        osg::ref_ptr<osgShadow::ShadowSettings> mShadowSettings;
        osg::ref_ptr<MWShadowTechnique> mShadowTechnique;

        unsigned int mOutdoorShadowCastingMask;
        unsigned int mIndoorShadowCastingMask;
    };
}

#endif // COMPONENTS_SCENEUTIL_SHADOW_H
