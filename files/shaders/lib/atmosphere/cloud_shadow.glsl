#ifndef OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_GLSL

#include "lib/atmosphere/uniforms.glsl"
#include "lib/atmosphere/diagnostics.glsl"

uniform float waterHeight;

/// Expose surface-lighting cloud parameters to the shared cloud-field implementation.
vec4 openmwseCloudShadowParameters()
{
    return atmosphere.cloudParameters;
}

/// Expose surface-lighting cloud morphology to the shared cloud-field implementation.
vec4 openmwseCloudShadowShapeParameters()
{
    return atmosphere.cloudShapeParameters;
}

/// Expose surface-lighting cloud organization to the shared cloud-field implementation.
vec4 openmwseCloudShadowOrganizationParameters()
{
    return atmosphere.cloudOrganizationParameters;
}

/// Expose surface-lighting cloud structure to the shared cloud-field implementation.
vec4 openmwseCloudShadowStructureParameters()
{
    return atmosphere.cloudStructureParameters;
}

/// Expose the exterior water datum to the shared cloud-field implementation.
float openmwseCloudShadowWaterHeight()
{
    return waterHeight;
}

/// Expose integrated world-space cloud displacement to the shared cloud-field implementation.
vec2 openmwseCloudShadowAdvectionOffsetWorldUnits()
{
    return atmosphere.cloudAdvectionOffsetWorldUnits;
}

/// Sample the canonical tileable multi-channel cloud volume for surface shadowing.
vec4 openmwseCloudShadowNoiseSample(vec3 coordinates)
{
    return texture3D(openmwseCloudNoiseTexture, fract(coordinates));
}

/// Sample the canonical broad cloud-organization field for surface shadowing.
vec4 openmwseCloudShadowWeatherSample(vec2 coordinates)
{
    return texture2D(openmwseCloudWeatherTexture, fract(coordinates));
}

#define openmwseCloudMaximumMarchingDistance openmwseCloudShadowMaximumMarchingDistance
#define openmwseCloudMaximumEntryDistance openmwseCloudShadowMaximumEntryDistance
#define openmwseCloudMaximumRayDistance openmwseCloudShadowMaximumRayDistance
#define openmwseCloudBaseVariation openmwseCloudShadowBaseVariation
#define openmwseCloudMinimumThicknessScale openmwseCloudShadowMinimumThicknessScale
#define openmwseCloudMaximumThicknessScale openmwseCloudShadowMaximumThicknessScale
#define openmwseCloudReferenceShapeNoiseScale openmwseCloudShadowReferenceShapeNoiseScale
#define openmwseCloudReferenceWeatherScale openmwseCloudShadowReferenceWeatherScale
#define openmwseCloudLowFormationScale openmwseCloudShadowLowFormationScale
#define openmwseCloudParameters openmwseCloudShadowParameters
#define openmwseCloudShapeParameters openmwseCloudShadowShapeParameters
#define openmwseCloudOrganizationParameters openmwseCloudShadowOrganizationParameters
#define openmwseCloudStructureParameters openmwseCloudShadowStructureParameters
#define openmwseCloudWaterHeight openmwseCloudShadowWaterHeight
#define openmwseCloudAdvectionOffsetWorldUnits openmwseCloudShadowAdvectionOffsetWorldUnits
#define openmwseCloudNoiseSample openmwseCloudShadowNoiseSample
#define openmwseCloudWeatherSample openmwseCloudShadowWeatherSample
#define openmwseCloudRemap openmwseCloudShadowRemap
#define openmwseCloudRemapUnclamped openmwseCloudShadowRemapUnclamped
#define openmwseCloudBaseFromNoise openmwseCloudShadowBaseFromNoise
#define openmwseCloudHash openmwseCloudShadowHash
#define openmwseCloudValueNoise openmwseCloudShadowValueNoise
#define openmwseCloudAdvectedPosition openmwseCloudShadowAdvectedPosition
#define openmwseCloudWeatherCoordinates openmwseCloudShadowWeatherCoordinates
#define openmwseCloudWeatherCoordinatesForScale openmwseCloudShadowWeatherCoordinatesForScale
#define openmwseCloudConvectiveWeatherCoordinates openmwseCloudShadowConvectiveWeatherCoordinates
#define openmwseCloudColumnProperties openmwseCloudShadowColumnProperties
#define openmwseCloudColumnPropertiesForScale openmwseCloudShadowColumnPropertiesForScale
#define openmwseCloudColumnNoise openmwseCloudShadowColumnNoise
#define openmwseCloudLocalCoverage openmwseCloudShadowLocalCoverage
#define openmwseCloudColumnOccupancy openmwseCloudShadowColumnOccupancy
#define openmwseCloudColumnDensity openmwseCloudShadowColumnDensity
#include "cloud_field.glsl"
#undef openmwseCloudColumnDensity
#undef openmwseCloudColumnOccupancy
#undef openmwseCloudLocalCoverage
#undef openmwseCloudColumnNoise
#undef openmwseCloudColumnProperties
#undef openmwseCloudColumnPropertiesForScale
#undef openmwseCloudConvectiveWeatherCoordinates
#undef openmwseCloudWeatherCoordinates
#undef openmwseCloudWeatherCoordinatesForScale
#undef openmwseCloudAdvectedPosition
#undef openmwseCloudValueNoise
#undef openmwseCloudHash
#undef openmwseCloudAdvectionOffsetWorldUnits
#undef openmwseCloudBaseFromNoise
#undef openmwseCloudRemapUnclamped
#undef openmwseCloudRemap
#undef openmwseCloudNoiseSample
#undef openmwseCloudWeatherSample
#undef openmwseCloudWaterHeight
#undef openmwseCloudParameters
#undef openmwseCloudShapeParameters
#undef openmwseCloudOrganizationParameters
#undef openmwseCloudStructureParameters
#undef openmwseCloudMaximumThicknessScale
#undef openmwseCloudLowFormationScale
#undef openmwseCloudReferenceWeatherScale
#undef openmwseCloudReferenceShapeNoiseScale
#undef openmwseCloudMinimumThicknessScale
#undef openmwseCloudBaseVariation
#undef openmwseCloudMaximumRayDistance
#undef openmwseCloudMaximumEntryDistance
#undef openmwseCloudMaximumMarchingDistance

