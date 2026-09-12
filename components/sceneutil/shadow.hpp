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
    struct OpenMWSEShadowsCategory;
    struct ShadowsCategory;
}

namespace SceneUtil
{
    class MWShadowTechnique;

    /// Own shadow-map policy, resources, shader specialization, and indoor/outdoor mode transitions.
    class ShadowManager
    {
    public:
        static Shader::ShaderManager::DefineMap getShadowsDisabledDefines();

        static const ShadowManager& instance();

        /// Return the common texture dimension limit of realized graphics contexts, or zero before discovery.
        static int getMaximumShadowMapResolution();

        /// Return the active detailed plus optional distant cascade count.
        int getEffectiveShadowMapCount() const;

        /// Attach shadow rendering to the scene and reserve sampler capacity for the initial visible distance.
        explicit ShadowManager(osg::ref_ptr<osg::Group> sceneRoot, osg::ref_ptr<osg::Group> rootNode,
            unsigned int outdoorShadowCastingMask, unsigned int indoorShadowCastingMask, unsigned int worldMask,
            const Settings::ShadowsCategory& settings, const Settings::OpenMWSEShadowsCategory& presentationSettings,
            float visibleDistance, Shader::ShaderManager& shaderManager);
        ~ShadowManager();

        /// Apply shadow policy and texture dimensions while render threads are quiescent.
        void setupShadowSettings(const Settings::ShadowsCategory& settings,
            const Settings::OpenMWSEShadowsCategory& presentationSettings, float visibleDistance,
            Shader::ShaderManager& shaderManager);
        void setOutdoorShadowCastingMask(unsigned int mask);
        void setIndoorShadowCastingMask(unsigned int mask);

        void setupShaders(Shader::ShaderManager& shaderManager) const;

        void disableShadowsForStateSet(osg::StateSet& stateset) const;

        /// Return receiver-shader specializations for the shared and fork-owned shadow settings.
        Shader::ShaderManager::DefineMap getShadowDefines(const Settings::ShadowsCategory& settings,
            const Settings::OpenMWSEShadowsCategory& presentationSettings) const;

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
        int mReservedShadowMapCapacity = 0;
    };
}

#endif // COMPONENTS_SCENEUTIL_SHADOW_H
