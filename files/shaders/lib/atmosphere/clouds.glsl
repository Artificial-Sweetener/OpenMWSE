#ifndef OPENMWSE_ATMOSPHERE_CLOUDS_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUDS_GLSL

#include "cloud_field.glsl"

/// Describe one independently placeable cloud stratum consumed by the shared density and transport path.
struct OpenMWSECloudLayer
{
    vec4 parameters;
    vec4 shape;
    vec4 organization;
    vec4 structure;
    vec2 fieldOffsetWorldUnits;
    float terrainCoupling;
};

// Use an Earth-scale 6360 km cloud datum. Only the main cloud shell and its optical paths
// use this sphere; Morrowind terrain and the deterministic horizontal density field remain uncurved.
const float openmwseCloudVirtualPlanetRadius
    = 6360000.0 * openmwseCloudWorldUnitsPerMeter;

/// Return the main weather-cloud stratum.
OpenMWSECloudLayer openmwseMainCloudLayer()
{
    return OpenMWSECloudLayer(openmwseCloudParameters(), openmwseCloudShapeParameters(),
        openmwseCloudOrganizationParameters(), openmwseCloudStructureParameters(), vec2(0.0), 0.0);
}

/// Return the low environmental-cloud stratum with an independent deterministic field phase.
OpenMWSECloudLayer openmwseLowCloudLayer()
{
    return OpenMWSECloudLayer(
        openmwseLowCloudParameters(), openmwseLowCloudShapeParameters(), vec4(0.72, 0.08, 0.16, 0.0),
        vec4(0.72, 0.42, 0.0, 0.0), vec2(137000.0, -91000.0), 1.0);
}

/// Return the fixed virtual-planet center for OpenMW's world-space tangent frame.
vec3 openmwseCloudVirtualPlanetCenter()
{
    return vec3(0.0, 0.0, openmwseCloudWaterHeight() - openmwseCloudVirtualPlanetRadius);
}

/// Convert a physical world position to altitude above the virtual spherical cloud datum.
float openmwseCloudVirtualAltitude(vec3 worldPosition)
{
    return length(worldPosition - openmwseCloudVirtualPlanetCenter())
        - openmwseCloudVirtualPlanetRadius + openmwseCloudWaterHeight();
}

/// Return the position consumed by cloud formation while retaining stable world-space horizontal coordinates.
vec3 openmwseCloudFieldPosition(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    if (layer.terrainCoupling > 0.0)
        return worldPosition;
    worldPosition.z = openmwseCloudVirtualAltitude(worldPosition);
    return worldPosition;
}

/// Return whether one cloud stratum can contribute visible density.
bool openmwseCloudLayerEnabled(OpenMWSECloudLayer layer)
{
    return layer.parameters.x > 0.001 && layer.parameters.y > 0.001;
}

/// Return the local influence of the sparsely sampled terrain model without projecting it across the whole world.
float openmwseCloudTerrainInfluence(vec2 worldPosition)
{
    vec2 sampleCenter = openmwseCloudOrographicParameters().xy;
    float distanceFromSample = length(worldPosition - sampleCenter);
    return 1.0 - smoothstep(8192.0, 20480.0, distanceFromSample);
}

/// Return local terrain height from the cached center sample and first-order world-space slope.
float openmwseCloudTerrainHeight(vec2 worldPosition)
{
    vec4 mediumReference = openmwseCloudMediumReferenceParameters();
    vec4 orography = openmwseCloudOrographicParameters();
    vec2 localOffset = clamp(worldPosition - orography.xy, vec2(-20480.0), vec2(20480.0));
    return mediumReference.x + dot(localOffset, orography.zw);
}

/// Return a cloud base that follows local terrain only for the low environmental stratum.
float openmwseCloudNominalBaseHeight(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    float fixedBaseHeight = openmwseCloudWaterHeight() + layer.parameters.z;
    vec4 mediumReference = openmwseCloudMediumReferenceParameters();
    float terrainInfluence = layer.terrainCoupling * layer.shape.w * mediumReference.y
        * openmwseCloudTerrainInfluence(worldPosition.xy);
    float terrainBaseHeight = openmwseCloudTerrainHeight(worldPosition.xy) + mediumReference.z;
    return mix(fixedBaseHeight, terrainBaseHeight, clamp(terrainInfluence, 0.0, 1.0));
}

/// Return terrain-driven windward formation or leeward dissipation for the low environmental stratum.
float openmwseCloudOrographicDensityScale(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    if (layer.terrainCoupling <= 0.0)
        return 1.0;
    vec2 terrainGradient = openmwseCloudOrographicParameters().zw;
    vec2 cloudMotionDirection = normalize(vec2(-0.78, -0.36));
    float slopeAlongWind = dot(terrainGradient, cloudMotionDirection);
    float windwardFormation = smoothstep(0.025, 0.22, slopeAlongWind);
    float leewardDissipation = smoothstep(0.025, 0.22, -slopeAlongWind);
    float response = 1.0 + windwardFormation * 0.85 - leewardDissipation * 0.7;
    float influence = layer.shape.w * openmwseCloudMediumReferenceParameters().y
        * openmwseCloudTerrainInfluence(worldPosition.xy);
    return mix(1.0, response, clamp(influence, 0.0, 1.0));
}

/// Return an advected and independently phased position for one cloud stratum.
vec3 openmwseCloudLayerPosition(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    vec3 advectedPosition = openmwseCloudAdvectedPosition(worldPosition);
    advectedPosition.xy += layer.fieldOffsetWorldUnits;
    return advectedPosition;
}

/// Expand the configurable main shell only as convection matures while retaining the authored low-cloud thickness.
float openmwseCloudVerticalExtentScale(OpenMWSECloudLayer layer)
{
    if (layer.terrainCoupling > 0.0)
        return 1.0;
    float matureDevelopment = clamp(layer.shape.z * layer.organization.z, 0.0, 1.0);
    return mix(1.0, openmwseCloudMaximumThicknessScale, matureDevelopment);
}

