#ifndef OPENMWSE_ATMOSPHERE_CLOUDS_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUDS_GLSL

#include "cloud_field.glsl"

/// Describe one independently placeable cloud stratum consumed by the shared density and transport path.
struct OpenMWSECloudLayer
{
    vec4 parameters;
    vec4 shape;
    vec2 fieldOffsetWorldUnits;
    float terrainCoupling;
};

/// Return the main weather-cloud stratum.
OpenMWSECloudLayer openmwseMainCloudLayer()
{
    return OpenMWSECloudLayer(openmwseCloudParameters(), openmwseCloudShapeParameters(), vec2(0.0), 0.0);
}

/// Return the low environmental-cloud stratum with an independent deterministic field phase.
OpenMWSECloudLayer openmwseLowCloudLayer()
{
    return OpenMWSECloudLayer(
        openmwseLowCloudParameters(), openmwseLowCloudShapeParameters(), vec2(137000.0, -91000.0), 1.0);
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
    float response = 1.0 + windwardFormation * 0.46 - leewardDissipation * 0.38;
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

/// Return the mildly height-skewed position used only by the broad cloud-organization field.
vec2 openmwseCloudColumnPosition(
    vec3 worldPosition, vec3 advectedPosition, OpenMWSECloudLayer layer)
{
    float nominalThickness = max(layer.parameters.w, 1.0);
    float envelopeHeight = clamp((worldPosition.z - openmwseCloudNominalBaseHeight(worldPosition, layer))
            / (nominalThickness * openmwseCloudMaximumThicknessScale),
        0.0, 1.0);
    float cloudFamily = clamp(layer.shape.x, 0.0, 1.0);
    float development = clamp(layer.shape.z, 0.0, 1.0);
    float coverageSkew = mix(0.03, 0.55, smoothstep(0.32, 0.94, cloudFamily)) * development;
    vec2 coverageWindDirection = normalize(vec2(-0.85, 0.53));
    return advectedPosition.xy
        - coverageWindDirection * nominalThickness * envelopeHeight * coverageSkew;
}

/// Return the cloud-family vertical profile for one normalized local-envelope height.
float openmwseCloudHeightProfile(float relativeHeight, float cloudFamily, float development)
{
    float stratus = smoothstep(0.01, 0.08, relativeHeight)
        * (1.0 - smoothstep(mix(0.2, 0.28, development), mix(0.3, 0.4, development), relativeHeight));
    float stratocumulus = smoothstep(0.01, 0.08, relativeHeight)
        * (1.0 - smoothstep(mix(0.34, 0.43, development), mix(0.48, 0.58, development), relativeHeight));
    float cumulus = smoothstep(0.01, 0.07, relativeHeight)
        * (1.0 - smoothstep(mix(0.5, 0.66, development), mix(0.76, 0.9, development), relativeHeight));
    float storm = smoothstep(0.01, 0.055, relativeHeight)
        * (1.0 - smoothstep(mix(0.7, 0.84, development), mix(0.92, 0.99, development), relativeHeight));
    float profile = mix(stratus, stratocumulus, smoothstep(0.06, 0.36, cloudFamily));
    profile = mix(profile, cumulus, smoothstep(0.32, 0.7, cloudFamily));
    return mix(profile, storm, smoothstep(0.68, 0.94, cloudFamily));
}

/// Return the requested family's maximum local-envelope thickness scale.
float openmwseCloudEnvelopeThicknessScale(float cloudFamily, float development)
{
    float stratusEnvelope = mix(0.58, 0.92, development);
    float stratocumulusEnvelope = mix(0.72, 1.1, development);
    float cumulusEnvelope = mix(0.82, 1.35, pow(development, 0.72));
    float stormEnvelope = mix(0.92, openmwseCloudMaximumThicknessScale, pow(development, 0.68));
    float envelopeThickness
        = mix(stratusEnvelope, stratocumulusEnvelope, smoothstep(0.06, 0.36, cloudFamily));
    envelopeThickness = mix(envelopeThickness, cumulusEnvelope, smoothstep(0.32, 0.7, cloudFamily));
    return mix(envelopeThickness, stormEnvelope, smoothstep(0.68, 0.94, cloudFamily));
}

/// Return the normalized height above which the requested family has no profile support.
float openmwseCloudHeightProfileUpperBound(float cloudFamily, float development)
{
    float stratusTop = mix(0.3, 0.4, development);
    float stratocumulusTop = mix(0.48, 0.58, development);
    float cumulusTop = mix(0.76, 0.9, development);
    float stormTop = mix(0.92, 0.99, development);
    float profileTop = mix(stratusTop, stratocumulusTop, smoothstep(0.06, 0.36, cloudFamily));
    profileTop = mix(profileTop, cumulusTop, smoothstep(0.32, 0.7, cloudFamily));
    return mix(profileTop, stormTop, smoothstep(0.68, 0.94, cloudFamily));
}

/// Return conservative world-space bounds specialized to one requested cloud family.
vec2 openmwseCloudLayerHeightBounds(OpenMWSECloudLayer layer)
{
    float fixedBaseHeight = openmwseCloudWaterHeight() + layer.parameters.z;
    vec4 mediumReference = openmwseCloudMediumReferenceParameters();
    float terrainWeight = layer.terrainCoupling * layer.shape.w * mediumReference.y;
    float terrainBaseHeight = mediumReference.x + mediumReference.z;
    float nominalBaseHeight = mix(fixedBaseHeight, terrainBaseHeight, clamp(terrainWeight, 0.0, 1.0));
    float terrainExcursion = length(openmwseCloudOrographicParameters().zw) * 20480.0
        * clamp(terrainWeight, 0.0, 1.0);
    float nominalThickness = max(layer.parameters.w, 1.0);
    float requestedFamily = clamp(layer.shape.x, 0.0, 1.0);
    float requestedDevelopment = clamp(layer.shape.z, 0.0, 1.0);
    float conservativeFamily = clamp(requestedFamily + 0.1, 0.0, 1.0);
    float envelopeScale
        = openmwseCloudEnvelopeThicknessScale(conservativeFamily, requestedDevelopment);
    float profileTop
        = openmwseCloudHeightProfileUpperBound(conservativeFamily, requestedDevelopment);
    float anvilTop = layer.shape.w > 0.001 && requestedFamily > 0.68 ? 0.95 : 0.0;
    float maximumProfileHeight = max(profileTop, anvilTop);
    const float maximumBaseVariation = 0.28;
    return vec2(nominalBaseHeight - nominalThickness * maximumBaseVariation - terrainExcursion,
        nominalBaseHeight + nominalThickness
            * (maximumBaseVariation + envelopeScale * maximumProfileHeight) + terrainExcursion);
}

/// Derive stable type-aware base-noise coordinates with cheap low-frequency curl and wind shear.
vec3 openmwseCloudBaseNoiseCoordinates(
    vec3 advectedPosition, OpenMWSECloudColumnProperties columnProperties, vec4 shape,
    float relativeHeight, float distortionAmount)
{
    float featureScale = max(shape.y, 0.25);
    float cloudFamily = mix(columnProperties.family, shape.x, smoothstep(0.78, 1.0, shape.x));
    float structureScale
        = max(mix(featureScale, featureScale * 0.58, smoothstep(0.68, 0.94, cloudFamily)), 0.65);
    float horizontalScale = mix(0.0000135, 0.0000195, cloudFamily) / structureScale;
    float verticalScale = mix(0.000024, 0.0000185, cloudFamily) / structureScale;
    vec3 decorrelatedPosition = advectedPosition;
    decorrelatedPosition.z += advectedPosition.x * 0.31 + advectedPosition.y * 0.17;
    vec3 noiseCoordinates = decorrelatedPosition * vec3(horizontalScale, horizontalScale, verticalScale);
    vec2 curlCoordinates = advectedPosition.xy * (0.000018 / structureScale);
    vec2 curl = vec2(openmwseCloudValueNoise(curlCoordinates + vec2(17.3, -9.1)),
                    openmwseCloudValueNoise(curlCoordinates.yx + vec2(-4.7, 23.9)))
        - 0.5;
    noiseCoordinates.xy += curl * distortionAmount * mix(0.12, 0.3, cloudFamily);
    noiseCoordinates.z += dot(curl, vec2(0.57, -0.43)) * distortionAmount
        * mix(0.08, 0.2, cloudFamily) * columnProperties.development;
    noiseCoordinates.xy += vec2(-0.24, 0.15) * relativeHeight * shape.w * columnProperties.development;
    return noiseCoordinates;
}

/// Erode reconstructed base density with packed medium-scale cells without creating independent Worley mass.
float openmwseCloudTypedBaseNoise(vec4 noiseSample, float cloudFamily)
{
    float reconstructedNoise = openmwseCloudBaseFromNoise(noiseSample);
    float worleyFbm = dot(noiseSample.gba, vec3(0.625, 0.25, 0.125));
    float erosionStrength = mix(0.06, 0.32, smoothstep(0.2, 0.85, cloudFamily));
    float cumulusCarving = smoothstep(0.3, 0.62, cloudFamily)
        * (1.0 - smoothstep(0.84, 0.98, cloudFamily));
    erosionStrength += cumulusCarving * 0.12;
    return openmwseCloudRemap(reconstructedNoise, (1.0 - worleyFbm) * erosionStrength, 1.0);
}

/// Return family, deck, convective-body, and outflow thickness scales for one weather column.
vec4 openmwseCloudComponentThicknessScales(
    OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    float requestedDevelopment = clamp(layer.shape.z, 0.0, 1.0);
    float familyEnvelopeThickness = openmwseCloudEnvelopeThicknessScale(
        columnProperties.family, columnProperties.development);
    float deckFamily = mix(0.12, 0.26, columnProperties.baseVariation);
    float deckDevelopment = requestedDevelopment * mix(0.2, 0.48,
        smoothstep(0.3, 0.9, columnProperties.convectivePotential));
    float deckEnvelopeThickness
        = openmwseCloudEnvelopeThicknessScale(deckFamily, deckDevelopment);
    float developedEnvelopeThickness = openmwseCloudEnvelopeThicknessScale(1.0,
        max(columnProperties.development,
            max(columnProperties.stormCellWeight, columnProperties.stormOutflowWeight)));
    float bodyEnvelopeThickness = mix(deckEnvelopeThickness, developedEnvelopeThickness,
        pow(columnProperties.stormCellWeight, 0.78));
    float outflowEnvelopeThickness = mix(deckEnvelopeThickness, developedEnvelopeThickness,
        pow(columnProperties.stormOutflowWeight, 0.68));
    return vec4(familyEnvelopeThickness, deckEnvelopeThickness,
        bodyEnvelopeThickness, outflowEnvelopeThickness);
}

/// Return the locally varied cloud base height and conservative support thickness.
vec2 openmwseCloudLocalLayerFromProperties(
    vec3 worldPosition, OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    float nominalBaseHeight = openmwseCloudNominalBaseHeight(worldPosition, layer);
    float nominalThickness = max(layer.parameters.w, 1.0);
    float requestedFamily = clamp(layer.shape.x, 0.0, 1.0);
    float baseVariation = mix(0.28, 0.1, requestedFamily);
    float localBaseHeight
        = nominalBaseHeight + (columnProperties.baseVariation * 2.0 - 1.0) * nominalThickness * baseVariation;
    vec4 thicknessScales = openmwseCloudComponentThicknessScales(columnProperties, layer);
    float stormWeight = smoothstep(0.78, 0.98, requestedFamily);
    float stormEnvelopeThickness = max(thicknessScales.z, thicknessScales.w);
    float envelopeThickness = mix(thicknessScales.x, stormEnvelopeThickness, stormWeight);
    return vec2(localBaseHeight, nominalThickness * envelopeThickness);
}

/// Return the locally varied cloud base height and thickness for a world-space column.
vec2 openmwseCloudLocalLayer(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    return openmwseCloudLocalLayerFromProperties(
        worldPosition,
        openmwseCloudColumnProperties(
            openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters, layer.shape),
        layer);
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

/// Apply the typed dimensional profile and canonical coverage erosion to reconstructed base noise.
float openmwseCloudDensityFromNoise(
    vec3 worldPosition, vec3 advectedPosition, float cloudNoise,
    OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    vec2 localLayer = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localLayer.x || worldPosition.z > localLayer.x + localLayer.y)
        return 0.0;
    float heightAboveBase = worldPosition.z - localLayer.x;
    float nominalThickness = max(layer.parameters.w, 1.0);
    vec4 componentThickness = nominalThickness
        * openmwseCloudComponentThicknessScales(columnProperties, layer);
    vec4 componentRelativeHeight = clamp(heightAboveBase / componentThickness, 0.0, 1.0);
    float relativeHeight = clamp(heightAboveBase / localLayer.y, 0.0, 1.0);
    float familyRelativeHeight = componentRelativeHeight.x;
    float deckRelativeHeight = componentRelativeHeight.y;
    float bodyRelativeHeight = componentRelativeHeight.z;
    float outflowRelativeHeight = componentRelativeHeight.w;
    float cloudFamily = columnProperties.family;
    float development = columnProperties.development;
    float localCoverage = columnProperties.localCoverage;
    if (max(localCoverage, max(columnProperties.stormSystemWeight,
            columnProperties.stormOutflowWeight)) <= 0.001)
        return 0.0;
    float stormWeight = smoothstep(0.78, 0.98, layer.shape.x);
    float upperOutflow = smoothstep(0.68, 0.78, outflowRelativeHeight)
        * (1.0 - smoothstep(0.92, 0.98, outflowRelativeHeight));
    float anvilStrength = clamp(layer.shape.w, 0.0, 1.0);
    float outflowSupport = stormWeight * anvilStrength * columnProperties.stormOutflowWeight
        * smoothstep(0.2, 0.72, columnProperties.organizationPotential)
        * smoothstep(0.18, 0.78, columnProperties.convectivePotential);
    float profileFamily = clamp(cloudFamily + upperOutflow * outflowSupport * 0.16, 0.0, 1.0);
    float familyHeightProfile
        = openmwseCloudHeightProfile(familyRelativeHeight, profileFamily, development);
    float deckFamily = mix(0.14, 0.26, columnProperties.baseVariation);
    float deckDevelopment = min(development, 0.48);
    float deckHeightProfile = openmwseCloudHeightProfile(deckRelativeHeight, deckFamily, deckDevelopment);
    float broadStormBodyWeight
        = max(columnProperties.stormCellWeight, columnProperties.stormOutflowWeight * 0.28);
    float stormBodyWeight = mix(broadStormBodyWeight, columnProperties.stormCellWeight,
        smoothstep(0.18, 0.52, bodyRelativeHeight));
    float towerTaperExponent = mix(0.72, 2.4,
        smoothstep(0.36, 0.9, bodyRelativeHeight));
    stormBodyWeight = pow(max(stormBodyWeight, 0.0), towerTaperExponent);
    float towerDevelopment = max(development, stormBodyWeight * 0.72);
    float developedCell = stormWeight
        * columnProperties.stormCellWeight
        * smoothstep(0.32, 0.9, development);
    float towerHeightProfile = openmwseCloudHeightProfile(bodyRelativeHeight, 1.0, towerDevelopment);
    float stormHeightProfile
        = max(deckHeightProfile, towerHeightProfile * stormBodyWeight);
    float heightProfile = mix(familyHeightProfile, stormHeightProfile, stormWeight);
    float stormSystemDeck = stormWeight * columnProperties.stormSystemWeight
        * (1.0 - smoothstep(0.42, 0.62, deckRelativeHeight));
    float anvilProfile = smoothstep(0.68, 0.78, outflowRelativeHeight)
        * (1.0 - smoothstep(0.92, 0.98, outflowRelativeHeight));
    float requestedCoverage = clamp(layer.parameters.x, 0.0, 1.0);
    float stormDeckCoverage
        = openmwseCloudColumnOccupancy(columnProperties.organizationPotential, requestedCoverage);
    float expandedCoverage = openmwseCloudLocalCoverage(columnProperties.organizationPotential,
        clamp(requestedCoverage + outflowSupport * 0.12, 0.0, 1.0));
    float convectiveOrganization = mix(0.5, 0.72, columnProperties.organizationPotential);
    float stratiformOrganization = mix(0.86, 0.96, columnProperties.organizationPotential);
    float lowerStormDeck = stormWeight * (1.0 - smoothstep(0.16, 0.38, deckRelativeHeight));
    float organizationFamily = mix(clamp(layer.shape.x, 0.0, 1.0), 0.2, lowerStormDeck);
    float typeOrganization = mix(stratiformOrganization, convectiveOrganization,
        smoothstep(0.3, 0.78, organizationFamily));
    float coherentDeckCoverage = max(localCoverage, stormDeckCoverage * 0.58);
    float organizedCoverage = mix(localCoverage, coherentDeckCoverage, lowerStormDeck);
    float cloudCoverage
        = mix(organizedCoverage, expandedCoverage, upperOutflow * outflowSupport) * typeOrganization;
    float reconstructionThreshold
        = mix(0.27, 0.31, smoothstep(0.18, 0.78, cloudFamily)) + developedCell * 0.02;
    float typedCloudNoise = openmwseCloudRemap(cloudNoise, reconstructionThreshold, 1.0);
    float multipliedProfileNoise = typedCloudNoise * heightProfile;
    float erodedProfileNoise = max(typedCloudNoise - (1.0 - heightProfile), 0.0);
    float profileErosionWeight = smoothstep(0.2, 0.68, cloudFamily);
    float dimensionalNoise
        = mix(multipliedProfileNoise, erodedProfileNoise, profileErosionWeight);
    float coveredNoise = openmwseCloudRemap(dimensionalNoise, 1.0 - cloudCoverage, 1.0);
    float stormSystemCoverage
        = mix(0.62, 0.92, columnProperties.stormSystemWeight) * columnProperties.stormSystemWeight;
    float stormSystemNoise = openmwseCloudRemap(typedCloudNoise * deckHeightProfile,
        1.0 - stormSystemCoverage, 1.0) * stormSystemDeck;
    coveredNoise = max(coveredNoise, stormSystemNoise);

    float bodyCoverage = stormWeight * smoothstep(0.06, 0.82, stormBodyWeight)
        * mix(0.5, 0.8, stormBodyWeight);
    bodyCoverage *= mix(1.0, 0.72, smoothstep(0.56, 0.9, bodyRelativeHeight));
    float bodyThreshold = reconstructionThreshold
        + smoothstep(0.54, 0.92, bodyRelativeHeight) * 0.07;
    float bodyBaseNoise = openmwseCloudRemap(cloudNoise, bodyThreshold, 1.0);
    float bodyDimensionalNoise
        = max(bodyBaseNoise - (1.0 - towerHeightProfile * stormBodyWeight), 0.0);
    float stormBodyNoise = openmwseCloudRemap(bodyDimensionalNoise,
        1.0 - bodyCoverage, 1.0) * developedCell;
    coveredNoise = max(coveredNoise, stormBodyNoise);

    float anvilPresence = smoothstep(0.08, 0.72, outflowSupport);
    float anvilSystemCoverage = mix(0.38, 0.66, anvilPresence);
    float anvilBaseNoise = openmwseCloudRemap(cloudNoise, reconstructionThreshold + 0.045, 1.0);
    float anvilDimensionalNoise
        = max(anvilBaseNoise - (1.0 - anvilProfile), 0.0);
    float anvilNoise = openmwseCloudRemap(anvilDimensionalNoise,
        1.0 - anvilSystemCoverage, 1.0) * outflowSupport;
    coveredNoise = max(coveredNoise, anvilNoise);
    float dimensionalExponent = 0.65;
    float reconstructedDensity = pow(coveredNoise, dimensionalExponent) * cloudCoverage;
    float developedDensityScale = mix(1.0, 1.18, developedCell);
    return reconstructedDensity * max(layer.parameters.y, 0.0) * developedDensityScale
        * openmwseCloudOrographicDensityScale(worldPosition, layer);
}

/// Evaluate cloud density with high-frequency billows filtered beyond their spatial sampling footprint.
float openmwseCloudDensity(
    vec3 worldPosition, float distanceFromEye, float sampleFootprint, OpenMWSECloudLayer layer)
{
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    if (worldPosition.z < layerHeightBounds.x || worldPosition.z > layerHeightBounds.y)
        return 0.0;
    float distanceDetail = 1.0 - smoothstep(80000.0, 300000.0, distanceFromEye);
    float footprintDetail = 1.0 - smoothstep(300.0, 1800.0, sampleFootprint);
    float billowDetailAmount = distanceDetail * footprintDetail * 0.5;
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    OpenMWSECloudColumnProperties columnProperties
        = openmwseCloudColumnProperties(
            openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters, layer.shape);
    if (max(columnProperties.localCoverage,
            max(columnProperties.stormSystemWeight, columnProperties.stormOutflowWeight)) <= 0.001)
        return 0.0;
    vec2 localHeightRange = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localHeightRange.x || worldPosition.z > localHeightRange.x + localHeightRange.y)
        return 0.0;
    float relativeHeight = clamp((worldPosition.z - localHeightRange.x) / localHeightRange.y, 0.0, 1.0);
    vec3 baseCoordinates
        = openmwseCloudBaseNoiseCoordinates(
            advectedPosition, columnProperties, layer.shape, relativeHeight, billowDetailAmount);
    vec4 baseNoiseSample = openmwseCloudNoiseSample(baseCoordinates);
    float noiseFamily
        = mix(columnProperties.family, layer.shape.x, smoothstep(0.78, 1.0, layer.shape.x));
    float baseNoise = openmwseCloudTypedBaseNoise(baseNoiseSample, noiseFamily);
    float coarseDensity
        = openmwseCloudDensityFromNoise(
            worldPosition, advectedPosition, baseNoise, columnProperties, layer);
    if (coarseDensity <= 0.001 || billowDetailAmount <= 0.01)
        return coarseDensity;

    float detailFamily
        = mix(columnProperties.family, layer.shape.x, smoothstep(0.78, 1.0, layer.shape.x));
    float detailScale = mix(2.8, 3.8, detailFamily);
    vec4 detailNoise = openmwseCloudDetailNoiseSample(baseCoordinates * detailScale + vec3(0.19, 0.43, 0.71));
    float detailFbm = dot(detailNoise.rgb, vec3(0.625, 0.25, 0.125));
    float heightErosion = mix(1.0 - detailFbm, detailFbm, clamp(relativeHeight * 10.0, 0.0, 1.0));
    float erosionStrength = mix(0.12, 0.22, detailFamily);
    float normalizedCoarseDensity = coarseDensity / max(layer.parameters.y, 0.001);
    float detailedDensity = openmwseCloudRemap(normalizedCoarseDensity, heightErosion * erosionStrength, 1.0)
        * max(layer.parameters.y, 0.0);
    return mix(coarseDensity, detailedDensity, billowDetailAmount);
}

