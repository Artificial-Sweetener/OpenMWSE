#ifndef OPENMWSE_SOLAR_GLSL
#define OPENMWSE_SOLAR_GLSL

#include "aerial_perspective.glsl"

/// Draw the refracted scene-linear sun disc while leaving broad glow to atmospheric scattering.
vec3 openmwseSolarDisc(vec3 worldDirection, vec3 sunDirection, vec4 sunDiscColor)
{
    vec3 geometricSunDirection = normalize(sunDirection);
    vec3 apparentSunDirection = openmwseApparentSunDirection(geometricSunDirection);
    vec3 geometricViewDirection = openmwseGeometricDirectionFromApparent(worldDirection);
    float discAngularDistance
        = sqrt(max(2.0 * (1.0 - dot(geometricViewDirection, geometricSunDirection)), 0.0));
    float horizonVisibility = smoothstep(-0.034, -0.002, apparentSunDirection.z);
    float pixelHorizonVisibility = smoothstep(-0.001, 0.002, worldDirection.z);
    float disc = 1.0 - smoothstep(0.026, 0.03, discAngularDistance);
    vec3 sunColor = mix(vec3(1.0), sunDiscColor.rgb, 0.78);
    return sunColor * disc * 3.2 * sunDiscColor.a * horizonVisibility * pixelHorizonVisibility;
}


#endif
