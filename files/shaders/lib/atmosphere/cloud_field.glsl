#ifndef OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL

#include "cloud_field.h.glsl"

/// Translate an episode's stationary noise realization without changing its spatial frequencies.
vec2 openmwseCloudEpisodeOffset(float phase)
{
    return (vec2(cos(phase), sin(phase)) - vec2(1.0, 0.0))
        * (50000.0 * openmwseAtmosphereWorldUnitsPerMeter);
}

/// Recover elapsed wind travel from OpenMW's integrated weather clock, including pauses and speed changes.
float openmwseCloudAnimationSeconds(vec2 integratedDisplacementWorldUnits)
{
    return dot(integratedDisplacementWorldUnits, normalize(vec2(0.78, 0.36)))
        / (30.0 * openmwseAtmosphereWorldUnitsPerMeter);
}

#endif
