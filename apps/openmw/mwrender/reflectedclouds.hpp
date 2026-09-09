#ifndef OPENMW_MWRENDER_REFLECTEDCLOUDS_H
#define OPENMW_MWRENDER_REFLECTEDCLOUDS_H

/// @file Schedule current-view cloud transport before reflected sky composition.

#include <components/sceneutil/nodecallback.hpp>

#include <osg/ref_ptr>

#include <map>

namespace osg
{
    class StateSet;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace SceneUtil
{
    class RTTNode;
}

namespace MWRender
{
    /// Render reflected cloud transport independently from full-resolution sky and foreground geometry.
    class ReflectedClouds final : public SceneUtil::NodeCallback<ReflectedClouds, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        /// Retain the atmosphere dome and allocate half-resolution, per-view targets when broad cloud cover first needs
        /// them.
        explicit ReflectedClouds(osg::Node* atmosphere);

        /// Release dependent cameras after their queued render states finish using their textures.
        ~ReflectedClouds() override;

        /// Retain depth rejection for sparse cloud cover and buffer broad decks with hysteresis during transitions.
        void updateCloudCover(float mainCoverage, float lowCoverage);

        /// Match the current reflection viewport and schedule its rays before binding their radiance and transmittance.
        void operator()(osg::Node* node, osgUtil::CullVisitor* visitor);

    private:
        // Keep array cloud textures separate from the ordinary diffuse and moon-mask sampler units.
        static constexpr int sTextureUnit = 2;
        bool mUseCloudTarget = false;
        osg::ref_ptr<osg::Node> mAtmosphere;
        osg::ref_ptr<SceneUtil::RTTNode> mClouds;
        std::map<osgUtil::CullVisitor*, osg::ref_ptr<osg::StateSet>> mViewStates;
    };
}

#endif
