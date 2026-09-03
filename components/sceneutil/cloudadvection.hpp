#ifndef OPENMW_COMPONENTS_SCENEUTIL_CLOUDADVECTION_H
#define OPENMW_COMPONENTS_SCENEUTIL_CLOUDADVECTION_H

/// @file Share the integrated coverage-to-shape motion mapping across atmosphere render paths.

#include <cmath>

#include <osg/Vec2d>
#include <osg/Vec2f>
#include <osg/Vec3f>

namespace SceneUtil
{
    /// Return the prevailing weather-field direction used by integrated cloud motion.
    inline osg::Vec2d cloudCoverageWindDirection()
    {
        return { 0.78, 0.36 };
    }

    /// Derive rising shape-noise motion from the integrated weather-field phase.
    inline osg::Vec3f cloudShapeAdvectionOffset(const osg::Vec2f& coverageOffsetWorldUnits)
    {
        // The primary shape wind is 15/30 of coverage wind, rotated by .75 radians,
        // with vertical coordinate velocity -0.5 times shape speed. A linear transform
        // preserves accumulated motion across speed changes and deterministic seed offsets.
        constexpr double relativeAngle = 0.75;
        constexpr double speedRatio = 0.5;
        const double cosine = std::cos(relativeAngle);
        const double sine = std::sin(relativeAngle);
        osg::Vec2d coverageDirection = cloudCoverageWindDirection();
        coverageDirection.normalize();
        const osg::Vec2d offset(coverageOffsetWorldUnits);
        return { static_cast<float>((offset.x() * cosine - offset.y() * sine) * speedRatio),
            static_cast<float>((offset.x() * sine + offset.y() * cosine) * speedRatio),
            static_cast<float>((offset * coverageDirection) * -0.5 * speedRatio) };
    }
}

#endif