/// Return the height-skewed coverage-map position for the main layer.
vec2 openmwseCloudColumnPosition(
    vec3 worldPosition, vec3 advectedPosition, OpenMWSECloudLayer layer)
{
    float nominalThickness = max(layer.parameters.w, 1.0) * openmwseCloudVerticalExtentScale(layer);
    float envelopeHeight = clamp((worldPosition.z - openmwseCloudNominalBaseHeight(worldPosition, layer))
            / nominalThickness,
        0.0, 1.0);
    float coverageSkewWorldUnits = 2500.0 * openmwseCloudWorldUnitsPerMeter;
    float formationScale = mix(openmwseCloudLowFormationScale, 1.0,
        1.0 - clamp(layer.terrainCoupling, 0.0, 1.0));
    vec2 coverageWindDirection = layer.terrainCoupling <= 0.0
        ? vec2(1.0, 0.0) : normalize(vec2(-0.85, 0.53));
    return advectedPosition.xy + coverageWindDirection * coverageSkewWorldUnits
        * formationScale * envelopeHeight;
}

/// Return the physically calibrated horizontal formation scale for one cloud stratum.
float openmwseCloudFormationScale(OpenMWSECloudLayer layer)
{
    return mix(openmwseCloudLowFormationScale, 1.0,
        1.0 - clamp(layer.terrainCoupling, 0.0, 1.0));
}

/// Evaluate one layer's weather field with the same physical scale used by its density samples.
OpenMWSECloudColumnProperties openmwseCloudLayerColumnProperties(
    vec3 worldPosition, vec3 advectedPosition, OpenMWSECloudLayer layer)
{
    return openmwseCloudColumnPropertiesForScale(
        openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters,
        layer.shape, layer.organization, layer.structure, openmwseCloudFormationScale(layer));
}

/// Return continuous small, medium, and large cloud-type weights from one weather column.
vec3 openmwseCloudTypeWeights(float cloudType)
{
    cloudType = clamp(cloudType, 0.0, 1.0);
    return vec3(1.0 - clamp(cloudType * 2.0, 0.0, 1.0),
        max(1.0 - abs(cloudType - 0.5) * 2.0, 0.0),
        clamp((cloudType - 0.5) * 2.0, 0.0, 1.0));
}

/// Return the black-white-white-black height gradient for one cloud column.
vec4 openmwseCloudTypeHeightGradient(float cloudType, float development)
{
    vec3 typeWeights = openmwseCloudTypeWeights(cloudType);
    vec4 smallGradient = vec4(0.01, 0.1, 0.11, 0.2);
    vec4 mediumGradient = vec4(0.01, 0.08, 0.3, 0.4);
    vec4 largeGradient = vec4(0.01, 0.06, 0.75, 0.95);
    return smallGradient * typeWeights.x + mediumGradient * typeWeights.y + largeGradient * typeWeights.z;
}

/// Evaluate the type-specific vertical density support used before coverage erosion.
float openmwseCloudTypeHeightDensity(float relativeHeight, float cloudType, float development)
{
    vec4 gradient = openmwseCloudTypeHeightGradient(cloudType, development);
    return smoothstep(gradient.x, gradient.y, relativeHeight)
        - smoothstep(gradient.z, gradient.w, relativeHeight);
}

/// Deform column support into a laterally spreading top without constructing a separate anvil volume.
float openmwseCloudAnvilDeformation(
    float relativeHeight, float cloudType, float development, float requestedStrength)
{
    vec3 typeWeights = openmwseCloudTypeWeights(cloudType);
    float activeStrength = clamp(requestedStrength * development, 0.0, 1.0);
    vec4 mediumDeformation = vec4(15.0, 0.1, 15.0, 0.1);
    vec4 largeDeformation = vec4(5.0, 0.25, 5.0, 0.15);
    vec4 deformation = mediumDeformation * typeWeights.y + largeDeformation * typeWeights.z;
    vec4 gradient = openmwseCloudTypeHeightGradient(cloudType, development);
    float top = pow(clamp(relativeHeight + deformation.y + (1.0 - gradient.w), 0.0, 1.0), deformation.x);
    float bottom = pow(clamp((1.0 - relativeHeight) + deformation.w + gradient.x, 0.0, 1.0), deformation.z);
    return mix(1.0, clamp(top + bottom, 0.0, 1.0), activeStrength);
}

/// Return conservative world-space bounds for one cloud stratum.
vec2 openmwseCloudLayerHeightBounds(OpenMWSECloudLayer layer)
{
    float fixedBaseHeight = openmwseCloudWaterHeight() + layer.parameters.z;
    float nominalThickness = max(layer.parameters.w, 1.0) * openmwseCloudVerticalExtentScale(layer);
    if (layer.terrainCoupling <= 0.0)
        return vec2(fixedBaseHeight, fixedBaseHeight + nominalThickness);

    vec4 mediumReference = openmwseCloudMediumReferenceParameters();
    float terrainWeight = layer.terrainCoupling * layer.shape.w * mediumReference.y;
    float terrainBaseHeight = mediumReference.x + mediumReference.z;
    float nominalBaseHeight = mix(fixedBaseHeight, terrainBaseHeight, clamp(terrainWeight, 0.0, 1.0));
    float terrainExcursion = length(openmwseCloudOrographicParameters().zw) * 20480.0
        * clamp(terrainWeight, 0.0, 1.0);
    float maximumBaseVariation = mix(0.08, 0.28, clamp(layer.structure.y, 0.0, 1.0));
    float layerThickness = nominalThickness * openmwseCloudMaximumThicknessScale;
    return vec2(nominalBaseHeight - nominalThickness * maximumBaseVariation - terrainExcursion,
        nominalBaseHeight + layerThickness + nominalThickness * maximumBaseVariation + terrainExcursion);
}

