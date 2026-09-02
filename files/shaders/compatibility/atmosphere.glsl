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

/// Expose canonical weather-specific cloud morphology to the shared cloud implementation.
vec4 openmwseCloudShapeParameters()
{
    return atmosphere.cloudShapeParameters;
}

/// Expose independently controlled deck and convective organization.
vec4 openmwseCloudOrganizationParameters()
{
    return atmosphere.cloudOrganizationParameters;
}

/// Expose independently controlled tower taper and base variation.
vec4 openmwseCloudStructureParameters()
{
    return atmosphere.cloudStructureParameters;
}

/// Expose the independently placeable low-cloud layer to the shared implementation.
vec4 openmwseLowCloudParameters()
{
    return atmosphere.lowCloudParameters;
}

/// Expose weather and environment morphology for the low-cloud layer.
vec4 openmwseLowCloudShapeParameters()
{
    return atmosphere.lowCloudShapeParameters;
}

/// Expose local terrain height and terrain-following controls to shared cloud height calculations.
vec4 openmwseCloudMediumReferenceParameters()
{
    return atmosphere.mediumReferenceParameters;
}

/// Expose the sparse terrain sample center and gradient used by orographic formation.
vec4 openmwseCloudOrographicParameters()
{
    return atmosphere.orographicParameters;
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

/// Sample the canonical tileable multi-channel cloud shape volume.
vec4 openmwseCloudNoiseSample(vec3 coordinates)
{
    return texture3D(openmwseCloudNoiseTexture, fract(coordinates));
}

/// Sample the independent high-frequency cloud erosion volume.
vec4 openmwseCloudDetailNoiseSample(vec3 coordinates)
{
    return texture3D(openmwseCloudDetailNoiseTexture, fract(coordinates));
}

/// Sample the canonical broad cloud-organization and morphology field.
vec4 openmwseCloudWeatherSample(vec2 coordinates)
{
    return texture2D(openmwseCloudWeatherTexture, fract(coordinates));
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
    parameters.solarRadiance = atmosphere.solarRadiance;
    parameters.skyIrradiance = atmosphere.skyIrradiance;
    parameters.groundIrradiance = atmosphere.groundIrradiance;
    parameters.weatherWeight = atmosphere.weatherWeight;
    parameters.fogStart = fog.start;
    parameters.fogEnd = fog.end;
#if @exponentialFog
    parameters.exponentialFog = 1.0;
#else
    parameters.exponentialFog = 0.0;
#endif
    parameters.referenceHeight = waterHeight;
    parameters.observerHeight = atmosphereEyePosition().z;
    parameters.weatherCloudTopHeight = openmwseCloudLayerHeightBounds(openmwseMainCloudLayer()).y;
    parameters.weatherCloudCoverage = atmosphere.cloudParameters.x;
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
    return openmwseAtmosphereSkyRadiance(worldDirection, openmwseAerialPerspectiveParameters());
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
    vec3 geometricSunDirection = normalize(atmosphere.sunDirection);
    vec3 apparentSunDirection = openmwseApparentSunDirection(geometricSunDirection);
    vec3 geometricViewDirection = openmwseGeometricDirectionFromApparent(worldDirection);
    float discAngularDistance
        = sqrt(max(2.0 * (1.0 - dot(geometricViewDirection, geometricSunDirection)), 0.0));
    float haloAngularDistance
        = sqrt(max(2.0 * (1.0 - dot(worldDirection, apparentSunDirection)), 0.0));
    float horizonVisibility = smoothstep(-0.034, -0.002, apparentSunDirection.z);
    float pixelHorizonVisibility = smoothstep(-0.001, 0.002, worldDirection.z);
    float disc = 1.0 - smoothstep(0.026, 0.03, discAngularDistance);
    float halo = exp(-haloAngularDistance * haloAngularDistance * 360.0) * 0.32;
    vec3 sunColor = mix(vec3(1.0), atmosphere.sunDiscColor.rgb, 0.78);
    return sunColor * (disc * 3.2 + halo) * atmosphere.sunDiscColor.a * horizonVisibility * pixelHorizonVisibility;
}

#endif
