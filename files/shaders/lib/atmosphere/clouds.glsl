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
        * (1.0 - smoothstep(mix(0.62, 0.68, development), mix(0.92, 0.98, development), relativeHeight));
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

/// Return conservative world-space bounds for the deck and every requested convective cell.
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
    float convectiveEnvelopeWeight = smoothstep(0.02, 0.42, layer.organization.y)
        * clamp(layer.organization.z, 0.0, 1.0);
    float stormEnvelopeWeight = max(clamp(layer.organization.w, 0.0, 1.0), convectiveEnvelopeWeight);
    float conservativeFamily
        = max(clamp(requestedFamily + 0.1, 0.0, 1.0), mix(requestedFamily, 1.0, stormEnvelopeWeight));
    float conservativeDevelopment
        = max(requestedDevelopment, requestedDevelopment * mix(0.62, 1.0, stormEnvelopeWeight));
    float envelopeScale
        = openmwseCloudEnvelopeThicknessScale(conservativeFamily, conservativeDevelopment);
    float profileTop
        = openmwseCloudHeightProfileUpperBound(conservativeFamily, conservativeDevelopment);
    float anvilTop = layer.shape.w * stormEnvelopeWeight > 0.001 ? 0.95 : 0.0;
    float maximumProfileHeight = max(profileTop, anvilTop);
    float maximumBaseVariation = mix(0.08, 0.28, clamp(layer.structure.y, 0.0, 1.0));
    return vec2(nominalBaseHeight - nominalThickness * maximumBaseVariation - terrainExcursion,
        nominalBaseHeight + nominalThickness
            * (maximumBaseVariation + envelopeScale * maximumProfileHeight) + terrainExcursion);
}

/// Derive stable type-aware base-noise coordinates with coherent convective height and cheap wind shear.
vec3 openmwseCloudBaseNoiseCoordinates(
    vec3 advectedPosition, OpenMWSECloudColumnProperties columnProperties, vec4 shape,
    float nominalThickness, float relativeHeight, float distortionAmount)
{
    float featureScale = max(shape.y, 0.25);
    float cloudFamily = mix(columnProperties.family, shape.x, smoothstep(0.78, 1.0, shape.x));
    float domainScale = mix(0.82, 1.22, smoothstep(0.72, 1.6, featureScale));
    float horizontalDomain = max(70000.0,
        nominalThickness * mix(0.9, 1.08, cloudFamily) * domainScale);
    float verticalDomain = max(65000.0,
        nominalThickness * mix(0.82, 1.0, cloudFamily) * domainScale);
    vec3 decorrelatedPosition = advectedPosition;
    decorrelatedPosition.z += advectedPosition.x * 0.31 + advectedPosition.y * 0.17;
    vec3 noiseCoordinates = decorrelatedPosition / vec3(horizontalDomain, horizontalDomain, verticalDomain);
    vec2 curlCoordinates = advectedPosition.xy / (horizontalDomain * 1.35);
    vec2 curl = vec2(openmwseCloudValueNoise(curlCoordinates + vec2(17.3, -9.1)),
                    openmwseCloudValueNoise(curlCoordinates.yx + vec2(-4.7, 23.9)))
        - 0.5;
    noiseCoordinates.xy += curl * distortionAmount * mix(0.12, 0.3, cloudFamily);
    noiseCoordinates.z += dot(curl, vec2(0.57, -0.43)) * distortionAmount
        * mix(0.08, 0.2, cloudFamily) * columnProperties.development;
    noiseCoordinates.xy += vec2(-0.24, 0.15) * relativeHeight * shape.w * columnProperties.development;
    float envelopeDecorrelation = 1.0 - smoothstep(0.28, 0.92,
        max(columnProperties.development, columnProperties.stormCellWeight));
    noiseCoordinates.z += envelopeDecorrelation * mix(0.18, 0.52, cloudFamily);
    return noiseCoordinates;
}