/// Derive low-frequency coordinates with height-dependent wind shear.
vec3 openmwseCloudBaseNoiseCoordinates(
    vec3 advectedPosition, OpenMWSECloudLayer layer, float relativeHeight)
{
    float featureScale = max(layer.shape.y, 0.25) * openmwseCloudFormationScale(layer);
    vec3 modelPosition = vec3(advectedPosition.x, advectedPosition.z, advectedPosition.y);
    if (layer.terrainCoupling > 0.0)
    {
        float densityCoordinateThickness
            = max(layer.parameters.w * openmwseCloudMaximumThicknessScale, 1.0);
        modelPosition.y = relativeHeight * densityCoordinateThickness;
    }
    vec3 shapeWindDirection = vec3(cos(0.75), -0.5, sin(0.75));
    modelPosition += shapeWindDirection * relativeHeight
        * (700.0 * openmwseCloudWorldUnitsPerMeter) * openmwseCloudFormationScale(layer);
    modelPosition.y += modelPosition.x / 3.0 + modelPosition.z / 7.0;
    return modelPosition * (openmwseCloudReferenceShapeNoiseScale / featureScale);
}

/// Derive high-frequency coordinates independently from the low-frequency domain warp.
vec3 openmwseCloudDetailNoiseCoordinates(vec3 advectedPosition, OpenMWSECloudLayer layer,
    float relativeHeight, float distortionAmount)
{
    float formationScale = openmwseCloudFormationScale(layer);
    float featureScale = max(layer.shape.y, 0.25) * formationScale;
    vec3 modelPosition = vec3(advectedPosition.x, advectedPosition.z, advectedPosition.y);
    if (layer.terrainCoupling > 0.0)
    {
        float densityCoordinateThickness
            = max(layer.parameters.w * openmwseCloudMaximumThicknessScale, 1.0);
        modelPosition.y = relativeHeight * densityCoordinateThickness;
    }
    vec3 shapeWindDirection = vec3(cos(0.75), -0.5, sin(0.75));
    modelPosition += shapeWindDirection * relativeHeight
        * (700.0 * openmwseCloudWorldUnitsPerMeter) * formationScale;
    vec2 curlCoordinates = advectedPosition.xy
        * (0.3 * openmwseCloudReferenceShapeNoiseScale / featureScale);
    vec3 curl = openmwseCloudCurlNoiseSample(curlCoordinates) * 2.0 - 1.0;
    float curlHeightWeight = pow(clamp(1.0 - relativeHeight, 0.0, 1.0), 5.0);
    modelPosition += vec3(curl.x, curl.z, curl.y)
        * (500.0 * openmwseCloudWorldUnitsPerMeter) * formationScale
        * curlHeightWeight * distortionAmount;
    return modelPosition * (openmwseCloudReferenceShapeNoiseScale / featureScale);
}

/// Return the locally varied base and conservative height of the coherent dimensional profile.
vec2 openmwseCloudLocalLayerFromProperties(
    vec3 worldPosition, OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    float nominalBaseHeight = openmwseCloudNominalBaseHeight(worldPosition, layer);
    float nominalThickness = max(layer.parameters.w, 1.0) * openmwseCloudVerticalExtentScale(layer);
    if (layer.terrainCoupling <= 0.0)
        return vec2(nominalBaseHeight, nominalThickness);

    float baseVariation = mix(0.08, 0.28, clamp(layer.structure.y, 0.0, 1.0));
    float localBaseHeight
        = nominalBaseHeight + (columnProperties.baseVariation * 2.0 - 1.0) * nominalThickness * baseVariation;
    return vec2(localBaseHeight, nominalThickness * openmwseCloudMaximumThicknessScale);
}

/// Return the locally varied cloud base height and thickness for a world-space column.
vec2 openmwseCloudLocalLayer(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    worldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    return openmwseCloudLocalLayerFromProperties(
        worldPosition, openmwseCloudLayerColumnProperties(worldPosition, advectedPosition, layer), layer);
}

/// Return the forward ray interval intersecting the conservative cloud-height bounds.
vec2 openmwseCloudLayerIntersection(vec3 origin, vec3 direction, float lowerHeight, float upperHeight)
{
    if (abs(direction.z) < 0.0001)
        return origin.z >= lowerHeight && origin.z <= upperHeight ? vec2(0.0, openmwseCloudMaximumDistance)
                                                                  : vec2(1.0, 0.0);

    float first = (lowerHeight - origin.z) / direction.z;
    float second = (upperHeight - origin.z) / direction.z;
    return vec2(max(0.0, min(first, second)), min(openmwseCloudMaximumDistance, max(first, second)));
}

/// Return the nearest forward interval through a virtual spherical main-cloud shell.
vec2 openmwseMainCloudLayerIntersection(
    vec3 origin, vec3 direction, float lowerHeight, float upperHeight)
{
    vec3 center = openmwseCloudVirtualPlanetCenter();
    vec3 relativeOrigin = origin - center;
    float lowerRadius = openmwseCloudVirtualPlanetRadius
        + lowerHeight - openmwseCloudWaterHeight();
    float upperRadius = openmwseCloudVirtualPlanetRadius
        + upperHeight - openmwseCloudWaterHeight();
    float projectedOrigin = dot(relativeOrigin, direction);
    float originRadiusSquared = dot(relativeOrigin, relativeOrigin);
    float upperDiscriminant
        = projectedOrigin * projectedOrigin - (originRadiusSquared - upperRadius * upperRadius);
    if (upperDiscriminant < 0.0)
        return vec2(1.0, 0.0);

    float upperRoot = sqrt(upperDiscriminant);
    float outerNear = -projectedOrigin - upperRoot;
    float outerFar = -projectedOrigin + upperRoot;
    if (outerFar <= 0.0)
        return vec2(1.0, 0.0);

    float originRadius = sqrt(originRadiusSquared);
    float innerDiscriminant
        = projectedOrigin * projectedOrigin - (originRadiusSquared - lowerRadius * lowerRadius);
    float intervalStart = max(outerNear, 0.0);
    float intervalEnd = outerFar;
    if (originRadius < lowerRadius)
    {
        if (innerDiscriminant < 0.0)
            return vec2(1.0, 0.0);
        intervalStart = max(-projectedOrigin + sqrt(innerDiscriminant), 0.0);
    }
    else if (innerDiscriminant >= 0.0)
    {
        float innerNear = -projectedOrigin - sqrt(innerDiscriminant);
        if (innerNear > intervalStart)
            intervalEnd = min(intervalEnd, innerNear);
    }
    return vec2(intervalStart, min(intervalEnd, openmwseCloudMaximumDistance));
}