/// Evaluate normalized low-cloud optical density from the shared weather field.
float openmwseLowCloudShadowColumnDensity(vec2 worldPosition)
{
    vec4 parameters = atmosphere.lowCloudParameters;
    vec4 shape = atmosphere.lowCloudShapeParameters;
    vec3 advectedPosition = openmwseCloudShadowAdvectedPosition(vec3(worldPosition, 0.0));
    advectedPosition.xy += vec2(137000.0, -91000.0);
    OpenMWSECloudColumnProperties properties
        = openmwseCloudShadowColumnPropertiesForScale(advectedPosition.xy, parameters, shape,
            vec4(0.72, 0.08, 0.16, 0.0), vec4(0.72, 0.42, 0.0, 0.0),
            openmwseCloudShadowLowFormationScale);
    return smoothstep(0.08, 0.82, properties.localCoverage) * max(parameters.y, 0.0);
}

/// Evaluate Beer transmittance through the shared world-anchored cloud column toward the sun.
float openmwseCloudSunTransmittance(vec3 worldPosition)
{
    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticCloudSurfaceShadowsBypassed)
        return 1.0;

    vec3 sunDirection = normalize(atmosphere.sunDirection);
    bool mainCloudEnabled = atmosphere.cloudParameters.x > 0.001 && atmosphere.cloudParameters.y > 0.001;
    bool lowCloudEnabled = atmosphere.lowCloudParameters.x > 0.001 && atmosphere.lowCloudParameters.y > 0.001;
    if (!atmosphere.enabled || (!mainCloudEnabled && !lowCloudEnabled) || sunDirection.z <= 0.01)
        return 1.0;

    float slantPathScale = 1.0 / max(sunDirection.z, 0.22);
    float opticalDepth = 0.0;
    if (mainCloudEnabled)
    {
        float layerCenterHeight
            = waterHeight + atmosphere.cloudParameters.z + atmosphere.cloudParameters.w * 0.5;
        float distanceToLayer = max(layerCenterHeight - worldPosition.z, 0.0) / max(sunDirection.z, 0.08);
        vec2 shadowPosition = worldPosition.xy + sunDirection.xy * distanceToLayer;
        float columnDensity = openmwseCloudShadowColumnDensity(shadowPosition);
        opticalDepth += columnDensity * max(atmosphere.cloudParameters.w, 1.0) * 0.000105 * slantPathScale;
    }
    if (lowCloudEnabled)
    {
        float layerCenterHeight
            = waterHeight + atmosphere.lowCloudParameters.z + atmosphere.lowCloudParameters.w * 0.5;
        float distanceToLayer = max(layerCenterHeight - worldPosition.z, 0.0) / max(sunDirection.z, 0.08);
        vec2 shadowPosition = worldPosition.xy + sunDirection.xy * distanceToLayer;
        float columnDensity = openmwseLowCloudShadowColumnDensity(shadowPosition);
        opticalDepth += columnDensity * max(atmosphere.lowCloudParameters.w, 1.0) * 0.000105 * slantPathScale;
    }
    return exp(-opticalDepth);
}

/// Apply cloud-column sun occlusion and broad sky visibility to directional surface lighting.
void openmwseApplyCloudLighting(vec3 worldPosition, inout vec3 diffuseLight, inout vec3 ambientLight,
    inout vec3 specularLight)
{
    float sunTransmittance = openmwseCloudSunTransmittance(worldPosition);
    diffuseLight *= sunTransmittance;
    specularLight *= sunTransmittance;

    float mainCoverage = clamp(atmosphere.cloudParameters.x * atmosphere.cloudParameters.y, 0.0, 1.0);
    float lowCoverage = clamp(atmosphere.lowCloudParameters.x * atmosphere.lowCloudParameters.y, 0.0, 1.0);
    float coverage = 1.0 - (1.0 - mainCoverage) * (1.0 - lowCoverage);
    float localSkyVisibility = mix(1.0, 0.82 + 0.18 * sqrt(sunTransmittance), coverage * 0.35);
    ambientLight *= localSkyVisibility;
}

#endif