/// Evaluate cloud density from only the stable low-frequency field.
float openmwseCloudBroadDensity(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    if (worldPosition.z < layerHeightBounds.x || worldPosition.z > layerHeightBounds.y)
        return 0.0;
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    OpenMWSECloudColumnProperties columnProperties
        = openmwseCloudColumnProperties(
            openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters, layer.shape);
    if (max(columnProperties.localCoverage,
            max(columnProperties.stormSystemWeight, columnProperties.stormOutflowWeight)) <= 0.001)
        return 0.0;
    vec2 localHeightRange = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localHeightRange.x || worldPosition.z > localHeightRange.x + localHeightRange.y)
        return 0.0;
    float relativeHeight = clamp((worldPosition.z - localHeightRange.x) / localHeightRange.y, 0.0, 1.0);
    vec3 baseCoordinates
        = openmwseCloudBaseNoiseCoordinates(advectedPosition, columnProperties, layer.shape, relativeHeight, 0.0);
    vec4 baseNoiseSample = openmwseCloudNoiseSample(baseCoordinates);
    float noiseFamily
        = mix(columnProperties.family, layer.shape.x, smoothstep(0.78, 1.0, layer.shape.x));
    float baseNoise = openmwseCloudTypedBaseNoise(baseNoiseSample, noiseFamily);
    return openmwseCloudDensityFromNoise(
        worldPosition, advectedPosition, baseNoise, columnProperties, layer);
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