/// Apply one type profile, coverage remap, and anvil deformation to base noise.
float openmwseCloudDensityComponent(float cloudNoise, float relativeHeight, float cloudType,
    float development, float localCoverage, float anvilStrength)
{
    if (localCoverage <= 0.001)
        return 0.0;
    float heightDensity = openmwseCloudTypeHeightDensity(relativeHeight, cloudType, development);
    float anvilDeformation = openmwseCloudAnvilDeformation(
        relativeHeight, cloudType, development, anvilStrength);
    float coverage = clamp(localCoverage, 0.0, 1.0);
    float shapedDensity = cloudNoise * heightDensity;
    shapedDensity = openmwseCloudRemapUnclamped(shapedDensity, 1.0 - coverage, 1.0) * coverage;
    shapedDensity *= anvilDeformation;
    return max(shapedDensity, 0.0);
}

/// Erode one independently sampled density component with height-dilated Worley detail.
float openmwseCloudDetailedDensityComponent(float coarseDensity, vec4 detailNoise,
    float relativeHeight, float detailAmount)
{
    if (coarseDensity <= 0.001 || detailAmount <= 0.01)
        return coarseDensity;
    float detailFbm = dot(detailNoise.rgb, vec3(0.625, 0.25, 0.125));
    float heightErosion = mix(1.0 - detailFbm, detailFbm,
        clamp(relativeHeight * 10.0, 0.0, 1.0));
    float detailedDensity = max(openmwseCloudRemapUnclamped(
        coarseDensity, heightErosion * 0.3, 1.0), 0.0);
    return mix(coarseDensity, detailedDensity, detailAmount);
}

/// Return whether one component can contain density before sampling either 3D noise volume.
bool openmwseCloudDensityComponentPotential(float relativeHeight, float cloudType,
    float development, float localCoverage, float anvilStrength)
{
    vec4 gradient = openmwseCloudTypeHeightGradient(cloudType, development);
    float deformedCoverage = localCoverage * openmwseCloudAnvilDeformation(
        relativeHeight, cloudType, development, anvilStrength);
    return deformedCoverage > 0.05 && relativeHeight > gradient.x && relativeHeight < gradient.w;
}

/// Return the weather presentation's strength for typed anvil deformation.
float openmwseCloudAnvilStrength(OpenMWSECloudLayer layer)
{
    return clamp(layer.shape.w, 0.0, 1.0);
}

/// Evaluate one coherent weather, type, and density field for a cloud stratum.
float openmwseCloudDensityFromNoise(
    vec3 worldPosition, float cloudNoise,
    OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    vec2 localLayer = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localLayer.x || worldPosition.z > localLayer.x + localLayer.y)
        return 0.0;
    float relativeHeight
        = clamp((worldPosition.z - localLayer.x) / max(localLayer.y, 1.0), 0.0, 1.0);
    float anvilStrength = openmwseCloudAnvilStrength(layer);
    float coverage = columnProperties.localCoverage;
    return openmwseCloudDensityComponent(cloudNoise, relativeHeight,
        columnProperties.family, columnProperties.development, coverage, anvilStrength)
        * max(layer.parameters.y, 0.0)
        * openmwseCloudOrographicDensityScale(worldPosition, layer);
}

/// Reject a cloud sample from weather and height data before paying for three-dimensional noise.
bool openmwseCloudPotentialDensity(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    worldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    if (worldPosition.z < layerHeightBounds.x || worldPosition.z > layerHeightBounds.y)
        return false;
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    OpenMWSECloudColumnProperties columnProperties
        = openmwseCloudLayerColumnProperties(worldPosition, advectedPosition, layer);
    vec2 localLayer = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localLayer.x || worldPosition.z > localLayer.x + localLayer.y)
        return false;
    float relativeHeight
        = clamp((worldPosition.z - localLayer.x) / max(localLayer.y, 1.0), 0.0, 1.0);
    float anvilStrength = openmwseCloudAnvilStrength(layer);
    float coverage = columnProperties.localCoverage;
    return openmwseCloudDensityComponentPotential(relativeHeight, columnProperties.family,
        columnProperties.development, coverage, anvilStrength);
}

/// Carry density and independently authored precipitation optics from one cloud sample.
struct OpenMWSECloudDensitySample
{
    float density;
    float precipitation;
};

