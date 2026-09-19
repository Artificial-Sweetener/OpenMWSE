#ifndef OPENMW_COMPONENTS_SCENEUTIL_SURFACEEFFECT_H
#define OPENMW_COMPONENTS_SCENEUTIL_SURFACEEFFECT_H

#include <optional>
#include <string>

#include <osg/Object>

namespace SceneUtil
{
    /// Identifies the renderer implementation assigned to a classified material surface.
    enum class SurfaceEffectType
    {
        Water,
    };

    /// Stores the resolved surface-effect profile independently from its source material appearance.
    struct SurfaceEffectProfile
    {
        std::string mId;
        SurfaceEffectType mType = SurfaceEffectType::Water;
        /// Override the global mesh-water wave height when a material profile needs distinct behavior.
        std::optional<float> mGeometricWaveHeight;
    };

    /// Tags one scene-graph material scope with its resolved surface-effect profile.
    class SurfaceEffect : public osg::Object
    {
    public:
        /// Construct an empty tag for OpenSceneGraph serialization and cloning.
        SurfaceEffect() = default;

        /// Construct a tag for one immutable resolved profile.
        explicit SurfaceEffect(SurfaceEffectProfile profile);

        /// Preserve the resolved profile while cloning an asset template into the scene.
        SurfaceEffect(const SurfaceEffect& copy, const osg::CopyOp& copyOp);

        META_Object(SceneUtil, SurfaceEffect)

        /// Return the immutable profile selected during asset classification.
        const SurfaceEffectProfile& profile() const { return mProfile; }

    private:
        SurfaceEffectProfile mProfile;
    };
}

#endif
