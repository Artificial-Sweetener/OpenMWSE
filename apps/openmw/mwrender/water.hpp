#ifndef OPENMW_MWRENDER_WATER_H
#define OPENMW_MWRENDER_WATER_H

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <osg/Vec3d>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/sceneutil/waterutil.hpp>
#include <components/settings/settings.hpp>
#include <components/vfs/pathutil.hpp>

#include "surfaceeffectobjectregistry.hpp"
#include "watersurface.hpp"

namespace osg
{
    class Group;
    class PositionAttitudeTransform;
    class Drawable;
    class Geometry;
    class Node;
    class Callback;
    class Texture2D;
    class Texture3D;
    class Texture;
}

namespace SceneUtil
{
    struct SurfaceEffectProfile;
}

namespace osgUtil
{
    class IncrementalCompileOperation;
}

namespace SceneUtil
{
    class StateUpdater;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace Fallback
{
    class Map;
}

namespace MWRender
{

    class PlanarReflectionProvider;
    class PlanarReflectionReceiver;
    class GpuScene;
    class RippleSimulation;
    class RainSettingsUpdater;
    class Ripples;
    class WaterWaveOcclusion;
    class WaterScreenCoverage;
    class MeshWaterObject;
    class MeshWaterPlane;

    /// Water rendering
    class Water
    {
        osg::ref_ptr<RainSettingsUpdater> mRainSettingsUpdater;

        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<osg::PositionAttitudeTransform> mWaterNode;
        osg::ref_ptr<osg::Geometry> mFlatWaterGeom;
        osg::ref_ptr<osg::Geometry> mDisplacedWaterGeom;
        osg::ref_ptr<osg::Geometry> mWaterGeom;
        osg::ref_ptr<osg::Geometry> mSimpleWaterGeom;
        osg::ref_ptr<WaterScreenCoverage> mWorldWaterCoverage;
        osg::ref_ptr<osg::Group> mMeshWaterSeamRoot;
        Resource::ResourceSystem* mResourceSystem;
        GpuScene* mGpuScene;
        SceneUtil::StateUpdater* mSceneStateUpdater;
        osg::ref_ptr<osgUtil::IncrementalCompileOperation> mIncrementalCompileOperation;

        std::unique_ptr<RippleSimulation> mSimulation;
        std::unique_ptr<WaterWaveOcclusion> mWaveOcclusion;
        WaterSurfaceParameters mSurfaceParameters;
        /// Track whether classified authored materials currently use renderer-owned water replacements.
        bool mMeshWaterEnabled = true;

        osg::ref_ptr<PlanarReflectionProvider> mReflectionProvider;
        osg::ref_ptr<PlanarReflectionReceiver> mReflection;
        osg::ref_ptr<Ripples> mRipples;

        bool mEnabled;
        bool mToggled;
        float mTop;
        bool mInterior;
        bool mShowWorld;

        osg::Callback* mCullCallback;
        osg::ref_ptr<osg::Callback> mShaderWaterStateSetUpdater;
        osg::ref_ptr<osg::Texture2D> mWaterNormalMap;
        osg::ref_ptr<osg::Texture3D> mMgeWaterVolume;
        std::unordered_map<const osg::Node*, std::unique_ptr<MeshWaterObject>> mMeshWaterObjects;
        SurfaceEffectObjectRegistry mKnownSurfaceEffectObjects;
        std::map<SceneUtil::WaterSurfacePlaneKey, std::unique_ptr<MeshWaterPlane>> mMeshWaterPlanes;
        unsigned int mMeshWaterSeamRebuildDelay = 0;
        bool mUsesDisplacedGeometry = false;
        bool mUsesSurfaceCoverage = false;
        float mSewerRippleTimer = 0.f;
        float mRainIntensity = 0.f;

        osg::Vec3f getSceneNodeCoordinates(int gridX, int gridY);
        void updateVisible();

        void createSimpleWaterStateSet(osg::Node* node, float alpha);

        void createShaderWaterStateSet(osg::Node* node);

        /// Replace one classified authored material scope with renderer-owned mesh water state.
        void createMeshWaterStateSet(osg::Group& wrapper, PlanarReflectionReceiver& reflection,
            const SceneUtil::SurfaceEffectProfile& profile, const osg::Vec3f& nodePosition);