/// Evaluate cloud density and precipitation with high-frequency billows filtered by sampling footprint.
OpenMWSECloudDensitySample openmwseCloudDensitySample(vec3 worldPosition, float distanceFromEye, float sampleFootprint,
    float lodBias, OpenMWSECloudLayer layer)
{
    worldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    if (worldPosition.z < layerHeightBounds.x || worldPosition.z > layerHeightBounds.y)
        return OpenMWSECloudDensitySample(0.0, 0.0);
    float distanceDetail = 1.0 - smoothstep(30.0 * 1000.0 * openmwseCloudWorldUnitsPerMeter,
        60.0 * 1000.0 * openmwseCloudWorldUnitsPerMeter, distanceFromEye);
    float footprintDetail = 1.0 - smoothstep(450.0, 2600.0, sampleFootprint);
    float billowDetailAmount = distanceDetail * footprintDetail;
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    OpenMWSECloudColumnProperties columnProperties
        = openmwseCloudLayerColumnProperties(worldPosition, advectedPosition, layer);
    if (columnProperties.localCoverage <= 0.001)
        return OpenMWSECloudDensitySample(0.0, 0.0);
    vec2 localHeightRange = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localHeightRange.x || worldPosition.z > localHeightRange.x + localHeightRange.y)
        return OpenMWSECloudDensitySample(0.0, 0.0);
    float relativeHeight
        = clamp((worldPosition.z - localHeightRange.x) / max(localHeightRange.y, 1.0), 0.0, 1.0);
    float anvilStrength = openmwseCloudAnvilStrength(layer);
    float coverage = columnProperties.localCoverage;
    bool densityPotential = openmwseCloudDensityComponentPotential(relativeHeight,
        columnProperties.family, columnProperties.development, coverage, anvilStrength);
    if (!densityPotential)
        return OpenMWSECloudDensitySample(0.0, 0.0);
    vec3 baseCoordinates
        = openmwseCloudBaseNoiseCoordinates(advectedPosition, layer, relativeHeight);
    vec4 baseNoise = openmwseCloudNoiseSample(baseCoordinates, lodBias);
    float density = openmwseCloudDensityComponent(openmwseCloudBaseFromNoise(baseNoise),
        relativeHeight, columnProperties.family, columnProperties.development,
        coverage, anvilStrength);
    if (billowDetailAmount > 0.01 && density > 0.001)
    {
        vec3 detailCoordinates = openmwseCloudDetailNoiseCoordinates(
            advectedPosition, layer, relativeHeight, billowDetailAmount) * 4.0;
        vec4 detailNoise = openmwseCloudDetailNoiseSample(detailCoordinates, lodBias);
        density = openmwseCloudDetailedDensityComponent(
            density, detailNoise, relativeHeight, billowDetailAmount);
    }
    density *= max(layer.parameters.y, 0.0)
        * openmwseCloudOrographicDensityScale(worldPosition, layer);
    return OpenMWSECloudDensitySample(density, columnProperties.precipitation);
}

/// Evaluate only cloud density for shadow, occupancy, and compatibility callers.
float openmwseCloudDensity(vec3 worldPosition, float distanceFromEye, float sampleFootprint,
    float lodBias, OpenMWSECloudLayer layer)
{
    return openmwseCloudDensitySample(
        worldPosition, distanceFromEye, sampleFootprint, lodBias, layer).density;
}

/// Evaluate cloud density from only the stable low-frequency field.
float openmwseCloudBroadDensity(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    worldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    if (worldPosition.z < layerHeightBounds.x || worldPosition.z > layerHeightBounds.y)
        return 0.0;
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    OpenMWSECloudColumnProperties columnProperties
        = openmwseCloudLayerColumnProperties(worldPosition, advectedPosition, layer);
    if (columnProperties.localCoverage <= 0.001)
        return 0.0;
    vec2 localHeightRange = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localHeightRange.x || worldPosition.z > localHeightRange.x + localHeightRange.y)
        return 0.0;
    float relativeHeight
        = clamp((worldPosition.z - localHeightRange.x) / max(localHeightRange.y, 1.0), 0.0, 1.0);
    vec3 baseCoordinates
        = openmwseCloudBaseNoiseCoordinates(advectedPosition, layer, relativeHeight);
    float cloudNoise = openmwseCloudBaseFromNoise(openmwseCloudNoiseSample(baseCoordinates, 0.0));
    return openmwseCloudDensityFromNoise(worldPosition, cloudNoise, columnProperties, layer);
}

/// Evaluate stable broad density from every enabled stratum for temporal classification.
float openmwseCloudBroadDensity(vec3 worldPosition)
{
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    float density = openmwseCloudLayerEnabled(mainLayer)
        ? openmwseCloudBroadDensity(worldPosition, mainLayer) : 0.0;
    if (openmwseCloudLayerEnabled(lowLayer))
        density += openmwseCloudBroadDensity(worldPosition, lowLayer);
    return density;
}

/// Evaluate stable low-frequency occupancy for the cloud strata active in one integration.
float openmwseCloudBroadDensityForLayers(vec3 worldPosition, bool mainLayerEnabled,
    bool lowLayerEnabled, OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer lowLayer)
{
    float density = 0.0;
    if (mainLayerEnabled)
        density += openmwseCloudBroadDensity(worldPosition, mainLayer);
    if (lowLayerEnabled)
        density += openmwseCloudBroadDensity(worldPosition, lowLayer);
    return density;
}

/// Evaluate a normalized Henyey-Greenstein phase lobe for cloud droplets.
float openmwseCloudMiePhase(float cosineAngle, float anisotropy)
{
    float denominator = 1.0 + anisotropy * anisotropy - 2.0 * anisotropy * cosineAngle;
    return (1.0 - anisotropy * anisotropy) / max(12.5663706 * pow(denominator, 1.5), 0.001);
}

/// Evaluate the forward/backward dual-lobe droplet phase function.
float openmwseCloudDualLobePhase(float cosineAngle)
{
    return mix(openmwseCloudMiePhase(cosineAngle, 0.5),
        openmwseCloudMiePhase(cosineAngle, -0.5), 0.2);
}

