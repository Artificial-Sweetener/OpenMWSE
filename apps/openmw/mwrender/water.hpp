#ifndef OPENMW_MWRENDER_WATER_H
#define OPENMW_MWRENDER_WATER_H

#include <memory>
#include <vector>

#include <osg/Vec3d>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/settings/settings.hpp>
#include <components/vfs/pathutil.hpp>

#include "watersurface.hpp"

namespace osg
{
    class Group;
    class PositionAttitudeTransform;
    class Drawable;
    class Geometry;
    class Node;
    class Callback;
}

namespace osgUtil
{
    class IncrementalCompileOperation;
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

    class Reflection;
    class GpuScene;
    class RippleSimulation;
    class RainSettingsUpdater;
    class Ripples;
    class WaterWaveOcclusion;

    /// Water rendering
    class Water
    {
        osg::ref_ptr<RainSettingsUpdater> mRainSettingsUpdater;

        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<osg::PositionAttitudeTransform> mWaterNode;
        osg::ref_ptr<osg::Geometry> mWaterGeom;
        osg::ref_ptr<osg::Geometry> mSimpleWaterGeom;
        Resource::ResourceSystem* mResourceSystem;
        GpuScene* mGpuScene;
        osg::ref_ptr<osgUtil::IncrementalCompileOperation> mIncrementalCompileOperation;

        std::unique_ptr<RippleSimulation> mSimulation;
        std::unique_ptr<WaterWaveOcclusion> mWaveOcclusion;
        WaterSurfaceParameters mSurfaceParameters;

        osg::ref_ptr<Reflection> mReflection;
        osg::ref_ptr<Ripples> mRipples;

        bool mEnabled;
        bool mToggled;
        float mTop;
        bool mInterior;
        bool mShowWorld;

        osg::Callback* mCullCallback;
        osg::ref_ptr<osg::Callback> mShaderWaterStateSetUpdater;
        bool mUsesDisplacedGeometry = false;
        bool mUsesSurfaceCoverage = false;
        float mSewerRippleTimer = 0.f;
        float mRainIntensity = 0.f;

        osg::Vec3f getSceneNodeCoordinates(int gridX, int gridY);
        void updateVisible();

        void createSimpleWaterStateSet(osg::Node* node, float alpha);

        void createShaderWaterStateSet(osg::Node* node);

        void updateWaterMaterial();

        /// Rebuild only the shader state while retaining reflection and ripple render targets.
        void updateShaderWaterState();

        /// Select the water mesh and synchronize shared near-surface coverage resources.
        void updateWaterGeometry();

    public:
        Water(osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem,
            osgUtil::IncrementalCompileOperation* ico, GpuScene* gpuScene);
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

        osg::Drawable* getDrawable() const;

        /// Return the independently owned policy for the default world-water surface.
        const WaterSurfaceParameters& getSurfaceParameters() const { return mSurfaceParameters; }

        /// Return the current rain contribution used by the active optical field.
        float getRainIntensity() const { return mRainIntensity; }

        /// Apply changed global settings to the default surface while preserving unrelated render targets.
        void processChangedSettings(const Settings::CategorySettingVector& settings);

        void showWorld(bool show);
    };

}

#endif