/// Erode reconstructed base density by type and height while retaining smooth organized outflow mass.
float openmwseCloudTypedBaseNoise(
    vec4 noiseSample, float cloudFamily, float relativeHeight, float outflowPresence)
{
    float reconstructedNoise = openmwseCloudBaseFromNoise(noiseSample);
    float worleyFbm = dot(noiseSample.gba, vec3(0.625, 0.25, 0.125));
    float erosionStrength = mix(0.06, 0.32, smoothstep(0.2, 0.85, cloudFamily));
    float cumulusCarving = smoothstep(0.3, 0.62, cloudFamily)
        * (1.0 - smoothstep(0.84, 0.98, cloudFamily));
    erosionStrength += cumulusCarving * 0.12;
    float upperBillowCarving = smoothstep(0.38, 0.9, relativeHeight)
        * smoothstep(0.28, 0.82, cloudFamily);
    erosionStrength += upperBillowCarving * mix(0.12, 0.34, smoothstep(0.7, 1.0, cloudFamily));
    float organizedOutflowSmoothing
        = clamp(outflowPresence, 0.0, 1.0) * smoothstep(0.58, 0.82, relativeHeight);
    erosionStrength *= mix(1.0, 0.22, organizedOutflowSmoothing);
    return openmwseCloudRemap(reconstructedNoise, (1.0 - worleyFbm) * erosionStrength, 1.0);
}

/// Return the local convective body weight without assigning it an independent volume.
float openmwseCloudConvectiveBodyWeight(
    OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    float maturity = smoothstep(0.04, 0.82, clamp(layer.organization.z, 0.0, 1.0));
    float ambientConvection = smoothstep(0.14, 0.9, columnProperties.convectivePotential)
        * smoothstep(0.02, 0.52, clamp(layer.organization.y, 0.0, 1.0)) * maturity
        * (1.0 - clamp(layer.organization.w, 0.0, 1.0))
        * mix(0.68, 0.28, clamp(layer.organization.x, 0.0, 1.0));
    return clamp(max(columnProperties.stormCellWeight, ambientConvection), 0.0, 1.0);
}

/// Return how strongly the background cloud mass should behave as a coherent level deck.
float openmwseCloudBackgroundDeckWeight(OpenMWSECloudLayer layer)
{
    float deckContinuity = smoothstep(0.18, 0.94, clamp(layer.organization.x, 0.0, 1.0));
    float stormDeckWeight = smoothstep(0.35, 0.8, clamp(layer.organization.w, 0.0, 1.0));
    return max(deckContinuity, stormDeckWeight);
}

/// Return the normalized top heights and anvil base for one coherent dimensional profile.
vec4 openmwseCloudVerticalStructure(
    OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    float requestedDevelopment = clamp(layer.shape.z, 0.0, 1.0);
    float deckContinuity = smoothstep(0.18, 0.94, clamp(layer.organization.x, 0.0, 1.0));
    float deckFamily = mix(0.12, 0.26, columnProperties.baseVariation);
    float deckDevelopment = requestedDevelopment * mix(0.2, 0.48,
        smoothstep(0.3, 0.9, columnProperties.convectivePotential));
    float backgroundDeckWeight = openmwseCloudBackgroundDeckWeight(layer);
    float familyTop = openmwseCloudEnvelopeThicknessScale(
        columnProperties.family, columnProperties.development) / openmwseCloudMaximumThicknessScale;
    float isolatedFlatTop = familyTop * mix(0.32, 0.52,
        smoothstep(0.2, 0.85, columnProperties.family));
    float isolatedLiftPotential = smoothstep(0.08, 0.92, columnProperties.convectivePotential)
        * mix(0.38, 1.0, requestedDevelopment);
    float isolatedTop = mix(isolatedFlatTop, familyTop,
        pow(clamp(isolatedLiftPotential, 0.0, 1.0), 0.72));
    float deckTop = openmwseCloudEnvelopeThicknessScale(
        deckFamily, deckDevelopment) / openmwseCloudMaximumThicknessScale;
    float backgroundTop = mix(isolatedTop, deckTop, backgroundDeckWeight);

    float convectiveBodyWeight = openmwseCloudConvectiveBodyWeight(columnProperties, layer);
    float developedTop = openmwseCloudEnvelopeThicknessScale(1.0,
        max(columnProperties.development, requestedDevelopment * convectiveBodyWeight))
        / openmwseCloudMaximumThicknessScale;
    float coreTop = mix(backgroundTop, developedTop, pow(convectiveBodyWeight, 0.9));

    float anvilStrength = clamp(layer.shape.w, 0.0, 1.0)
        * clamp(layer.organization.w, 0.0, 1.0);
    float outflowPresence = smoothstep(0.02, 0.14,
        columnProperties.stormOutflowPotential) * anvilStrength;
    float organizedStormTop = openmwseCloudEnvelopeThicknessScale(1.0,
        max(requestedDevelopment, clamp(layer.organization.z, 0.0, 1.0)))
        / openmwseCloudMaximumThicknessScale;
    float anvilTop = mix(max(backgroundTop, coreTop * 0.82), organizedStormTop * 0.96,
        outflowPresence);
    float anvilDepth = mix(0.08, 0.17, anvilStrength) * mix(0.65, 1.0, outflowPresence);
    float anvilBase = max(backgroundTop * 0.82, anvilTop - anvilDepth);
    return clamp(vec4(backgroundTop, coreTop, anvilBase, anvilTop), 0.0, 1.0);
}