/// Evaluate a normalized Henyey-Greenstein phase lobe for cloud droplets.
float openmwseCloudMiePhase(float cosineAngle, float anisotropy)
{
    float denominator = 1.0 + anisotropy * anisotropy - 2.0 * anisotropy * cosineAngle;
    return (1.0 - anisotropy * anisotropy) / max(12.5663706 * pow(denominator, 1.5), 0.001);
}

/// Approximate cloud multiple scattering with progressively wider phase lobes.
float openmwseCloudMultipleScatteringPhase(float cosineAngle)
{
    float primaryScattering = openmwseCloudMiePhase(cosineAngle, 0.68);
    float secondaryScattering = openmwseCloudMiePhase(cosineAngle, 0.34);
    float tertiaryScattering = openmwseCloudMiePhase(cosineAngle, 0.17);
    return primaryScattering * 0.72 + secondaryScattering * 0.22 + tertiaryScattering * 0.06;
}

/// Approximate one stratum's optical depth toward the sun from a cloud sample.
float openmwseCloudSunOpticalDepth(vec3 samplePosition, vec3 sunDirection, int quality,
    float localDensity, OpenMWSECloudLayer layer)
{
    if (!openmwseCloudLayerEnabled(layer))
        return 0.0;
    vec2 layerHeightBounds = openmwseCloudLayerHeightBounds(layer);
    if (sunDirection.z > 0.0 && samplePosition.z >= layerHeightBounds.y)
        return 0.0;
    vec2 localHeightRange = openmwseCloudLocalLayer(samplePosition, layer);
    float layerTop = localHeightRange.x + localHeightRange.y;
    float distanceToTop = max(layerTop - samplePosition.z, 0.0) / max(sunDirection.z, 0.12);
    vec3 coneReference = abs(sunDirection.z) < 0.95 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 coneRight = normalize(cross(sunDirection, coneReference));
    vec3 coneUp = cross(coneRight, sunDirection);
    vec3 nearPosition = samplePosition + sunDirection * (distanceToTop * 0.28)
        + coneRight * (distanceToTop * 0.035);
    float nearDensity = openmwseCloudBroadDensity(nearPosition, layer);
    float farDensity = quality >= 3
        ? openmwseCloudBroadDensity(samplePosition + sunDirection * (distanceToTop * 0.72)
              + (coneUp * 0.8 - coneRight * 0.6) * (distanceToTop * 0.11), layer)
        : nearDensity;
    return (localDensity * 0.28 + nearDensity * 0.42 + farDensity * 0.3)
        * distanceToTop * 0.00018;
}