        /// Reallocate one mesh-water reflection at a stable quality-bucket transition.
        void resizeMeshWaterPlane(MeshWaterPlane& plane, unsigned int resolution);

        /// Allocate one provider-owned reflection receiver for an exact mesh-water plane.
        osg::ref_ptr<PlanarReflectionReceiver> acquireMeshWaterReflection(
            unsigned int resolution, const SceneUtil::WaterSurfacePlane& plane);

        /// Release an unused reflection target through its owning provider.
        void releaseMeshWaterReflection(osg::ref_ptr<PlanarReflectionReceiver> reflection);

        /// Restore authored geometry for one object without forgetting its live classification source.
        void removeSurfaceEffectBindings(const osg::Node* objectRoot);

        /// Refresh every loaded surface-effect object, optionally restoring its authored geometry first.
        void refreshSurfaceEffectBindings(bool rebuildGeometry);

        /// Recompile mesh-water shader state after a global water permutation changes.
        void rebuildMeshWaterStateSets();

        /// Rebuild bounded bridge geometry between compatible loaded planar-water boundaries.
        void rebuildMeshWaterSeams();

        /// Return the exact classified mesh-water plane touching a world-space emitter.
        std::optional<float> meshWaterSurfaceHeight(const osg::Vec3f& position, float maximumVerticalDistance) const;

        void updateWaterMaterial();

        /// Publish optional near-surface coverage through the scene's double-buffered state owner.
        void updateWaterSurfaceCoverageState();

        /// Select the water mesh and synchronize shared near-surface coverage resources.
        void updateWaterGeometry();

    public:
        /// Construct world and authored-mesh water under the shared scene state owner.
        Water(osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem,
            osgUtil::IncrementalCompileOperation* ico, GpuScene* gpuScene, SceneUtil::StateUpdater* sceneStateUpdater);
        ~Water();

        void setCullCallback(osg::Callback* callback);

        void listAssetsToPreload(std::vector<VFS::Path::Normalized>& textures);

        void setEnabled(bool enabled);

        void setGpuScene(GpuScene* scene) { mGpuScene = scene; }

        bool toggle();

        bool isVisible() const { return mEnabled && mToggled; }

        bool isUnderwater(const osg::Vec3f& pos) const;

        /// adds an emitter, position will be tracked automatically using its scene node
        void addEmitter(const MWWorld::Ptr& ptr, float scale = 1.f, float force = 1.f);
        void removeEmitter(const MWWorld::Ptr& ptr);
        void updateEmitterPtr(const MWWorld::Ptr& old, const MWWorld::Ptr& ptr);
        void emitRipple(const osg::Vec3f& pos);

        void removeCell(const MWWorld::CellStore* store); ///< remove all emitters in this cell

        void clearRipples();

        void changeCell(const MWWorld::CellStore* store);
        void setHeight(const float height);
        void setRainIntensity(const float rainIntensity);

        void update(float dt, bool paused);

        osg::Vec3d getPosition() const;

        float getHeight() const { return mTop; }

        /// Return whether a drawable is either permanent world-water geometry.
        bool isWorldWaterDrawable(const osg::Drawable* drawable) const;

        /// Return whether a drawable belongs to world water or a classified finite water surface.
        bool isWaterDrawable(const osg::Drawable* drawable) const;

        /// Register one classified surface-effect object using its current complete world transform.
        void registerSurfaceEffects(osg::Node* objectRoot);

        /// Register or reclassify a marked surface-effect object after its world transform changes.
        void synchronizeSurfaceEffects(osg::Node* objectRoot);

        /// Release per-object water bindings and unused planar-reflection cameras.
        void unregisterSurfaceEffects(const osg::Node* objectRoot);

        /// Return the independently owned policy for the default world-water surface.
        const WaterSurfaceParameters& getSurfaceParameters() const { return mSurfaceParameters; }

        /// Return the current rain contribution used by the active optical field.
        float getRainIntensity() const { return mRainIntensity; }

        /// Return the current shared ripple target, if shader water owns one.
        osg::Texture* getRippleTexture() const;

        /// Apply changed global settings to the default surface while preserving unrelated render targets.
        void processChangedSettings(const Settings::CategorySettingVector& settings);

        void showWorld(bool show);
    };

}

#endif
