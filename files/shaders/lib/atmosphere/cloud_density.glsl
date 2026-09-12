#ifndef OPENMWSE_ATMOSPHERE_CLOUD_DENSITY_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_DENSITY_GLSL

#include "cloud_geometry.glsl"

/// Carry density and independently authored precipitation optics from one cloud sample.
struct OpenMWSECloudDensitySample
{
    float density;
    float precipitation;
};

/// Prefilter continuously with view distance so Morrowind's compressed layers do not cross discrete density shells.
float openmwseCloudNoiseLod(float distanceFromEye, float additionalLodBias, OpenMWSECloudLayer layer)
{
    float featureScale = layer.model.physicalScale;
    float viewLod = max(distanceFromEye, 0.0) / (openmwseCloudNoiseLodDistance * featureScale);
    return clamp(viewLod, 0.0, 3.0) + additionalLodBias;
}

/// Evaluate the source density model while keeping placement, environmental influence, and optical weight outside it.
OpenMWSECloudDensitySample openmwseCloudDensitySample(vec3 worldPosition, float distanceFromEye,
    float additionalLodBias, OpenMWSECloudLayer layer)
{
    vec3 fieldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    OpenMWSECloudFormation formation = openmwseCloudFormation(fieldPosition, layer);
    float heightFraction = openmwseCloudHeightFraction(fieldPosition.z, formation.baseHeight, layer.parameters.w);
    if (heightFraction < 0.0 || heightFraction > 1.0)
        return OpenMWSECloudDensitySample(0.0, 0.0);
    vec3 positionMeters = openmwseCloudModelPosition(fieldPosition, layer);
    vec3 weather = cloudModelSampleWeather(positionMeters, heightFraction, layer.model);
    float density = cloudModelSampleDensity(positionMeters, heightFraction, layer.model, weather,
        openmwseCloudNoiseLod(distanceFromEye, additionalLodBias, layer), true);
    density *= max(layer.parameters.y, 0.0) * formation.densityScale;
    return OpenMWSECloudDensitySample(density, weather.b);
}

/// Evaluate only cloud density for shadow, occupancy, and compatibility callers.
float openmwseCloudDensity(vec3 worldPosition, float distanceFromEye,
    float additionalLodBias, OpenMWSECloudLayer layer)
{
    return openmwseCloudDensitySample(
        worldPosition, distanceFromEye, additionalLodBias, layer).density;
}

/// Integrate density and density-weighted precipitation with two-point Gauss quadrature.
OpenMWSECloudDensitySample openmwseCloudSegmentDensity(vec3 origin, vec3 direction,
    float startDistance, float segmentLength, OpenMWSECloudLayer layer)
{
    float nearDistance = startDistance + segmentLength * openmwseCloudQuadratureOffsetLow;
    float farDistance = startDistance + segmentLength * openmwseCloudQuadratureOffsetHigh;
    OpenMWSECloudDensitySample nearSample = openmwseCloudDensitySample(
        origin + direction * nearDistance, nearDistance, 0.0, layer);
    OpenMWSECloudDensitySample farSample = openmwseCloudDensitySample(
        origin + direction * farDistance, farDistance, 0.0, layer);
    float densitySum = nearSample.density + farSample.density;
    float rainDensity = nearSample.density * nearSample.precipitation
        + farSample.density * farSample.precipitation;
    return OpenMWSECloudDensitySample(0.5 * densitySum, rainDensity / max(densitySum, 0.0001));
}

/// Resolve fine condensate extinction independently from its broader illumination sampling interval.
OpenMWSECloudDensitySample openmwseCondensateSegmentDensity(vec3 origin, vec3 direction,
    float startDistance, float segmentLength, OpenMWSECloudLayer layer)
{
    float density = 0.0;
    float rainDensity = 0.0;
    for (int index = 0; index < 4; ++index)
    {
        float distance = startDistance + segmentLength * openmwseCondensateQuadratureOffsets[index];
        OpenMWSECloudDensitySample sample = openmwseCloudDensitySample(
            origin + direction * distance, distance, 0.0, layer);
        float contribution = sample.density * openmwseCondensateQuadratureWeights[index];
        density += contribution;
        rainDensity += contribution * sample.precipitation;
    }
    return OpenMWSECloudDensitySample(density, rainDensity / max(density, 0.0001));
}

/// Evaluate the sampleDetail=false path for stable temporal occupancy classification.
float openmwseCloudBroadDensity(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    vec3 fieldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    OpenMWSECloudFormation formation = openmwseCloudFormation(fieldPosition, layer);
    float heightFraction = openmwseCloudHeightFraction(fieldPosition.z, formation.baseHeight, layer.parameters.w);
    if (heightFraction < 0.0 || heightFraction > 1.0)
        return 0.0;
    vec3 positionMeters = openmwseCloudModelPosition(fieldPosition, layer);
    vec3 weather = cloudModelSampleWeather(positionMeters, heightFraction, layer.model);
    return cloudModelSampleDensity(positionMeters, heightFraction, layer.model, weather, 0.0, false)
        * max(layer.parameters.y, 0.0) * formation.densityScale;
}

/// Evaluate stable broad density from every enabled stratum for temporal classification.
float openmwseCloudBroadDensity(vec3 worldPosition)
{
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer incomingLayer = openmwseIncomingCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    float density = openmwseCloudLayerEnabled(mainLayer)
        ? openmwseCloudBroadDensity(worldPosition, mainLayer) : 0.0;
    if (openmwseCloudLayerEnabled(incomingLayer))
        density += openmwseCloudBroadDensity(worldPosition, incomingLayer);
    if (openmwseCloudLayerEnabled(lowLayer))
        density += openmwseCloudBroadDensity(worldPosition, lowLayer);
    return density;
}

/// Evaluate stable low-frequency occupancy for the cloud strata active in one integration.
float openmwseCloudBroadDensityForLayers(vec3 worldPosition, bool mainLayerEnabled,
    bool incomingLayerEnabled, bool lowLayerEnabled, OpenMWSECloudLayer mainLayer,
    OpenMWSECloudLayer incomingLayer, OpenMWSECloudLayer lowLayer)
{
    float density = 0.0;
    if (mainLayerEnabled)
        density += openmwseCloudBroadDensity(worldPosition, mainLayer);
    if (incomingLayerEnabled)
        density += openmwseCloudBroadDensity(worldPosition, incomingLayer);
    if (lowLayerEnabled)
        density += openmwseCloudBroadDensity(worldPosition, lowLayer);
    return density;
}

#endif
