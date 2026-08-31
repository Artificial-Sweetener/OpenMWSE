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

#define openmwseCloudMaximumDistance openmwseCloudShadowMaximumDistance
#define openmwseCloudBaseVariation openmwseCloudShadowBaseVariation
#define openmwseCloudMinimumThicknessScale openmwseCloudShadowMinimumThicknessScale
#define openmwseCloudMaximumThicknessScale openmwseCloudShadowMaximumThicknessScale
#define openmwseCloudParameters openmwseCloudShadowParameters
#define openmwseCloudWaterHeight openmwseCloudShadowWaterHeight
#define openmwseCloudAdvectionOffsetWorldUnits openmwseCloudShadowAdvectionOffsetWorldUnits
#define openmwseCloudHash openmwseCloudShadowHash
#define openmwseCloudValueNoise openmwseCloudShadowValueNoise
#define openmwseCloudAdvectedPosition openmwseCloudShadowAdvectedPosition
#define openmwseCloudColumnNoise openmwseCloudShadowColumnNoise
#define openmwseCloudColumnDensity openmwseCloudShadowColumnDensity
#include "cloud_field.glsl"
#undef openmwseCloudColumnDensity
#undef openmwseCloudColumnNoise
#undef openmwseCloudAdvectedPosition
#undef openmwseCloudValueNoise
#undef openmwseCloudHash
#undef openmwseCloudAdvectionOffsetWorldUnits
#undef openmwseCloudWaterHeight
#undef openmwseCloudParameters
#undef openmwseCloudMaximumThicknessScale
#undef openmwseCloudMinimumThicknessScale
#undef openmwseCloudBaseVariation
#undef openmwseCloudMaximumDistance

/// Evaluate Beer transmittance through the shared world-anchored cloud column toward the sun.
float openmwseCloudSunTransmittance(vec3 worldPosition)
{
    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticCloudSurfaceShadowsBypassed)
        return 1.0;

    vec3 sunDirection = normalize(atmosphere.sunDirection);
    if (!atmosphere.enabled || atmosphere.cloudParameters.x <= 0.001 || sunDirection.z <= 0.01)
        return 1.0;

    float layerCenterHeight = waterHeight + atmosphere.cloudParameters.z + atmosphere.cloudParameters.w * 0.5;
    float distanceToLayer = max(layerCenterHeight - worldPosition.z, 0.0) / max(sunDirection.z, 0.08);
    vec2 shadowPosition = worldPosition.xy + sunDirection.xy * distanceToLayer;
    float columnDensity = openmwseCloudShadowColumnDensity(shadowPosition);
    float slantPathScale = 1.0 / max(sunDirection.z, 0.22);
    float opticalDepth = columnDensity * max(atmosphere.cloudParameters.w, 1.0) * 0.000105 * slantPathScale;
    return exp(-opticalDepth);
}

/// Apply cloud-column sun occlusion and broad sky visibility to directional surface lighting.
void openmwseApplyCloudLighting(vec3 worldPosition, inout vec3 diffuseLight, inout vec3 ambientLight,
    inout vec3 specularLight)
{
    float sunTransmittance = openmwseCloudSunTransmittance(worldPosition);
    diffuseLight *= sunTransmittance;
    specularLight *= sunTransmittance;

    float coverage = clamp(atmosphere.cloudParameters.x * atmosphere.cloudParameters.y, 0.0, 1.0);
    float localSkyVisibility = mix(1.0, 0.82 + 0.18 * sqrt(sunTransmittance), coverage * 0.35);
    ambientLight *= localSkyVisibility;
}

#endif