/// Integrate one stratum's optical depth toward the sun with nonlinear full-density samples.
float openmwseCloudSunOpticalDepth(vec3 samplePosition, vec3 sunDirection, int quality,
    float localDensity, OpenMWSECloudLayer layer)
{
    if (!openmwseCloudLayerEnabled(layer))
        return 0.0;
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    vec2 lightInterval = layer.terrainCoupling > 0.0
        ? openmwseCloudLayerIntersection(
            samplePosition, sunDirection, layerHeightBounds.x, layerHeightBounds.y)
        : openmwseMainCloudLayerIntersection(
            samplePosition, sunDirection, layerHeightBounds.x, layerHeightBounds.y);
    if (lightInterval.y <= lightInterval.x)
        return 0.0;
    float lightPathLength = min(lightInterval.y - lightInterval.x,
        3000.0 * openmwseCloudWorldUnitsPerMeter);
    if (lightPathLength <= 1.0)
        return 0.0;

    int shadowSampleCount = quality >= 2 ? 5 : quality >= 1 ? 4 : 3;
    float densityIntegral = 0.0;
    const int maximumShadowSampleCount = 5;
    for (int sampleIndex = 0; sampleIndex < maximumShadowSampleCount; ++sampleIndex)
    {
        if (sampleIndex >= shadowSampleCount)
            break;
        float intervalStart = float(sampleIndex) / float(shadowSampleCount);
        float intervalEnd = float(sampleIndex + 1) / float(shadowSampleCount);
        intervalStart *= intervalStart;
        intervalEnd *= intervalEnd;
        float intervalFraction = intervalEnd - intervalStart;
        float sampleFraction = intervalStart + intervalFraction * 0.5;
        float sampleDistance = lightInterval.x + lightPathLength * sampleFraction;
        vec3 shadowPosition = samplePosition + sunDirection * sampleDistance;
        float distanceFromEye = length(shadowPosition - openmwseCloudEyePosition());
        float shadowFootprint = max(700.0, distanceFromEye * 0.0018 + sampleDistance * 0.006);
        float shadowLodBias = 0.5 + float(sampleIndex) * 0.5;
        float shadowDensity = openmwseCloudDensity(
            shadowPosition, distanceFromEye, shadowFootprint, shadowLodBias, layer);
        densityIntegral += shadowDensity * lightPathLength * intervalFraction;
    }
    return densityIntegral * (0.086 / openmwseCloudWorldUnitsPerMeter);
}

/// Approximate direct-sun access through both cloud strata without duplicating transport policy.
float openmwseCloudSunAccess(vec3 samplePosition, vec3 sunDirection, int quality,
    float mainDensity, float lowDensity,
    OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer lowLayer)
{
    float opticalDepth = openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, mainDensity, mainLayer);
    opticalDepth += openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, lowDensity, lowLayer);
    return exp(-opticalDepth);
}

/// Apply the canonical bounded aerial perspective to one cloud stratum's accumulated radiance.
vec3 openmwseCloudAtmosphericContribution(vec3 scatteredLight, float opacity,
    float representativeDistanceSum, vec3 atmosphericTarget, float opticalDistanceScale,
    float opticalDistanceLimit,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    if (opacity <= 1e-5)
        return vec3(0.0);

    float representativeDistance = representativeDistanceSum / opacity;
    vec3 meanRadiance = scatteredLight / opacity;
    vec3 atmosphericRadiance = openmwseApplyAerialPerspectiveTowardTargetWithScale(meanRadiance,
        atmosphericTarget, representativeDistance, opticalDistanceScale, opticalDistanceLimit, atmosphere);
    return atmosphericRadiance * opacity;
}