/// Express world height in the shared vertical coordinate used by every local cloud column.
float openmwseCloudProfileHeightFraction(
    float worldHeight, float localBaseHeight, OpenMWSECloudLayer layer)
{
    float profileThickness
        = max(layer.parameters.w, 1.0) * openmwseCloudMaximumThicknessScale;
    return clamp((worldHeight - localBaseHeight) / profileThickness, 0.0, 1.0);
}

/// Return the locally varied base and conservative height of the coherent dimensional profile.
vec2 openmwseCloudLocalLayerFromProperties(
    vec3 worldPosition, OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    float nominalBaseHeight = openmwseCloudNominalBaseHeight(worldPosition, layer);
    float nominalThickness = max(layer.parameters.w, 1.0);
    float baseVariation = mix(0.08, 0.28, clamp(layer.structure.y, 0.0, 1.0));
    float localBaseHeight
        = nominalBaseHeight + (columnProperties.baseVariation * 2.0 - 1.0) * nominalThickness * baseVariation;
    vec4 verticalStructure = openmwseCloudVerticalStructure(columnProperties, layer);
    float localBodyTop = localBaseHeight + nominalThickness * openmwseCloudMaximumThicknessScale
        * max(verticalStructure.x, verticalStructure.y);
    float sharedOutflowTop = nominalBaseHeight
        + nominalThickness * openmwseCloudMaximumThicknessScale * verticalStructure.w;
    return vec2(localBaseHeight, max(localBodyTop, sharedOutflowTop) - localBaseHeight);
}

