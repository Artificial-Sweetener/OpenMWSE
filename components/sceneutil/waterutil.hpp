#ifndef OPENMW_COMPONENTS_WATERUTIL_H
#define OPENMW_COMPONENTS_WATERUTIL_H

#include <osg/ref_ptr>

namespace osg
{
    class Geometry;
    class StateSet;
}

namespace SceneUtil
{
    /// Allow authored surface coverings to sit slightly above or below the nominal water plane.
    inline constexpr float waterSurfaceInteractionTolerance = 4.f;

    /// Bound cached obstruction heights used by reflected and transmitted water-light paths.
    inline constexpr float waterCausticInteractionHeight = 1800.f;

    /// Return the conservative above-plane clearance that can interact with the configured geometric waves.
    float waterWaveInteractionHeight(float waveHeight);

    /// Create the existing uniformly subdivided flat water plane.
    osg::ref_ptr<osg::Geometry> createWaterGeometry(float size, int segments, float textureRepeats);

    /// Create the camera-centered radial topology used by MGE XE for geometric water displacement.
    osg::ref_ptr<osg::Geometry> createDisplacedWaterGeometry(float size, float textureRepeats);

    /// Create fixed-function-compatible simple-water render state.
    osg::ref_ptr<osg::StateSet> createSimpleWaterStateSet(float alpha, int renderBin);
}

#endif
