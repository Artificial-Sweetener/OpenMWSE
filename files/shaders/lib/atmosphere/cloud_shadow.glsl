#ifndef OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_GLSL

#include "lib/atmosphere/uniforms.glsl"
#include "lib/atmosphere/diagnostics.glsl"
#include "lib/atmosphere/textured_clouds.h.glsl"

uniform float waterHeight;

#include "cloud_field.h.glsl"
#include "cloud_shadow_column.h.glsl"

/// Read the same complete source model used to render this weather episode.
CloudModelLayer openmwseCloudShadowModel(int layerIndex)
{
    vec4 parameters[14];
    for (int field = 0; field < 14; ++field)
        parameters[field] = atmosphere.cloudLayers[layerIndex * 14 + field];
    return cloudModelDecodeLayer(parameters,
        openmwseCloudAnimationSeconds(atmosphere.cloudAdvectionOffsetWorldUnits));
}

/// Evaluate Beer transmittance through the shared world-anchored cloud column toward the sun.
float openmwseCloudSunTransmittance(vec3 worldPosition)
{
    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticCloudSurfaceShadowsBypassed)
        return 1.0;

    vec3 sunDirection = normalize(atmosphere.sunDirection);
    if (openmwseTexturedCloudsEnabled)
        return sunDirection.z > 0.01 ? openmwseTexturedCloudTransmittance(worldPosition, sunDirection) : 1.0;
    float incomingWeight = clamp(atmosphere.cloudScenarioParameters.z, 0.0, 1.0)
        * step(0.5, atmosphere.cloudScenarioParameters.w);
    float outgoingWeight = 1.0 - incomingWeight;
    bool mainCloudEnabled = atmosphere.cloudParameters.x > 0.001
        && atmosphere.cloudParameters.y * outgoingWeight > 0.001;
    bool incomingCloudEnabled = atmosphere.incomingCloudParameters.x > 0.001
        && atmosphere.incomingCloudParameters.y * incomingWeight > 0.001;
    bool lowCloudEnabled = atmosphere.lowCloudParameters.x > 0.001 && atmosphere.lowCloudParameters.y > 0.001;
    if (!atmosphere.enabled || (!mainCloudEnabled && !incomingCloudEnabled && !lowCloudEnabled)
        || sunDirection.z <= 0.01)
        return 1.0;

    float opticalDepth = 0.0;
    if (mainCloudEnabled)
    {
        vec4 parameters = atmosphere.cloudParameters;
        parameters.y *= outgoingWeight;
        opticalDepth += openmwseCloudShadowColumnOpticalDepth(worldPosition, sunDirection,
            parameters, waterHeight, openmwseCloudEpisodeOffset(atmosphere.cloudScenarioParameters.x),
            openmwseCloudShadowModel(0), openmwseCloudWeatherTexture);
    }
    if (incomingCloudEnabled)
    {
        vec4 parameters = atmosphere.incomingCloudParameters;
        parameters.y *= incomingWeight;
        opticalDepth += openmwseCloudShadowColumnOpticalDepth(worldPosition, sunDirection,
            parameters, waterHeight, openmwseCloudEpisodeOffset(atmosphere.cloudScenarioParameters.y),
            openmwseCloudShadowModel(1), openmwseCloudWeatherTexture);
    }
    if (lowCloudEnabled)
    {
        opticalDepth += openmwseCloudShadowColumnOpticalDepth(worldPosition, sunDirection,
            atmosphere.lowCloudParameters, waterHeight, vec2(137000.0, -91000.0),
            openmwseCloudShadowModel(2), openmwseCloudWeatherTexture);
    }
    return exp(-opticalDepth);
}

/// Apply cloud-column sun occlusion and broad sky visibility to directional surface lighting.
void openmwseApplyCloudLighting(vec3 worldPosition, inout vec3 diffuseLight, inout vec3 ambientLight,
    inout vec3 specularLight)
{
    if (!atmosphere.cloudShadowsEnabled
        || (!openmwseTexturedCloudsEnabled && (!atmosphere.cloudsEnabled || !atmosphere.enabled)))
        return;
    float sunTransmittance = openmwseCloudSunTransmittance(worldPosition);
    diffuseLight *= sunTransmittance;
    specularLight *= sunTransmittance;

    if (openmwseTexturedCloudsEnabled)
        return;

    float incomingWeight = clamp(atmosphere.cloudScenarioParameters.z, 0.0, 1.0)
        * step(0.5, atmosphere.cloudScenarioParameters.w);
    float mainCoverage = clamp(atmosphere.cloudParameters.x * atmosphere.cloudParameters.y
        * (1.0 - incomingWeight), 0.0, 1.0);
    float incomingCoverage = clamp(atmosphere.incomingCloudParameters.x
        * atmosphere.incomingCloudParameters.y * incomingWeight, 0.0, 1.0);
    mainCoverage = 1.0 - (1.0 - mainCoverage) * (1.0 - incomingCoverage);
    float lowCoverage = clamp(atmosphere.lowCloudParameters.x * atmosphere.lowCloudParameters.y, 0.0, 1.0);
    float coverage = 1.0 - (1.0 - mainCoverage) * (1.0 - lowCoverage);
    float localSkyVisibility = mix(1.0, 0.82 + 0.18 * sqrt(sunTransmittance), coverage * 0.35);
    ambientLight *= localSkyVisibility;
}

#endif
