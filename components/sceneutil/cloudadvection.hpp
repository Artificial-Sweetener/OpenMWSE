#ifndef OPENMW_COMPONENTS_SCENEUTIL_CLOUDADVECTION_H
#define OPENMW_COMPONENTS_SCENEUTIL_CLOUDADVECTION_H

/// @file Define the prevailing direction of OpenMW's integrated weather clock.

#include <osg/Vec2d>

namespace SceneUtil
{
    /// Return the prevailing weather-field direction used by integrated cloud motion.
    inline osg::Vec2d cloudCoverageWindDirection()
    {
        return { 0.78, 0.36 };
    }

}

#endif