/// Integrate premultiplied cloud radiance and transmittance along a bounded world-space ray.
vec4 openmwseIntegrateCloudsInternal(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere, float rayDither, bool quadratureSampling,
    bool useAtmosphericTarget)
{
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    bool mainLayerEnabled = openmwseCloudLayerEnabled(mainLayer);
    bool lowLayerEnabled = openmwseCloudLayerEnabled(lowLayer);
    if (!mainLayerEnabled && !lowLayerEnabled)
        return vec4(0.0, 0.0, 0.0, 1.0);

    vec2 mainInterval = vec2(1.0, 0.0);
    vec2 lowInterval = vec2(1.0, 0.0);
    bool cameraAboveEnabledLayer = false;
    if (mainLayerEnabled)
    {
        vec2 mainHeightBounds = openmwseCloudLayerHeightBounds(mainLayer);
        cameraAboveEnabledLayer
            = openmwseCloudVirtualAltitude(rayOrigin) > mainHeightBounds.y;
        mainInterval = openmwseMainCloudLayerIntersection(
            rayOrigin, worldDirection, mainHeightBounds.x, mainHeightBounds.y);
        mainInterval.y = min(mainInterval.y, maximumDistance);
    }
    if (lowLayerEnabled)
    {
        vec2 lowHeightBounds = openmwseCloudLayerHeightBounds(lowLayer);
        cameraAboveEnabledLayer
            = cameraAboveEnabledLayer || rayOrigin.z > lowHeightBounds.y;
        lowInterval = openmwseCloudLayerIntersection(
            rayOrigin, worldDirection, lowHeightBounds.x, lowHeightBounds.y);
        lowInterval.y = min(lowInterval.y, maximumDistance);
    }
    bool mainIntervalValid = mainInterval.y > mainInterval.x;
    bool lowIntervalValid = lowInterval.y > lowInterval.x;
    if (!mainIntervalValid && !lowIntervalValid)
        return vec4(0.0, 0.0, 0.0, 1.0);

    vec2 firstInterval = mainIntervalValid ? mainInterval : lowInterval;
    vec2 secondInterval = vec2(1.0, 0.0);
    bool separatedIntervals = false;
    if (mainIntervalValid && lowIntervalValid)
    {
        firstInterval = mainInterval.x <= lowInterval.x ? mainInterval : lowInterval;
        secondInterval = mainInterval.x <= lowInterval.x ? lowInterval : mainInterval;
        if (secondInterval.x <= firstInterval.y)
            firstInterval.y = max(firstInterval.y, secondInterval.y);
        else
            separatedIntervals = true;
    }

    const int maximumSampleCount = 192;
    int maximumAllocatedSamples = 48 + quality * 16;
    if (cameraAboveEnabledLayer)
        maximumAllocatedSamples *= 2;
    if (maximumAllocatedSamples > maximumSampleCount)
        maximumAllocatedSamples = maximumSampleCount;
    float firstIntervalLength = firstInterval.y - firstInterval.x;
    float secondIntervalLength = separatedIntervals ? secondInterval.y - secondInterval.x : 0.0;
    float traversedIntervalLength = firstIntervalLength + secondIntervalLength;
    float referenceThickness = 1.0;
    if (mainLayerEnabled)
        referenceThickness = max(referenceThickness, mainLayer.parameters.w);
    if (lowLayerEnabled)
        referenceThickness = max(referenceThickness, lowLayer.parameters.w);
    float stepAllocation = clamp(traversedIntervalLength / (referenceThickness * 3.0), 0.15, 1.0);
    int sampleCount = int(ceil(float(maximumAllocatedSamples) * stepAllocation));
    if (sampleCount < 8)
        sampleCount = 8;
    float fineStepLength = traversedIntervalLength / float(sampleCount);
    float distanceAlongCloudIntervals = 0.0;
    float transmittance = 1.0;
    vec3 mainScatteredLight = vec3(0.0);
    vec3 lowScatteredLight = vec3(0.0);
    float mainOpacity = 0.0;
    float lowOpacity = 0.0;
    float mainRepresentativeDistanceSum = 0.0;
    float lowRepresentativeDistanceSum = 0.0;
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    float primaryPhase = openmwseCloudDualLobePhase(dot(worldDirection, sunDirection));
    float secondaryPhase = mix(0.07957747, primaryPhase, 0.2);
    OpenMWSEAerialPerspectiveParameters cloudTopAtmosphere = atmosphere;
    cloudTopAtmosphere.observerHeight = atmosphere.weatherCloudTopHeight + 9000.0;
    vec3 clearAirSolarRadiance
        = openmwseClearAirSolarRadiance(solarRadiance, cloudTopAtmosphere);
    vec3 clearAirSkyIrradiance
        = openmwseClearAirSkyIrradiance(skyIrradiance, cloudTopAtmosphere);
    vec3 incidentSolarRadiance
        = openmwseIncidentSolarRadiance(clearAirSolarRadiance, cloudTopAtmosphere);
    vec3 viewSkyRadiance = openmwseAtmosphereSkyRadiance(worldDirection, cloudTopAtmosphere);
    vec3 environmentIrradiance
        = openmwseAtmosphereEnvironmentIrradiance(clearAirSkyIrradiance, cloudTopAtmosphere);
    environmentIrradiance += incidentSolarRadiance * 0.06;

    for (int sampleIndex = 0; sampleIndex < maximumSampleCount; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;
        if (distanceAlongCloudIntervals >= traversedIntervalLength)
            break;
        bool sampleSecondInterval = separatedIntervals && distanceAlongCloudIntervals >= firstIntervalLength;
        float intervalPathOffset
            = sampleSecondInterval ? distanceAlongCloudIntervals - firstIntervalLength : distanceAlongCloudIntervals;
        vec2 activeInterval = sampleSecondInterval ? secondInterval : firstInterval;
        float activeIntervalLength = sampleSecondInterval ? secondIntervalLength : firstIntervalLength;
        float remainingIntervalLength = activeIntervalLength - intervalPathOffset;
        if (remainingIntervalLength <= 0.0)
        {
            distanceAlongCloudIntervals = firstIntervalLength;
            continue;
        }
        float segmentLength = min(fineStepLength, remainingIntervalLength);
        float distanceAlongRay = activeInterval.x + intervalPathOffset;
        float sampleOffset = quadratureSampling ? 0.5 : rayDither;
        float sampleDistance = distanceAlongRay + segmentLength * sampleOffset;
        vec3 samplePosition = rayOrigin + worldDirection * sampleDistance;
        if (!quadratureSampling)
        {
            bool potentialDensity = mainLayerEnabled
                && openmwseCloudPotentialDensity(samplePosition, mainLayer);
            potentialDensity = potentialDensity || (lowLayerEnabled
                && openmwseCloudPotentialDensity(samplePosition, lowLayer));
            if (!potentialDensity)
            {
                float skipLength = min(segmentLength * 2.0, remainingIntervalLength);
                distanceAlongCloudIntervals += skipLength;
                if (skipLength > segmentLength * 1.5)
                    ++sampleIndex;
                continue;
            }
        }
        float mainDensity = 0.0;
        float lowDensity = 0.0;
        float mainPrecipitation = 0.0;
        float lowPrecipitation = 0.0;
        if (quadratureSampling)
        {
            const float firstQuadratureOffset = 0.2113248654;
            const float secondQuadratureOffset = 0.7886751346;
            float firstDistance = distanceAlongRay + segmentLength * firstQuadratureOffset;
            float secondDistance = distanceAlongRay + segmentLength * secondQuadratureOffset;
            if (mainLayerEnabled)
            {
                float firstFootprint = max(250.0, firstDistance * 0.0012);
                float secondFootprint = max(250.0, secondDistance * 0.0012);
                OpenMWSECloudDensitySample firstSample = openmwseCloudDensitySample(
                    rayOrigin + worldDirection * firstDistance, firstDistance, firstFootprint, 0.0, mainLayer);
                OpenMWSECloudDensitySample secondSample = openmwseCloudDensitySample(
                    rayOrigin + worldDirection * secondDistance, secondDistance, secondFootprint, 0.0, mainLayer);
                mainDensity = 0.5 * (firstSample.density + secondSample.density);
                mainPrecipitation = (firstSample.density * firstSample.precipitation
                    + secondSample.density * secondSample.precipitation)
                    / max(firstSample.density + secondSample.density, 0.0001);
            }
            if (lowLayerEnabled)
            {
                float firstFootprint = max(250.0, firstDistance * 0.0012);
                float secondFootprint = max(250.0, secondDistance * 0.0012);
                OpenMWSECloudDensitySample firstSample = openmwseCloudDensitySample(
                    rayOrigin + worldDirection * firstDistance, firstDistance, firstFootprint, 0.0, lowLayer);
                OpenMWSECloudDensitySample secondSample = openmwseCloudDensitySample(
                    rayOrigin + worldDirection * secondDistance, secondDistance, secondFootprint, 0.0, lowLayer);
                lowDensity = 0.5 * (firstSample.density + secondSample.density);
                lowPrecipitation = (firstSample.density * firstSample.precipitation
                    + secondSample.density * secondSample.precipitation)
                    / max(firstSample.density + secondSample.density, 0.0001);
            }
        }
        else
        {
            float rayFootprint = max(250.0, sampleDistance * 0.0012);
            if (mainLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudDensitySample(
                    samplePosition, sampleDistance, rayFootprint, 0.0, mainLayer);
                mainDensity = sample.density;
                mainPrecipitation = sample.precipitation;
            }
            if (lowLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudDensitySample(
                    samplePosition, sampleDistance, rayFootprint, 0.0, lowLayer);
                lowDensity = sample.density;
                lowPrecipitation = sample.precipitation;
            }
        }
        float density = mainDensity + lowDensity;
        if (density > 0.001)
        {
            float relativeHeight = 0.0;
            if (mainDensity > 0.001)
            {
                vec2 mainHeightRange = openmwseCloudLocalLayer(samplePosition, mainLayer);
                float mainFieldHeight = openmwseCloudVirtualAltitude(samplePosition);
                relativeHeight += mainDensity
                    * clamp((mainFieldHeight - mainHeightRange.x) / mainHeightRange.y, 0.0, 1.0);
            }
            if (lowDensity > 0.001)
            {
                vec2 lowHeightRange = openmwseCloudLocalLayer(samplePosition, lowLayer);
                relativeHeight += lowDensity
                    * clamp((samplePosition.z - lowHeightRange.x) / lowHeightRange.y, 0.0, 1.0);
            }
            relativeHeight /= density;
            float sunAccess = openmwseCloudSunAccess(
                samplePosition, sunDirection, quality,
                mainDensity, lowDensity, mainLayer, lowLayer);
            float extinctionCoefficient
                = density * (0.086 / openmwseCloudWorldUnitsPerMeter);
            float extinction = extinctionCoefficient * segmentLength;
            float sampleOpacity = 1.0 - exp(-extinction);
            float precipitation = (mainDensity * mainPrecipitation + lowDensity * lowPrecipitation)
                / max(density, 0.0001);
            float cloudAlbedo = mix(1.0, 0.58, clamp(precipitation, 0.0, 1.0));
            float topEnvironmentAccess = clamp(0.75 + relativeHeight, 0.0, 1.0);
            // Use height-weighted skylight instead of replacing the cloud underside with the
            // brighter ground-irradiance term. Retain a bounded canonical ground bounce near the base.
            vec3 environmentLight = environmentIrradiance * topEnvironmentAccess;
            environmentLight += groundIrradiance * (1.0 - topEnvironmentAccess) * 0.25;
            vec3 primaryLight
                = environmentLight + incidentSolarRadiance * (sunAccess * primaryPhase);

            // The second bounded scattering order retains the first-order scattering coefficient,
            // reduces extinction by 0.1, and broadens the phase toward isotropic response.
            float secondarySunAccess = pow(max(sunAccess, 0.0001), 0.1);
            vec3 secondaryLight = incidentSolarRadiance
                * (secondarySunAccess * secondaryPhase);
            // OpenMW's half-resolution pass traverses a longer physical segment than a full-resolution checkerboard
            // pass. Keep the second order bounded to the primary segment integral so one unresolved density
            // sample cannot amplify radiance into visible depth contours. Its broadened phase and reduced
            // solar optical depth retain the intended multiple-scattering fill.
            const float secondaryIntegralScale = 1.0;
            vec3 sampleLight = cloudAlbedo
                * (primaryLight + secondaryLight * secondaryIntegralScale);
            float contribution = transmittance * sampleOpacity;
            float mainContribution = contribution * mainDensity / density;
            float lowContribution = contribution * lowDensity / density;
            mainScatteredLight += mainContribution * sampleLight;
            lowScatteredLight += lowContribution * sampleLight;
            mainOpacity += mainContribution;
            lowOpacity += lowContribution;
            mainRepresentativeDistanceSum += mainContribution * sampleDistance;
            lowRepresentativeDistanceSum += lowContribution * sampleDistance;
            transmittance *= 1.0 - sampleOpacity;
            if (transmittance < 0.015)
                break;
        }
        distanceAlongCloudIntervals += segmentLength;
    }

    vec3 cloudAtmosphericTarget = useAtmosphericTarget ? atmosphericTarget : viewSkyRadiance;
    const float mainCloudOpticalDistanceScale = 1.0;
    vec2 mainCloudOpticalLimits = useAtmosphericTarget ? vec2(0.72, 1.27296568) : vec2(0.88, 2.12026354);
    float mainCloudOpticalDistanceLimit
        = mix(mainCloudOpticalLimits.x, mainCloudOpticalLimits.y, step(0.5, atmosphere.exponentialFog));
    vec3 scatteredLight = openmwseCloudAtmosphericContribution(mainScatteredLight, mainOpacity,
        mainRepresentativeDistanceSum, cloudAtmosphericTarget, mainCloudOpticalDistanceScale,
        mainCloudOpticalDistanceLimit, atmosphere);
    scatteredLight += openmwseCloudAtmosphericContribution(lowScatteredLight, lowOpacity,
        lowRepresentativeDistanceSum, cloudAtmosphericTarget, 1.0, 1e20, atmosphere);
    return vec4(scatteredLight, transmittance);
}

/// Integrate clouds with one deterministic or temporal sample per segment, or deterministic quadrature when requested.
vec4 openmwseIntegrateClouds(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance,
    OpenMWSEAerialPerspectiveParameters atmosphere, float rayDither, bool quadratureSampling)
{
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, vec3(0.0), atmosphere, rayDither, quadratureSampling, false);
}

/// Integrate deterministic reflected clouds without frame-dependent sampling noise.
vec4 openmwseIntegrateCloudsStable(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, atmosphericTarget, atmosphere, 0.5, true, true);
}

#endif
