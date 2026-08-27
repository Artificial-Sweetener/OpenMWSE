#ifndef OPENMW_MWRENDER_ATMOSPHERE_H
#define OPENMW_MWRENDER_ATMOSPHERE_H

#include <cmath>

#include <osg/Vec3f>

namespace MWRender
{
    inline osg::Vec3f getRenderedExteriorSunPosition(const osg::Vec3f& sunlightDirection)
    {
        osg::Vec3f position = -sunlightDirection;
        position.z() = 400.f - std::abs(position.x());
        return position;
    }

    inline osg::Vec3f getScatteringSunDirection(osg::Vec3f renderedSunDirection, bool isSunVisible)
    {
        // Morrowind bounces the rendered sun above the horizon at night. MGE XE restores its physical trajectory here.
        if (!isSunVisible)
            renderedSunDirection.z() = -std::abs(renderedSunDirection.z());
        renderedSunDirection.normalize();
        return renderedSunDirection;
    }

    struct AtmosphereState
    {
        osg::Vec3f mInScattering{ 0.25f, 0.38f, 0.48f };
        osg::Vec3f mOutScattering{ 0.07f, 0.36f, 0.76f };
        float mWeatherWeight = 0.f;
        bool mEnabled = false;
    };
}

#endif
