#ifndef OPENMW_COMPATIBILITY_ATMOSPHERE_GLSL
#define OPENMW_COMPATIBILITY_ATMOSPHERE_GLSL

#include "lib/util/dither.glsl"
#include "lib/atmosphere/aerial_perspective.glsl"

#include "lib/atmosphere/uniforms.glsl"

uniform float osg_SimulationTime;

/// Expose canonical cloud controls to the shared cloud implementation.
vec4 openmwseCloudParameters()
{
    return atmosphere.cloudParameters;
}

/// Expose the exterior water datum to shared cloud height calculations.
float openmwseCloudWaterHeight()
{
    return waterHeight;
}

/// Expose integrated world-space cloud displacement to deterministic cloud advection.
vec2 openmwseCloudAdvectionOffsetWorldUnits()
{
    return atmosphere.cloudAdvectionOffsetWorldUnits;
}

#include "lib/atmosphere/clouds.glsl"

/// Transform a view-space ray into the reflected or main-view world-space convention.
vec3 atmosphereWorldDirection(vec3 viewDirection)
{
    vec3 direction = (osg_ViewMatrixInverse * vec4(viewDirection, 0.0)).xyz;
    if (isReflection)
        direction.z = -direction.z;
    return normalize(direction);
}

/// Return the main or water-reflected camera position in world space.
vec3 atmosphereEyePosition()
{
    vec3 position = osg_ViewMatrixInverse[3].xyz;
    if (isReflection)
        position.z = 2.0 * waterHeight - position.z;
    return position;
}

/// Assemble shared aerial-perspective parameters from compatibility shader uniforms.
OpenMWSEAerialPerspectiveParameters openmwseAerialPerspectiveParameters()
{
    OpenMWSEAerialPerspectiveParameters parameters;
    parameters.inScattering = atmosphere.inScattering;
    parameters.outScattering = atmosphere.outScattering;
    parameters.skyColor = atmosphere.skyColor.rgb;
    parameters.fogColor = fog.color.rgb;
    parameters.sunDirection = atmosphere.sunDirection;
    parameters.weatherWeight = atmosphere.weatherWeight;
    parameters.fogStart = fog.start;
    parameters.fogEnd = fog.end;
#if @exponentialFog
    parameters.exponentialFog = 1.0;
#else
        parameters.exponentialFog = 0.0;
#endif
    parameters.referenceHeight = waterHeight;
    return parameters;
}

/// Evaluate compatibility-shader atmospheric scattering for a view-space direction.
vec3 atmosphereScatter(vec3 viewDirection, float opticalDistance, float visibility, vec3 baseColor)
{
    vec3 worldDirection = atmosphereWorldDirection(viewDirection);
    return openmwseAtmosphereScatteredLight(
        worldDirection, opticalDistance, visibility, baseColor, openmwseAerialPerspectiveParameters());
}

/// Evaluate the directional sky color used by main and reflected views.
vec3 atmosphereSkyColor(vec3 viewDirection)
{
    vec3 worldDirection = atmosphereWorldDirection(viewDirection);
    float skyAmount = 1.0 - pow(
        clamp(1.0 - 2.22 * clamp(worldDirection.z - 0.075, 0.0, 1.0), 0.0, 1.0), 1.15);
    vec3 directionalSky = mix(fog.color.xyz, atmosphere.skyColor.xyz, skyAmount);
    return atmosphereScatter(viewDirection, 1.0, 0.0, directionalSky);
}

/// Integrate deterministic cloud radiance for sky and water-reflection rendering.
vec4 atmosphereCloudScattering(vec3 worldDirection, vec3 atmosphericTarget)
{
    if (atmosphere.enabled == false)
        return vec4(0.0, 0.0, 0.0, 1.0);
    return openmwseIntegrateCloudsStable(atmosphereEyePosition(), worldDirection, openmwseCloudMaximumDistance,
        atmosphere.quality, atmosphere.solarRadiance, atmosphere.skyIrradiance, atmosphere.groundIrradiance,
        atmosphericTarget, openmwseAerialPerspectiveParameters());
}

/// Draw the scene-linear analytic sun disc and halo above the visible horizon.
vec3 atmosphereSunDisc(vec3 worldDirection)
{
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    float angularDistance = sqrt(max(0.0, 2.0 * (1.0 - dot(worldDirection, sunDirection))));
    float horizonVisibility = smoothstep(-0.012, 0.018, sunDirection.z);
    float pixelHorizonVisibility = smoothstep(-0.001, 0.002, worldDirection.z);
    float disc = 1.0 - smoothstep(0.026, 0.03, angularDistance);
    float halo = exp(-angularDistance * angularDistance * 360.0) * 0.32;
    vec3 sunColor = mix(vec3(1.0), atmosphere.sunDiscColor.rgb, 0.78);
    return sunColor * (disc * 3.2 + halo) * atmosphere.sunDiscColor.a * horizonVisibility * pixelHorizonVisibility;
}

#endif
