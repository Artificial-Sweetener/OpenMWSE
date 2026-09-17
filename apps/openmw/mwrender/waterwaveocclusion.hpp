#ifndef OPENMW_MWRENDER_WATERWAVEOCCLUSION_H
#define OPENMW_MWRENDER_WATERWAVEOCCLUSION_H

#include <osg/Texture2D>
#include <osg/Vec2f>
#include <osg/ref_ptr>

namespace osg
{
    class Camera;
    class Group;
    class Uniform;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    /// Cache top-down near-surface coverage used to suppress covered-water surface effects.
    class WaterWaveOcclusion
    {
    public:
        /// Create an initially dormant water-owned obstruction pass.
        WaterWaveOcclusion(osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem);

        /// Enable or disable obstruction rendering and retain the cached texture while dormant.
        void setEnabled(bool enabled);

        /// Recenter the obstruction field and request one refresh when its snapped origin changes.
        void setCenter(const osg::Vec2f& center);

        /// Set the water plane and request one refresh when its elevation changes.
        void setWaterHeight(float height);

        /// Limit blockers to the conservative vertical band that can interact with the waves.
        void setMaximumClearance(float height);

        /// Retire the bounded refresh scheduled for the current obstruction field.
        void update(float dt);

        /// Return the cached mipmapped near-surface coverage texture.
        osg::Texture2D* texture() const { return mCoverageTexture; }

        /// Return the world-space center represented by the cached texture.
        const osg::Vec2f& center() const { return mCenter; }

        /// Return the reciprocal world-space width represented by the cached texture.
        float inverseWorldSize() const;

        /// Return the edge resolution of the square coverage texture.
        float textureResolution() const;

    private:
        /// Synchronize the orthographic camera with the current snapped field origin and water level.
        void updateCamera();
        /// Schedule enough frames for every active view to observe a refreshed cache.
        void requestRefresh();

        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::Texture2D> mCoverageTexture;
        osg::ref_ptr<osg::Texture2D> mDepthTexture;
        osg::ref_ptr<osg::Uniform> mWaterLevelUniform;
        osg::ref_ptr<osg::Uniform> mMaximumClearanceUniform;
        osg::Vec2f mCenter;
        float mWaterHeight = 0.f;
        float mMaximumClearance = 32.f;
        bool mEnabled = false;
        int mRefreshFrames = 0;
    };
}

#endif