/// Return the locally varied cloud base height and thickness for a world-space column.
vec2 openmwseCloudLocalLayer(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    vec3 advectedPosition = openmwseCloudLayerPosition(worldPosition, layer);
    return openmwseCloudLocalLayerFromProperties(
        worldPosition,
        openmwseCloudColumnProperties(
            openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters, layer.shape,
            layer.organization, layer.structure),
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
    vec3 worldPosition, float cloudNoise,
    OpenMWSECloudColumnProperties columnProperties, OpenMWSECloudLayer layer)
{
    vec2 localLayer = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localLayer.x || worldPosition.z > localLayer.x + localLayer.y)
        return 0.0;
    float relativeHeight
        = openmwseCloudProfileHeightFraction(worldPosition.z, localLayer.x, layer);
    float outflowRelativeHeight = openmwseCloudProfileHeightFraction(
        worldPosition.z, openmwseCloudNominalBaseHeight(worldPosition, layer), layer);
    float cloudFamily = columnProperties.family;
    float development = columnProperties.development;
    float localCoverage = columnProperties.localCoverage;
    if (max(localCoverage, max(columnProperties.convectiveShoulderWeight,
            max(columnProperties.stormSystemWeight, columnProperties.stormOutflowWeight))) <= 0.001)
        return 0.0;
    vec4 verticalStructure = openmwseCloudVerticalStructure(columnProperties, layer);
    float bottomProfile = smoothstep(0.015, 0.075, relativeHeight);
    float backgroundFade = mix(0.055, 0.11, smoothstep(0.25, 0.9, cloudFamily));
    float backgroundVertical = bottomProfile * (1.0 - smoothstep(
        max(verticalStructure.x - backgroundFade, 0.0), verticalStructure.x, relativeHeight));

    float convectiveBodyWeight = openmwseCloudConvectiveBodyWeight(columnProperties, layer);
    float coreFade = mix(0.09, 0.16, clamp(layer.structure.x, 0.0, 1.0));
    float coreVertical = bottomProfile * (1.0 - smoothstep(
        max(verticalStructure.y - coreFade, 0.0), verticalStructure.y, relativeHeight));
    float lowerShoulderVertical = bottomProfile
        * (1.0 - smoothstep(verticalStructure.x * 0.55,
            max(verticalStructure.x * 0.92, 0.001), relativeHeight));

    float anvilFade = mix(0.025, 0.06, clamp(layer.structure.x, 0.0, 1.0));
    float outflowVertical = smoothstep(verticalStructure.z,
        min(verticalStructure.z + anvilFade, verticalStructure.w), outflowRelativeHeight)
        * (1.0 - smoothstep(max(verticalStructure.w - anvilFade, verticalStructure.z),
            verticalStructure.w, outflowRelativeHeight));
    float anvilStrength = clamp(layer.shape.w, 0.0, 1.0)
        * clamp(layer.organization.w, 0.0, 1.0);
    float outflowPotential = smoothstep(0.02, 0.24,
        columnProperties.stormOutflowPotential) * anvilStrength;
    float deckContinuity = smoothstep(0.18, 0.94, clamp(layer.organization.x, 0.0, 1.0));
    float developedCell = convectiveBodyWeight * smoothstep(0.32, 0.9, development);
    float requestedCoverage = clamp(layer.parameters.x, 0.0, 1.0);
    float stormDeckCoverage
        = openmwseCloudColumnOccupancy(columnProperties.organizationPotential, requestedCoverage);
    float coherentDeckCoverage = max(localCoverage,
        stormDeckCoverage * mix(0.42, 0.82, deckContinuity));
    float isolatedTaperHeight = smoothstep(
        verticalStructure.x * 0.32, verticalStructure.x, relativeHeight);
    float isolatedCoverage = pow(max(localCoverage, 0.0001),
        mix(0.72, 2.2, isolatedTaperHeight));
    float backgroundCoverage = mix(isolatedCoverage, coherentDeckCoverage,
        openmwseCloudBackgroundDeckWeight(layer));
    float backgroundEnvelope = backgroundVertical * backgroundCoverage;
    float coreTaper = mix(0.72, 1.9,
        smoothstep(verticalStructure.x * 0.72, verticalStructure.y, relativeHeight));
    float coreEnvelope = coreVertical * pow(max(convectiveBodyWeight, 0.0001), coreTaper);
    float shoulderEnvelope = lowerShoulderVertical
        * columnProperties.convectiveShoulderWeight * mix(0.28, 0.52, anvilStrength);
    float outflowEnvelope = outflowVertical * outflowPotential;

    float envelopeComplement = (1.0 - clamp(backgroundEnvelope, 0.0, 1.0))
        * (1.0 - clamp(coreEnvelope, 0.0, 1.0))
        * (1.0 - clamp(shoulderEnvelope, 0.0, 1.0))
        * (1.0 - clamp(outflowEnvelope, 0.0, 1.0));
    float dimensionalEnvelope = 1.0 - envelopeComplement;
    float coherentShapeNoise = mix(cloudNoise, 1.0,
        clamp(outflowEnvelope * 0.58, 0.0, 0.58));
    float reconstructedDensity = max(coherentShapeNoise - (1.0 - dimensionalEnvelope), 0.0);
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
            openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters, layer.shape,
            layer.organization, layer.structure);
    if (max(columnProperties.localCoverage, max(columnProperties.convectiveShoulderWeight,
            max(columnProperties.stormSystemWeight, columnProperties.stormOutflowWeight))) <= 0.001)
        return 0.0;
    vec2 localHeightRange = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localHeightRange.x || worldPosition.z > localHeightRange.x + localHeightRange.y)
        return 0.0;
    float relativeHeight
        = openmwseCloudProfileHeightFraction(worldPosition.z, localHeightRange.x, layer);
    vec3 baseCoordinates
        = openmwseCloudBaseNoiseCoordinates(
            advectedPosition, columnProperties, layer.shape, max(layer.parameters.w, 1.0),
            relativeHeight, billowDetailAmount);
    vec4 baseNoiseSample = openmwseCloudNoiseSample(baseCoordinates);
    float noiseFamily
        = mix(columnProperties.family, layer.shape.x, smoothstep(0.78, 1.0, layer.shape.x));
    float outflowPresence = smoothstep(0.02, 0.14, columnProperties.stormOutflowPotential)
        * clamp(layer.shape.w, 0.0, 1.0) * clamp(layer.organization.w, 0.0, 1.0);
    float baseNoise
        = openmwseCloudTypedBaseNoise(baseNoiseSample, noiseFamily, relativeHeight, outflowPresence);
    float coarseDensity
        = openmwseCloudDensityFromNoise(worldPosition, baseNoise, columnProperties, layer);
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
    float effectiveBillowDetailAmount = billowDetailAmount * mix(1.0, 0.3, outflowPresence);
    return mix(coarseDensity, detailedDensity, effectiveBillowDetailAmount);
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
            openmwseCloudColumnPosition(worldPosition, advectedPosition, layer), layer.parameters, layer.shape,
            layer.organization, layer.structure);
    if (max(columnProperties.localCoverage, max(columnProperties.convectiveShoulderWeight,
            max(columnProperties.stormSystemWeight, columnProperties.stormOutflowWeight))) <= 0.001)
        return 0.0;
    vec2 localHeightRange = openmwseCloudLocalLayerFromProperties(worldPosition, columnProperties, layer);
    if (worldPosition.z < localHeightRange.x || worldPosition.z > localHeightRange.x + localHeightRange.y)
        return 0.0;
    float relativeHeight
        = openmwseCloudProfileHeightFraction(worldPosition.z, localHeightRange.x, layer);
    vec3 baseCoordinates
        = openmwseCloudBaseNoiseCoordinates(advectedPosition, columnProperties, layer.shape,
            max(layer.parameters.w, 1.0), relativeHeight, 0.0);
    vec4 baseNoiseSample = openmwseCloudNoiseSample(baseCoordinates);
    float noiseFamily
        = mix(columnProperties.family, layer.shape.x, smoothstep(0.78, 1.0, layer.shape.x));
    float outflowPresence = smoothstep(0.02, 0.14, columnProperties.stormOutflowPotential)
        * clamp(layer.shape.w, 0.0, 1.0) * clamp(layer.organization.w, 0.0, 1.0);
    float baseNoise
        = openmwseCloudTypedBaseNoise(baseNoiseSample, noiseFamily, relativeHeight, outflowPresence);
    return openmwseCloudDensityFromNoise(worldPosition, baseNoise, columnProperties, layer);
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
    float distanceToTop = min(max(layerHeightBounds.y - samplePosition.z, 0.0),
        max(layer.parameters.w, 1.0) * 0.65) / max(sunDirection.z, 0.12);
    vec3 coneReference = abs(sunDirection.z) < 0.95 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 coneRight = normalize(cross(sunDirection, coneReference));
    vec3 coneUp = cross(coneRight, sunDirection);
    vec3 nearConeDirection = coneRight * 0.89442719 + coneUp * 0.4472136;
    vec3 farConeDirection = coneRight * -0.37139068 + coneUp * 0.92847669;
    vec3 nearPosition = samplePosition + sunDirection * (distanceToTop * 0.125)
        + nearConeDirection * (distanceToTop * 0.025);
    float nearDensity = openmwseCloudBroadDensity(nearPosition, layer);
    float farDensity = quality >= 2
        ? openmwseCloudBroadDensity(samplePosition + sunDirection * (distanceToTop * 0.625)
              + farConeDirection * (distanceToTop * 0.07), layer)
        : nearDensity;
    float nearSegmentDensity = mix(localDensity, nearDensity, 0.65);
    return (nearSegmentDensity * 0.25 + farDensity * 0.75) * distanceToTop * 0.00018;
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
    float minimumLayerThickness = 1e20;
    if (mainLayerEnabled)
        minimumLayerThickness = min(minimumLayerThickness, max(mainLayer.parameters.w, 1.0));
    if (lowLayerEnabled)
        minimumLayerThickness = min(minimumLayerThickness, max(lowLayer.parameters.w, 1.0));
    float fineStepLength = clamp(minimumLayerThickness * 0.16, 3500.0, 10000.0);
    float coarseStepLength = max(
        traversedIntervalLength / float(sampleCount), fineStepLength * 2.5);
    float distanceAlongCloudIntervals = 0.0;
    bool fineSampling = false;
    int consecutiveEmptyFineSamples = 0;
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
    OpenMWSEAerialPerspectiveParameters cloudTopAtmosphere = atmosphere;
    cloudTopAtmosphere.observerHeight = atmosphere.weatherCloudTopHeight + 1.0;
    vec3 viewSkyRadiance = openmwseAtmosphereSkyRadiance(worldDirection, cloudTopAtmosphere);
    vec3 environmentIrradiance = max(openmwseAtmosphereEnvironmentIrradiance(skyIrradiance, cloudTopAtmosphere),
        cloudTopAtmosphere.skyColor * 0.9);
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
        float remainingIntervalLength = activeIntervalLength - intervalPathOffset;
        if (remainingIntervalLength <= 0.0)
        {
            distanceAlongCloudIntervals = firstIntervalLength;
            fineSampling = false;
            consecutiveEmptyFineSamples = 0;
            continue;
        }
        float sampleOffset = quadratureSampling ? 0.5 : fract(rayDither + float(sampleIndex) * 0.61803398875);
        float densitySampleOffset = sampleOffset;
        if (!fineSampling)
        {
            float probeLength = min(coarseStepLength, remainingIntervalLength);
            const float firstProbeFraction = 1.0 / 6.0;
            const float secondProbeFraction = 0.5;
            const float thirdProbeFraction = 5.0 / 6.0;
            float probeStartDistance = activeInterval.x + intervalPathOffset;
            float firstBroadDensity = openmwseCloudBroadDensityForLayers(
                rayOrigin + worldDirection * (probeStartDistance + probeLength * firstProbeFraction),
                mainLayerEnabled, lowLayerEnabled, mainLayer, lowLayer);
            float secondBroadDensity = openmwseCloudBroadDensityForLayers(
                rayOrigin + worldDirection * (probeStartDistance + probeLength * secondProbeFraction),
                mainLayerEnabled, lowLayerEnabled, mainLayer, lowLayer);
            float thirdBroadDensity = openmwseCloudBroadDensityForLayers(
                rayOrigin + worldDirection * (probeStartDistance + probeLength * thirdProbeFraction),
                mainLayerEnabled, lowLayerEnabled, mainLayer, lowLayer);
            if (max(firstBroadDensity, max(secondBroadDensity, thirdBroadDensity)) <= 0.0005)
            {
                distanceAlongCloudIntervals += probeLength;
                continue;
            }

            float occupiedProbeFraction = firstBroadDensity > 0.0005 ? firstProbeFraction
                : secondBroadDensity > 0.0005 ? secondProbeFraction
                                              : thirdProbeFraction;
            float occupiedProbeOffset = probeLength * occupiedProbeFraction;
            float entryAdvance = max(occupiedProbeOffset - fineStepLength * 0.5, 0.0);
            intervalPathOffset += entryAdvance;
            distanceAlongCloudIntervals += entryAdvance;
            remainingIntervalLength -= entryAdvance;
            densitySampleOffset = clamp(
                (occupiedProbeOffset - entryAdvance) / min(fineStepLength, remainingIntervalLength), 0.0, 1.0);
            fineSampling = true;
        }

        float segmentLength = min(fineStepLength, remainingIntervalLength);
        float distanceAlongRay = activeInterval.x + intervalPathOffset;
        float sampleDistance = distanceAlongRay + segmentLength * densitySampleOffset;
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
            float relativeHeight = 0.0;
            if (mainDensity > 0.001)
            {
                vec2 mainHeightRange = openmwseCloudLocalLayer(samplePosition, mainLayer);
                relativeHeight += mainDensity
                    * clamp((samplePosition.z - mainHeightRange.x) / mainHeightRange.y, 0.0, 1.0);
            }
            if (lowDensity > 0.001)
            {
                vec2 lowHeightRange = openmwseCloudLocalLayer(samplePosition, lowLayer);
                relativeHeight += lowDensity
                    * clamp((samplePosition.z - lowHeightRange.x) / lowHeightRange.y, 0.0, 1.0);
            }
            relativeHeight /= density;
            float sunAccess = openmwseCloudSunAccess(
                samplePosition, sunDirection, quality, mainDensity, lowDensity, mainLayer, lowLayer);
            float daylightTopExposure = smoothstep(0.42, 0.84, relativeHeight)
                * smoothstep(0.18, 0.7, sunDirection.z);
            float extinction = density * segmentLength * 0.0002;
            float sampleOpacity = 1.0 - exp(-extinction);
            vec3 ambientLight
                = mix(lowerHemisphereIrradiance, environmentIrradiance, smoothstep(0.08, 0.92, relativeHeight));
            ambientLight += incidentSolarRadiance * daylightTopExposure * 0.025;
            float ambientAccess = mix(0.42, 0.98, smoothstep(0.04, 0.96, relativeHeight));
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
            vec3 sampleLight = ambientLight * ambientAccess
                + directLight * (sunAccess * (0.62 + 0.38 * powder) + silverLining)
                + multipleScatteringLight;
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
        if (density > 0.001)
            consecutiveEmptyFineSamples = 0;
        else
        {
            ++consecutiveEmptyFineSamples;
            if (consecutiveEmptyFineSamples >= 2)
            {
                fineSampling = false;
                consecutiveEmptyFineSamples = 0;
            }
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