/// Approximate direct-sun access through both cloud strata without duplicating transport policy.
float openmwseCloudSunAccess(vec3 samplePosition, vec3 sunDirection, int quality,
    float mainDensity, float lowDensity, OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer lowLayer)
{
    float opticalDepth = openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, mainDensity, mainLayer);
    opticalDepth += openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, lowDensity, lowLayer);
    return exp(-opticalDepth);
}

/// Apply the canonical bounded aerial perspective to one cloud stratum's accumulated radiance.
vec3 openmwseCloudAtmosphericContribution(vec3 scatteredLight, float opacity,
    float representativeDistanceSum, vec3 atmosphericTarget, float opticalDistanceLimit,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    if (opacity <= 1e-5)
        return vec3(0.0);

    float representativeDistance = representativeDistanceSum / opacity;
    vec3 meanRadiance = scatteredLight / opacity;
    vec3 atmosphericRadiance = openmwseApplyAerialPerspectiveTowardTarget(
        meanRadiance, atmosphericTarget, representativeDistance, opticalDistanceLimit, atmosphere);
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
    if (mainLayerEnabled)
    {
        vec2 mainHeightBounds = openmwseCloudLayerHeightBounds(mainLayer);
        mainInterval = openmwseCloudLayerIntersection(
            rayOrigin, worldDirection, mainHeightBounds.x, mainHeightBounds.y);
        mainInterval.y = min(mainInterval.y, maximumDistance);
    }
    if (lowLayerEnabled)
    {
        vec2 lowHeightBounds = openmwseCloudLayerHeightBounds(lowLayer);
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

    const int maximumSampleCount = 28;
    int sampleCount = 12 + quality * 4;
    if (sampleCount > maximumSampleCount)
        sampleCount = maximumSampleCount;
    float firstIntervalLength = firstInterval.y - firstInterval.x;
    float secondIntervalLength = separatedIntervals ? secondInterval.y - secondInterval.x : 0.0;
    float traversedIntervalLength = firstIntervalLength + secondIntervalLength;
    float stepLength = traversedIntervalLength / float(sampleCount);
    float distanceAlongCloudIntervals = 0.0;
    float transmittance = 1.0;
    vec3 mainScatteredLight = vec3(0.0);
    vec3 lowScatteredLight = vec3(0.0);
    float mainOpacity = 0.0;
    float lowOpacity = 0.0;
    float mainRepresentativeDistanceSum = 0.0;
    float lowRepresentativeDistanceSum = 0.0;
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    float phase = 0.18 + 0.68 * openmwseCloudMultipleScatteringPhase(dot(worldDirection, sunDirection));
    vec3 incidentSolarRadiance = openmwseIncidentSolarRadiance(solarRadiance, atmosphere);
    vec3 directLight = incidentSolarRadiance * (0.3 + 2.15 * phase);
    vec3 viewSkyRadiance = openmwseAtmosphereSkyRadiance(worldDirection, atmosphere);
    vec3 environmentIrradiance = max(openmwseAtmosphereEnvironmentIrradiance(skyIrradiance, atmosphere),
        max(viewSkyRadiance * 1.05, atmosphere.skyColor * 0.9));
    environmentIrradiance += incidentSolarRadiance * 0.06;
    vec3 lowerHemisphereIrradiance = max(groundIrradiance, environmentIrradiance * 0.62);

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
        float segmentLength = min(stepLength, activeIntervalLength - intervalPathOffset);
        if (segmentLength <= 0.0)
        {
            distanceAlongCloudIntervals = firstIntervalLength;
            continue;
        }
        float distanceAlongRay = activeInterval.x + intervalPathOffset;
        float sampleOffset = quadratureSampling ? 0.5 : fract(rayDither + float(sampleIndex) * 0.61803398875);
        float sampleDistance = distanceAlongRay + segmentLength * sampleOffset;
        vec3 samplePosition = rayOrigin + worldDirection * sampleDistance;
        float mainDensity = 0.0;
        float lowDensity = 0.0;
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
                mainDensity = 0.5
                    * (openmwseCloudDensity(
                           rayOrigin + worldDirection * firstDistance, firstDistance, firstFootprint, mainLayer)
                        + openmwseCloudDensity(
                            rayOrigin + worldDirection * secondDistance, secondDistance, secondFootprint, mainLayer));
            }
            if (lowLayerEnabled)
            {
                float firstFootprint = max(250.0, firstDistance * 0.0012);
                float secondFootprint = max(250.0, secondDistance * 0.0012);
                lowDensity = 0.5
                    * (openmwseCloudDensity(
                           rayOrigin + worldDirection * firstDistance, firstDistance, firstFootprint, lowLayer)
                        + openmwseCloudDensity(
                            rayOrigin + worldDirection * secondDistance, secondDistance, secondFootprint, lowLayer));
            }
        }
        else
        {
            float rayFootprint = max(250.0, sampleDistance * 0.0012);
            if (mainLayerEnabled)
                mainDensity = openmwseCloudDensity(samplePosition, sampleDistance, rayFootprint, mainLayer);
            if (lowLayerEnabled)
                lowDensity = openmwseCloudDensity(samplePosition, sampleDistance, rayFootprint, lowLayer);
        }
        float density = mainDensity + lowDensity;
        if (density > 0.001)
        {
            float sunAccess = openmwseCloudSunAccess(
                samplePosition, sunDirection, quality, mainDensity, lowDensity, mainLayer, lowLayer);
            float extinction = density * segmentLength * 0.0002;
            float sampleOpacity = 1.0 - exp(-extinction);
            float relativeHeight = 0.0;
            float developedCloudWeight = 0.0;
            if (mainDensity > 0.001)
            {
                vec2 mainHeightRange = openmwseCloudLocalLayer(samplePosition, mainLayer);
                relativeHeight += mainDensity
                    * clamp((samplePosition.z - mainHeightRange.x) / mainHeightRange.y, 0.0, 1.0);
                developedCloudWeight += mainDensity * mainLayer.shape.x * mainLayer.shape.z;
            }
            if (lowDensity > 0.001)
            {
                vec2 lowHeightRange = openmwseCloudLocalLayer(samplePosition, lowLayer);
                relativeHeight += lowDensity
                    * clamp((samplePosition.z - lowHeightRange.x) / lowHeightRange.y, 0.0, 1.0);
                developedCloudWeight += lowDensity * lowLayer.shape.x * lowLayer.shape.z;
            }
            relativeHeight /= density;
            developedCloudWeight /= density;
            vec3 ambientLight
                = mix(lowerHemisphereIrradiance, environmentIrradiance, smoothstep(0.08, 0.92, relativeHeight));
            float ambientAccess = mix(0.34, 0.98, smoothstep(0.04, 0.96, relativeHeight));
            ambientAccess *= mix(0.58, 1.0, sqrt(sunAccess));
            ambientAccess *= mix(1.0, 0.56, smoothstep(0.32, 1.2, density));
            float powder = 1.0 - exp(-density * segmentLength * 0.00034);
            float edgeVisibility = 1.0 - smoothstep(0.25, 1.15, density);
            float silverLining = sunAccess * edgeVisibility * phase * (0.03 + 0.18 * powder);
            float sunOpticalDepth = -log(max(sunAccess, 0.0001));
            float secondarySunAccess = exp(-sunOpticalDepth * 0.55);
            float tertiarySunAccess = exp(-sunOpticalDepth * 0.28);
            float multipleScatteringSun = powder
                * (secondarySunAccess * 0.18 + tertiarySunAccess * 0.06);
            vec3 multipleScatteringLight = incidentSolarRadiance
                * (0.18 + 0.48 * phase) * multipleScatteringSun;
            float stormDarkening = mix(1.0, 0.55, developedCloudWeight * (1.0 - sunAccess));
            vec3 sampleLight = (ambientLight * ambientAccess
                + directLight * (sunAccess * (0.62 + 0.38 * powder) + silverLining)
                + multipleScatteringLight)
                * stormDarkening;
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
    vec2 mainCloudOpticalLimits = useAtmosphericTarget ? vec2(0.72, 1.27296568) : vec2(0.88, 2.12026354);
    float mainCloudOpticalDistanceLimit
        = mix(mainCloudOpticalLimits.x, mainCloudOpticalLimits.y, step(0.5, atmosphere.exponentialFog));
    vec3 scatteredLight = openmwseCloudAtmosphericContribution(mainScatteredLight, mainOpacity,
        mainRepresentativeDistanceSum, cloudAtmosphericTarget, mainCloudOpticalDistanceLimit, atmosphere);
    scatteredLight += openmwseCloudAtmosphericContribution(lowScatteredLight, lowOpacity,
        lowRepresentativeDistanceSum, cloudAtmosphericTarget, 1e20, atmosphere);
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
