#ifndef OPENMWSE_ATMOSPHERE_CLOUDS_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUDS_GLSL

#include "cloud_field.h.glsl"
#include "cloud_model_density.glsl"
#include "cloud_shadow_column.h.glsl"
#include "participating_media.glsl"

/// Read a complete cloud layer from the common CPU publication without deriving morphology in the shader.
CloudModelLayer openmwseReadCloudModel(int layerIndex)
{
    vec4 parameters[14];
    for (int field = 0; field < 14; ++field)
        parameters[field] = openmwseCloudModelParameter(layerIndex, field);
    return cloudModelDecodeLayer(parameters,
        openmwseCloudAnimationSeconds(openmwseCloudAdvectionOffsetWorldUnits()));
}

/// Describe one independently placeable cloud stratum consumed by the shared density and transport path.
struct OpenMWSECloudLayer
{
    vec4 parameters;
    CloudModelLayer model;
    vec2 fieldOffsetWorldUnits;
    float terrainCoupling;
    float terrainWeight;
};

// Use an Earth-scale 6360 km cloud datum. Only the main cloud shell and its optical paths
// use this sphere; Morrowind terrain and the deterministic horizontal density field remain uncurved.
const float openmwseCloudVirtualPlanetRadius
    = 6360000.0 * openmwseAtmosphereWorldUnitsPerMeter;

// Use the same reduced extinction for the second solar path and its analytical view-segment integral.
const float openmwseCloudSecondaryExtinctionScale = 0.1;

// Use the two-point Gauss-Legendre abscissae for both deterministic and temporally interleaved segments.
const float openmwseCloudQuadratureOffsetLow = 0.2113248654;
const float openmwseCloudQuadratureOffsetHigh = 0.7886751346;

// Resolve thin condensate optics within the main deck's broader source sampling allocation.
const int openmwseCondensateTransportSubdivisions = 8;

// Scale density mip distances with the explicit physical scale.
const float openmwseCloudNoiseLodDistance = 30000.0 * openmwseAtmosphereWorldUnitsPerMeter;

/// Return the outgoing episode's independently authored cloud model and Morrowind placement.
OpenMWSECloudLayer openmwseMainCloudLayer()
{
    vec4 parameters = openmwseCloudParameters();
    vec4 scenario = openmwseCloudScenarioParameters();
    parameters.y *= 1.0 - clamp(scenario.z, 0.0, 1.0);
    return OpenMWSECloudLayer(parameters, openmwseReadCloudModel(0),
        openmwseCloudEpisodeOffset(scenario.x), 0.0, 0.0);
}

/// Keep the incoming episode's density model and field phase independent throughout its optical crossfade.
OpenMWSECloudLayer openmwseIncomingCloudLayer()
{
    vec4 parameters = openmwseIncomingCloudParameters();
    vec4 scenario = openmwseCloudScenarioParameters();
    parameters.y *= clamp(scenario.z, 0.0, 1.0) * step(0.5, scenario.w);
    return OpenMWSECloudLayer(parameters, openmwseReadCloudModel(1),
        openmwseCloudEpisodeOffset(scenario.y), 0.0, 0.0);
}

/// Place the environmental cloud layer and resolve its bounded terrain weight once per integration.
OpenMWSECloudLayer openmwseLowCloudLayer()
{
    return OpenMWSECloudLayer(openmwseLowCloudParameters(), openmwseReadCloudModel(2),
        vec2(137000.0, -91000.0), 1.0,
        clamp(openmwseLowCloudEnvironmentInfluence() * openmwseCloudMediumReferenceParameters().y, 0.0, 1.0));
}

/// Return the fixed virtual-planet center for OpenMW's world-space tangent frame.
vec3 openmwseCloudVirtualPlanetCenter()
{
    return vec3(0.0, 0.0, openmwseCloudWaterHeight() - openmwseCloudVirtualPlanetRadius);
}

/// Convert a physical world position to altitude above the virtual spherical cloud datum.
float openmwseCloudVirtualAltitude(vec3 worldPosition)
{
    vec3 relativePosition = worldPosition - openmwseCloudVirtualPlanetCenter();
    // Rationalize the curvature correction to retain local height below the radius's float ULP.
    float curvature = dot(worldPosition.xy, worldPosition.xy)
        / max(length(relativePosition) + relativePosition.z, 1.0);
    return worldPosition.z + curvature;
}

/// Return the position consumed by cloud formation while retaining stable world-space horizontal coordinates.
vec3 openmwseCloudFieldPosition(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    if (layer.terrainCoupling > 0.0)
        return worldPosition;
    worldPosition.z = openmwseCloudVirtualAltitude(worldPosition);
    return worldPosition;
}

/// Skip optical layers with no authored coverage or no density contribution.
bool openmwseCloudLayerEnabled(OpenMWSECloudLayer layer)
{
    return (layer.model.coverageAmount > 0.0 || layer.model.coverageMinimum > 0.0)
        && layer.parameters.y > 0.001;
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

/// Read a height from the packed world lattice.
float openmwseCloudTerrainNode(ivec2 coordinate)
{
    int index = coordinate.y * int(openmwseCloudTerrainOriginSpacing().w) + coordinate.x;
    vec4 heights = openmwseCloudTerrainHeights(index / 4);
    return max(heights[index - (index / 4) * 4], openmwseCloudWaterHeight());
}

/// Interpolate cloud-forming elevation and its continuous gradient at a fixed world position.
vec3 openmwseCloudFormationTerrain(vec2 worldPosition)
{
    vec4 grid = openmwseCloudTerrainOriginSpacing();
    vec2 coordinate = clamp((worldPosition - grid.xy) / grid.z, vec2(0.0), vec2(grid.w - 1.0));
    ivec2 cell = ivec2(min(floor(coordinate), vec2(grid.w - 2.0)));
    vec2 fraction = coordinate - vec2(cell);
    vec2 weight = fraction * fraction * (3.0 - 2.0 * fraction);
    vec2 derivative = 6.0 * fraction * (1.0 - fraction) / grid.z;
    float a = openmwseCloudTerrainNode(cell);
    float b = openmwseCloudTerrainNode(cell + ivec2(1, 0));
    float c = openmwseCloudTerrainNode(cell + ivec2(0, 1));
    float d = openmwseCloudTerrainNode(cell + ivec2(1, 1));
    return vec3(mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y),
        mix(b - a, d - c, weight.y) * derivative.x,
        mix(c - a, d - b, weight.x) * derivative.y);
}

/// Keep terrain-driven placement and droplet formation from the same world-space field evaluation.
struct OpenMWSECloudFormation
{
    float baseHeight;
    float densityScale;
};

/// Evaluate the shared terrain field once for cloud placement and windward or leeward formation.
OpenMWSECloudFormation openmwseCloudFormation(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    float fixedBaseHeight = openmwseCloudWaterHeight() + layer.parameters.z;
    if (layer.terrainWeight <= 0.0)
        return OpenMWSECloudFormation(fixedBaseHeight, 1.0);
    vec3 terrain = openmwseCloudFormationTerrain(worldPosition.xy);
    float terrainBaseHeight = terrain.x + openmwseCloudMediumReferenceParameters().z;
    vec2 cloudMotionDirection = normalize(vec2(-0.78, -0.36));
    float slopeAlongWind = dot(terrain.yz, cloudMotionDirection);
    float windwardFormation = smoothstep(0.025, 0.22, slopeAlongWind);
    float leewardDissipation = smoothstep(0.025, 0.22, -slopeAlongWind);
    float response = 1.0 + windwardFormation * 0.85 - leewardDissipation * 0.7;
    return OpenMWSECloudFormation(mix(fixedBaseHeight, terrainBaseHeight, layer.terrainWeight),
        mix(1.0, response, layer.terrainWeight));
}

/// Follow local terrain only when the low environmental stratum has a positive terrain weight.
float openmwseCloudNominalBaseHeight(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    return openmwseCloudFormation(worldPosition, layer).baseHeight;
}

/// Return the terrain formation multiplier independently of the source cloud-noise model.
float openmwseCloudOrographicDensityScale(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    return openmwseCloudFormation(worldPosition, layer).densityScale;
}

/// Scale the occupied-ray budget independently of authored noise frequencies.
float openmwseCloudLayerMaximumMarchingDistance(OpenMWSECloudLayer layer)
{
    return openmwseCloudMaximumMarchingDistance * min(1.0, layer.model.physicalScale);
}

/// Enclose the authored layer shell and the environmental terrain displacement.
vec2 openmwseCloudLayerHeightBounds(OpenMWSECloudLayer layer)
{
    float base = openmwseCloudWaterHeight() + layer.parameters.z;
    float weight = layer.terrainCoupling * layer.terrainWeight;
    vec2 terrainBounds = max(openmwseCloudTerrainHeightBounds(), vec2(openmwseCloudWaterHeight()))
        + openmwseCloudMediumReferenceParameters().z;
    return vec2(min(base, mix(base, terrainBounds.x, clamp(weight, 0.0, 1.0))),
        max(base, mix(base, terrainBounds.y, clamp(weight, 0.0, 1.0))) + layer.parameters.w);
}

/// Enclose the piecewise-linear gradients reachable through the authored weather-type remap.
vec2 openmwseCloudTypeHeightBounds(CloudModelLayer model)
{
    float minimumType = model.typeMinimum;
    float maximumType = minimumType + min(model.typeAmount, 1.0) * (1.0 - minimumType);
    vec4 first = cloudModelHeightGradient(vec3(0.0, minimumType, 0.0), model);
    vec4 last = cloudModelHeightGradient(vec3(0.0, maximumType, 0.0), model);
    vec4 middle = cloudModelHeightGradient(vec3(0.0, clamp(0.5, minimumType, maximumType), 0.0), model);
    float lower = min(first.x, min(middle.x, last.x));
    float upper = max(first.w, max(middle.w, last.w));
    return vec2(lower, upper);
}

/// Enclose all reachable typed density over the published terrain cache.
vec2 openmwseCloudDensityHeightBounds(OpenMWSECloudLayer layer)
{
    vec2 typeBounds = openmwseCloudTypeHeightBounds(layer.model);
    vec2 bounds = openmwseCloudLayerHeightBounds(layer);
    return bounds + layer.parameters.w * (typeBounds - vec2(0.0, 1.0));
}

/// Bound the fixed terrain field using every interpolation node touched by a ray's horizontal rectangle.
vec2 openmwseCloudTerrainBoundsAlongRay(vec3 origin, vec3 direction, vec2 interval)
{
    vec4 grid = openmwseCloudTerrainOriginSpacing();
    vec2 first = clamp((origin.xy + direction.xy * interval.x - grid.xy) / grid.z,
        vec2(0.0), vec2(grid.w - 1.0));
    vec2 last = clamp((origin.xy + direction.xy * interval.y - grid.xy) / grid.z,
        vec2(0.0), vec2(grid.w - 1.0));
    ivec2 lower = ivec2(min(floor(min(first, last)), vec2(grid.w - 2.0)));
    ivec2 upper = ivec2(min(floor(max(first, last)), vec2(grid.w - 2.0))) + ivec2(1);
    vec2 bounds = vec2(1e20, -1e20);
    // Smooth interpolation is a convex combination of these nodes, including at the clamped cache edge.
    for (int y = lower.y; y <= upper.y; ++y)
        for (int x = lower.x; x <= upper.x; ++x)
        {
            float height = openmwseCloudTerrainNode(ivec2(x, y));
            bounds = vec2(min(bounds.x, height), max(bounds.y, height));
        }
    return bounds;
}

/// Reject empty low-cloud samples without changing the march allocation or its quadrature positions.
vec2 openmwseLowCloudRayDensityHeightBounds(vec3 origin, vec3 direction, vec2 interval,
    OpenMWSECloudLayer layer)
{
    vec2 bases = vec2(openmwseCloudWaterHeight() + layer.parameters.z);
    float influence = layer.terrainWeight;
    if (influence > 0.0)
    {
        vec2 terrain = openmwseCloudTerrainBoundsAlongRay(origin, direction, interval);
        bases = mix(bases, terrain + openmwseCloudMediumReferenceParameters().z, influence);
    }
    // Expand by one world unit to keep floating-point boundary roundoff conservative.
    return bases + layer.parameters.w * openmwseCloudTypeHeightBounds(layer.model) + vec2(-1.0, 1.0);
}

/// Transition the reachable deck top with the weather owner while retaining separate density geometry.
float openmwseWeatherCloudTopHeight()
{
    OpenMWSECloudLayer outgoing = openmwseMainCloudLayer();
    OpenMWSECloudLayer incoming = openmwseIncomingCloudLayer();
    outgoing.parameters.y = openmwseCloudParameters().y;
    incoming.parameters.y = openmwseIncomingCloudParameters().y;
    float outgoingTop = openmwseCloudLayerEnabled(outgoing)
        ? openmwseCloudDensityHeightBounds(outgoing).y : openmwseCloudWaterHeight();
    float incomingTop = openmwseCloudLayerEnabled(incoming)
        ? openmwseCloudDensityHeightBounds(incoming).y : openmwseCloudWaterHeight();
    vec4 scenario = openmwseCloudScenarioParameters();
    return mix(outgoingTop, incomingTop, clamp(scenario.z, 0.0, 1.0) * step(0.5, scenario.w));
}

/// Return physical shell placement independently of the weather map and density model.
vec2 openmwseCloudLocalLayer(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    return vec2(openmwseCloudNominalBaseHeight(worldPosition, layer), max(layer.parameters.w, 1.0));
}

/// Convert OpenMW's world-space field into the fixed Y-up metre domain once at the model boundary.
vec3 openmwseCloudModelPosition(vec3 fieldPosition, OpenMWSECloudLayer layer)
{
    fieldPosition.xy += layer.fieldOffsetWorldUnits;
    return fieldPosition.xzy / openmwseAtmosphereWorldUnitsPerMeter;
}

/// Normalize the world-space field height against its prepared cloud base and thickness.
float openmwseCloudHeightFraction(float fieldHeight, float baseHeight, float thickness)
{
    return (fieldHeight - baseHeight) / max(thickness, 1.0);
}

/// Return the forward ray interval intersecting the conservative cloud-height bounds.
vec2 openmwseCloudLayerIntersection(vec3 origin, vec3 direction, float lowerHeight, float upperHeight)
{
    if (abs(direction.z) < 0.0001)
        return origin.z >= lowerHeight && origin.z <= upperHeight ? vec2(0.0, openmwseCloudMaximumMarchingDistance)
                                                                  : vec2(1.0, 0.0);

    float first = (lowerHeight - origin.z) / direction.z;
    float second = (upperHeight - origin.z) / direction.z;
    return vec2(max(0.0, min(first, second)), min(openmwseCloudMaximumMarchingDistance, max(first, second)));
}

/// Solve a virtual-sphere ray without subtracting nearly equal squared radii or nearby roots.
vec2 openmwseCloudSphereIntersection(vec3 origin, vec3 direction, float height)
{
    float altitude = origin.z - openmwseCloudWaterHeight();
    float shellHeight = height - openmwseCloudWaterHeight();
    float projectedOrigin = dot(origin.xy, direction.xy)
        + (openmwseCloudVirtualPlanetRadius + altitude) * direction.z;
    float radiusDifference = dot(origin.xy, origin.xy)
        + (altitude - shellHeight) * (2.0 * openmwseCloudVirtualPlanetRadius + altitude + shellHeight);
    float discriminant = projectedOrigin * projectedOrigin - radiusDifference;
    if (discriminant < 0.0)
        return vec2(1.0, 0.0);
    float root = sqrt(discriminant);
    float stableRoot = -projectedOrigin - (projectedOrigin >= 0.0 ? root : -root);
    if (abs(stableRoot) < 0.0001)
        return vec2(-projectedOrigin);
    float otherRoot = radiusDifference / stableRoot;
    return vec2(min(stableRoot, otherRoot), max(stableRoot, otherRoot));
}

/// Return a visible spherical-shell interval with the march budget measured from entry, not from the observer.
vec2 openmwseMainCloudLayerIntersection(
    vec3 origin, vec3 direction, float lowerHeight, float upperHeight)
{
    vec2 outer = openmwseCloudSphereIntersection(origin, direction, upperHeight);
    if (outer.x > outer.y || outer.y <= 0.0)
        return vec2(1.0, 0.0);

    vec2 inner = openmwseCloudSphereIntersection(origin, direction, lowerHeight);
    float intervalStart = max(outer.x, 0.0);
    float intervalEnd = outer.y;
    if (openmwseCloudVirtualAltitude(origin) < lowerHeight)
    {
        if (inner.x > inner.y)
            return vec2(1.0, 0.0);
        intervalStart = max(inner.y, 0.0);
    }
    else if (inner.x <= inner.y && inner.x > intervalStart)
    {
        intervalEnd = min(intervalEnd, inner.x);
    }
    if (intervalStart > openmwseCloudPresentationFadeEndDistance)
        return vec2(1.0, 0.0);
    return vec2(intervalStart, min(intervalEnd, openmwseCloudPresentationFadeEndDistance));
}

/// Fade main-cloud density over Morrowind's practical radial range at every observer altitude.
float openmwseMainCloudPresentationWeight(float distanceFromEye)
{
    return 1.0 - smoothstep(
        openmwseCloudPresentationFadeStartDistance,
        openmwseCloudPresentationFadeEndDistance,
        distanceFromEye);
}

/// Use the source weather and typed-height rejection before requesting three-dimensional noise samples.
bool openmwseCloudPotentialDensity(vec3 worldPosition, OpenMWSECloudLayer layer)
{
    vec3 fieldPosition = openmwseCloudFieldPosition(worldPosition, layer);
    float heightFraction = openmwseCloudHeightFraction(fieldPosition.z,
        openmwseCloudNominalBaseHeight(fieldPosition, layer), layer.parameters.w);
    if (heightFraction < 0.0 || heightFraction > 1.0)
        return false;
    vec3 weather = cloudModelSampleWeather(openmwseCloudModelPosition(fieldPosition, layer), heightFraction, layer.model);
    return cloudModelValidDensity(heightFraction, weather, layer.model);
}

/// Carry density and independently authored precipitation optics from one cloud sample.
struct OpenMWSECloudDensitySample
{
    float density;
    float precipitation;
};

/// Retain the nearest view mip before adding the independent light-ray mip offset.
float openmwseCloudNoiseLod(float distanceFromEye, float additionalLodBias, OpenMWSECloudLayer layer)
{
    float featureScale = layer.model.physicalScale;
    float viewLod = floor(max(distanceFromEye, 0.0) / (openmwseCloudNoiseLodDistance * featureScale) + 0.5);
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

/// Retain nonlinear sun samples while rejecting wholly empty terrain-dependent paths before sampling.
vec3 openmwseCloudSunOpticalDepth(vec3 samplePosition, vec3 sunDirection, int quality,
    OpenMWSECloudLayer layer)
{
    if (!openmwseCloudLayerEnabled(layer))
        return vec3(0.0);
    float lightPathLength = 3000.0 * layer.model.physicalScale
        * openmwseAtmosphereWorldUnitsPerMeter;
    if (layer.terrainWeight > 0.0)
    {
        vec2 heightBounds = openmwseCloudLayerHeightBounds(layer);
        // Density normalizes thickness to at least one world unit, including authored sub-unit layers.
        heightBounds.y += max(1.0 - layer.parameters.w, 0.0);
        float lastHeight = samplePosition.z + sunDirection.z * lightPathLength;
        float padding = max(1.0, max(abs(samplePosition.z), abs(lastHeight)) * 0.00001);
        // All fixed light samples lie between these endpoints; the terrain envelope covers the entire field.
        if (max(samplePosition.z, lastHeight) < heightBounds.x - padding
            || min(samplePosition.z, lastHeight) > heightBounds.y + padding)
            return vec3(0.0);
    }
    float distanceFromEye = length(samplePosition - openmwseCloudEyePosition());

    int shadowSampleCount = quality >= 2 ? 5 : quality >= 1 ? 4 : 3;
    vec3 opticalDepth = vec3(0.0);
    float shadowLodBias = 0.5;
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
        // Keep the source offsets fixed even when later samples leave the layer. Renormalizing the
        // lattice to the shell exit changes self-shadowing as a cloud surface crosses the light ray.
        float sampleDistance = lightPathLength * sampleFraction;
        vec3 shadowPosition = samplePosition + sunDirection * sampleDistance;
        if (!openmwseCloudPotentialDensity(shadowPosition, layer))
            continue;
        float shadowDensity = openmwseCloudDensity(
            shadowPosition, distanceFromEye, shadowLodBias, layer);
        opticalDepth += clamp(shadowDensity * layer.model.extinctionPerMeter, 0.0, 1.0)
            * (lightPathLength * intervalFraction / openmwseAtmosphereWorldUnitsPerMeter);
        shadowLodBias += 0.5;
    }
    return opticalDepth;
}

/// Retain independent RGB solar attenuation for both scattering orders.
struct OpenMWSECloudSolarTransmittance
{
    vec3 primary;
    vec3 secondary;
};

/// Return source spectral solar attenuation for the first and reduced-extinction scattering orders.
OpenMWSECloudSolarTransmittance openmwseCloudSunTransmittance(vec3 samplePosition, vec3 sunDirection, int quality,
    OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer incomingLayer, OpenMWSECloudLayer lowLayer)
{
    vec3 opticalDepth = openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, mainLayer);
    opticalDepth += openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, incomingLayer);
    opticalDepth += openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, lowLayer);
    return OpenMWSECloudSolarTransmittance(exp(-opticalDepth),
        exp(-opticalDepth * openmwseCloudSecondaryExtinctionScale));
}

/// Evaluate the shared broad shadow column at the cloud stratum's physical local placement.
float openmwsePlacedCloudColumnOpticalDepth(vec3 position, vec3 sunDirection, OpenMWSECloudLayer layer)
{
    if (!openmwseCloudLayerEnabled(layer))
        return 0.0;
    vec4 parameters = layer.parameters;
    parameters.z = openmwseCloudNominalBaseHeight(position, layer) - openmwseCloudWaterHeight();
    return openmwseCloudShadowColumnOpticalDepth(position, sunDirection, parameters,
        openmwseCloudWaterHeight(), layer.fieldOffsetWorldUnits, layer.model, openmwseCloudWeatherTexture);
}

/// Light broad mist and fog with the same weather-column shadow approximation used by scene surfaces.
float openmwseLocalMediaSunTransmittance(vec3 position, vec3 sunDirection,
    OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer incomingLayer, OpenMWSECloudLayer lowLayer)
{
    float opticalDepth = openmwsePlacedCloudColumnOpticalDepth(position, sunDirection, mainLayer);
    opticalDepth += openmwsePlacedCloudColumnOpticalDepth(position, sunDirection, incomingLayer);
    opticalDepth += openmwsePlacedCloudColumnOpticalDepth(position, sunDirection, lowLayer);
    return exp(-opticalDepth);
}

/// Carry direct irradiance and angle-integrated diffuse radiance into participating-media transport.
struct OpenMWSEVolumeIllumination
{
    vec3 solarIrradiance;
    vec3 diffuseRadiance;
};

/// Convert canonical surface-light units to normalized-phase cloud lighting above the weather deck.
OpenMWSEVolumeIllumination openmwseCloudIllumination(vec3 solarRadiance, vec3 skyIrradiance,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    atmosphere.observerHeight = atmosphere.weatherCloudTopHeight + 9000.0;
    vec3 clearAirSolarRadiance = openmwseClearAirSolarRadiance(solarRadiance, atmosphere);
    vec3 clearAirSkyIrradiance = openmwseClearAirSkyIrradiance(skyIrradiance, atmosphere);
    vec3 incidentSolarRadiance = openmwseIncidentSolarRadiance(clearAirSolarRadiance, atmosphere);
    vec3 skyRadianceMean = openmwseAtmosphereEnvironmentIrradiance(clearAirSkyIrradiance, atmosphere);
    // Native unit-white Lambert lighting is L = sunColor * NdotL, so its incident irradiance is pi * sunColor.
    // The normalized phase consumes irradiance; the sky LUT includes the isotropic angular integral.
    const float lambertRadianceToIrradiance = 3.14159265;
    const float hemisphereIsotropicWeight = 0.5; // 2*pi steradians times 1/(4*pi).
    return OpenMWSEVolumeIllumination(incidentSolarRadiance * lambertRadianceToIrradiance,
        skyRadianceMean * hemisphereIsotropicWeight);
}

/// Average the rendered upper hemisphere with Gaussian elevation quadrature and four fixed azimuths.
vec3 openmwseLocalSkyRadiance(OpenMWSEAerialPerspectiveParameters atmosphere)
{
    vec3 radiance = vec3(0.0);
    for (int elevation = 0; elevation < 2; ++elevation)
    {
        float cosineZenith = elevation == 0 ? openmwseCloudQuadratureOffsetLow : openmwseCloudQuadratureOffsetHigh;
        float horizontalLength = sqrt(1.0 - cosineZenith * cosineZenith);
        vec2 azimuth = vec2(1.0, 0.0);
        for (int direction = 0; direction < 4; ++direction)
        {
            radiance += openmwseAtmosphereSkyRadiance(vec3(azimuth * horizontalLength, cosineZenith), atmosphere);
            azimuth = vec2(-azimuth.y, azimuth.x);
        }
    }
    return radiance * 0.125;
}

/// Illuminate nearby participating media from the visible sky and ground at their physical altitude.
OpenMWSEVolumeIllumination openmwseLocalVolumeIllumination(vec3 solarRadiance,
    vec3 groundIrradiance, float altitude, OpenMWSEAerialPerspectiveParameters atmosphere)
{
    atmosphere.observerHeight = altitude;
    return OpenMWSEVolumeIllumination(
        3.14159265 * openmwseIncidentSolarRadiance(solarRadiance, atmosphere),
        0.5 * (groundIrradiance + openmwseLocalSkyRadiance(atmosphere)));
}

/// Apply the canonical bounded aerial perspective to one cloud stratum's accumulated radiance.
vec3 openmwseCloudAtmosphericContribution(vec3 scatteredLight, float opacity,
    float representativeDistanceSum, vec3 atmosphericTarget, vec3 worldDirection, float opticalDistanceScale,
    float opticalDistanceLimit,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    if (opacity <= 1e-5)
        return vec3(0.0);

    float representativeDistance = representativeDistanceSum / opacity;
    vec3 meanRadiance = scatteredLight / opacity;
    vec3 atmosphericRadiance = openmwseApplyAerialPerspectiveTowardTargetWithScale(meanRadiance,
        atmosphericTarget, worldDirection, representativeDistance, opticalDistanceScale, opticalDistanceLimit, atmosphere);
    return atmosphericRadiance * opacity;
}

/// Preserve distance-based step allocation at the selected physical feature scale.
float openmwseCloudMarchStepLength(float intervalLength, float physicalScale, int quality)
{
    float allocationDistance = 15000.0 * physicalScale
        * openmwseAtmosphereWorldUnitsPerMeter;
    float maximumSteps = float(48 + quality * 16);
    // OpenMW also has terrain condensate much thinner than the kilometre-scale shell. Retain
    // one eighth of the quality budget in short intersections so jitter cannot alternate empty/full coverage.
    return min(intervalLength / (maximumSteps * 0.125), max(intervalLength, allocationDistance) / maximumSteps);
}

/// Interleave cloud and local-medium transport along the ray, retaining visible-opacity-weighted world distances.
vec4 openmwseIntegrateCloudsInternal(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere, float rayDither, bool quadratureSampling,
    bool useAtmosphericTarget, OpenMWSELocalMedia localMedia,
    out float visibleDistanceSum, out float visibleCloudOpacity)
{
    visibleDistanceSum = 0.0;
    visibleCloudOpacity = 0.0;
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer incomingLayer = openmwseIncomingCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    bool outgoingLayerEnabled = openmwseCloudLayerEnabled(mainLayer);
    bool incomingLayerEnabled = openmwseCloudLayerEnabled(incomingLayer);
    bool mainLayerEnabled = outgoingLayerEnabled || incomingLayerEnabled;
    bool lowLayerEnabled = openmwseCloudLayerEnabled(lowLayer);
    if (!mainLayerEnabled && !lowLayerEnabled)
        return openmwseIntegrateLocalMedia(localMedia, maximumDistance, worldDirection, atmosphere, visibleDistanceSum);

    vec2 mainInterval = vec2(1.0, 0.0);
    vec2 lowInterval = vec2(1.0, 0.0);
    vec2 lowHeightBounds = vec2(1e20, -1e20);
    if (mainLayerEnabled)
    {
        vec2 mainDensityBounds = outgoingLayerEnabled
            ? openmwseCloudDensityHeightBounds(mainLayer) : openmwseCloudDensityHeightBounds(incomingLayer);
        if (incomingLayerEnabled && outgoingLayerEnabled)
        {
            vec2 incomingDensityBounds = openmwseCloudDensityHeightBounds(incomingLayer);
            mainDensityBounds = vec2(min(mainDensityBounds.x, incomingDensityBounds.x),
                max(mainDensityBounds.y, incomingDensityBounds.y));
        }
        mainInterval = openmwseMainCloudLayerIntersection(
            rayOrigin, worldDirection, mainDensityBounds.x, mainDensityBounds.y);
        mainInterval.y = min(mainInterval.y, maximumDistance);
    }
    if (lowLayerEnabled)
    {
        lowHeightBounds = openmwseCloudDensityHeightBounds(lowLayer);
        lowInterval = openmwseCloudLayerIntersection(
            rayOrigin, worldDirection, lowHeightBounds.x, lowHeightBounds.y);
        lowInterval.y = min(lowInterval.y, maximumDistance);
        lowInterval.y = min(lowInterval.y,
            lowInterval.x + openmwseCloudLayerMaximumMarchingDistance(lowLayer));
    }
    bool mainIntervalValid = mainInterval.y > mainInterval.x;
    bool lowIntervalValid = lowInterval.y > lowInterval.x;
    if (!mainIntervalValid && !lowIntervalValid)
        return openmwseIntegrateLocalMedia(localMedia, maximumDistance, worldDirection, atmosphere, visibleDistanceSum);

    vec2 lowRayHeightBounds = lowIntervalValid
        ? openmwseLowCloudRayDensityHeightBounds(rayOrigin, worldDirection, lowInterval, lowLayer)
        : vec2(1e20, -1e20);

    vec2 firstInterval = mainIntervalValid ? mainInterval : lowInterval;
    vec2 secondInterval = vec2(1.0, 0.0);
    bool firstIntervalIsMain = mainIntervalValid;
    bool separatedIntervals = false;
    if (mainIntervalValid && lowIntervalValid)
    {
        firstIntervalIsMain = mainInterval.x <= lowInterval.x;
        firstInterval = firstIntervalIsMain ? mainInterval : lowInterval;
        secondInterval = firstIntervalIsMain ? lowInterval : mainInterval;
        if (secondInterval.x <= firstInterval.y)
            firstInterval.y = max(firstInterval.y, secondInterval.y);
        else
            separatedIntervals = true;
    }

    // Quality three permits 96 steps per stratum; reserve the split-boundary segments as well.
    const int maximumSampleCount = 96 * (1 + openmwseCondensateTransportSubdivisions) + 4;
    float firstIntervalLength = min(firstInterval.y - firstInterval.x, openmwseCloudMaximumMarchingDistance);
    float secondIntervalLength = separatedIntervals
        ? min(secondInterval.y - secondInterval.x, openmwseCloudMaximumMarchingDistance - firstIntervalLength) : 0.0;
    float traversedIntervalLength = firstIntervalLength + secondIntervalLength;
    float mainPhysicalScale = min(outgoingLayerEnabled ? mainLayer.model.physicalScale : 1e20,
        incomingLayerEnabled ? incomingLayer.model.physicalScale : 1e20);
    float firstPhysicalScale = firstIntervalIsMain ? mainPhysicalScale : lowLayer.model.physicalScale;
    if (!separatedIntervals && mainIntervalValid && lowIntervalValid)
        firstPhysicalScale = min(mainPhysicalScale, lowLayer.model.physicalScale);
    float firstFineStepLength = openmwseCloudMarchStepLength(firstIntervalLength, firstPhysicalScale, quality);
    float secondFineStepLength = separatedIntervals
        ? openmwseCloudMarchStepLength(secondIntervalLength,
            firstIntervalIsMain ? lowLayer.model.physicalScale : mainPhysicalScale, quality)
        : firstFineStepLength;
    // Independent strata retain the source allocation without spending samples in the empty gap.
    // Continuous lengths prevent allocation-rounding contours as a ray crosses the layer horizon.
    float distanceAlongCloudIntervals = 0.0;
    vec3 transmittance = vec3(1.0);
    float localCursor = 0.0;
    vec3 localScatteredLight = vec3(0.0);
    float localOpacity = 0.0;
    float localDistanceSum = 0.0;
    vec3 mainScatteredLight = vec3(0.0);
    vec3 lowScatteredLight = vec3(0.0);
    float mainOpacity = 0.0;
    float lowOpacity = 0.0;
    float mainRepresentativeDistanceSum = 0.0;
    float lowRepresentativeDistanceSum = 0.0;
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    // The HG denominator uses -2*g*cosine; the equivalent +2*g*cosine form negates its input.
    // Both conventions must place the forward-scattering peak on view rays directed toward the sun.
    float primaryPhase = openmwseCloudDualLobePhase(dot(worldDirection, sunDirection));
    float secondaryPhase = mix(0.07957747, primaryPhase, 0.2);
    OpenMWSEVolumeIllumination illumination = openmwseCloudIllumination(solarRadiance, skyIrradiance, atmosphere);
    // Nearby condensate receives both sky and ground hemispheres at its own altitude.
    // Its illumination cannot inherit the observer's altitude or the main deck's above-cloud recovery.
    OpenMWSEVolumeIllumination lowIllumination = openmwseLocalVolumeIllumination(solarRadiance,
        groundIrradiance, openmwseCloudWaterHeight() + lowLayer.parameters.z + lowLayer.parameters.w * 0.5, atmosphere);
    // Incident light is evaluated above the deck, but the cloud-to-eye path belongs to the real observer.
    vec3 viewSkyRadiance = openmwseAtmosphereSkyRadiance(worldDirection, atmosphere);

    for (int sampleIndex = 0; sampleIndex < maximumSampleCount; ++sampleIndex)
    {
        if (distanceAlongCloudIntervals >= traversedIntervalLength)
            break;
        bool sampleSecondInterval = separatedIntervals && distanceAlongCloudIntervals >= firstIntervalLength;
        bool activeIntervalIsMain
            = sampleSecondInterval ? !firstIntervalIsMain : firstIntervalIsMain;
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
        float fineStepLength = sampleSecondInterval ? secondFineStepLength : firstFineStepLength;
        float segmentLength = min(fineStepLength, remainingIntervalLength);
        float distanceAlongRay = activeInterval.x + intervalPathOffset;
        // Beer-powder albedo and sunlight must resolve the same shallow features as extinction.
        // Averaging fine density into a coarse lighting segment makes radiance change with its boundaries.
        if (lowIntervalValid && distanceAlongRay < lowInterval.y)
        {
            if (distanceAlongRay < lowInterval.x)
                segmentLength = min(segmentLength, lowInterval.x - distanceAlongRay);
            else
                segmentLength = min(min(fineStepLength / float(openmwseCondensateTransportSubdivisions), remainingIntervalLength),
                    lowInterval.y - distanceAlongRay);
        }
        // Keep one ray-coherent abscissa through the march, matching the blue-noise integration.
        // The caller rotates this offset continuously between checkerboard refreshes.
        // Shallow condensate resolves density and lighting on the same finer transport segments.
        // Keep those samples deterministic wherever the ray intersects its occupied height envelope.
        bool sampleQuadrature = quadratureSampling || (lowIntervalValid
            && distanceAlongRay < lowInterval.y && distanceAlongRay + segmentLength > lowInterval.x);
        float sampleOffset = sampleQuadrature ? 0.5 : rayDither;
        float sampleDistance = distanceAlongRay + segmentLength * sampleOffset;
        vec3 samplePosition = rayOrigin + worldDirection * sampleDistance;
        float sampleHeightRadius = sampleQuadrature
            ? abs(worldDirection.z) * segmentLength * (0.5 - openmwseCloudQuadratureOffsetLow) : 0.0;
        // The main interval can continue beyond the low march limit into terrain outside this ray bound.
        bool boundedLowSegment = lowIntervalValid && distanceAlongRay >= lowInterval.x
            && distanceAlongRay + segmentLength <= lowInterval.y;
        vec2 sampleLowHeightBounds = boundedLowSegment
            ? lowRayHeightBounds : lowHeightBounds + vec2(-1.0, 1.0);
        bool sampleLowLayer = lowLayerEnabled
            && samplePosition.z + sampleHeightRadius >= sampleLowHeightBounds.x
            && samplePosition.z - sampleHeightRadius <= sampleLowHeightBounds.y;
        openmwseAdvanceLocalMedia(localMedia, sampleDistance, localCursor, transmittance,
            localScatteredLight, localOpacity, localDistanceSum);
        if (!sampleQuadrature)
        {
            bool fullySampleLowInterval
                = lowLayerEnabled && !activeIntervalIsMain && (!mainIntervalValid || separatedIntervals);
            // A terrain-following low stratum can enter and leave its local height envelope inside one
            // conservative interval. Do not skip the next valid segment from one empty point estimate.
            bool potentialDensity = fullySampleLowInterval
                || (outgoingLayerEnabled && openmwseCloudPotentialDensity(samplePosition, mainLayer));
            potentialDensity = potentialDensity || (incomingLayerEnabled
                && openmwseCloudPotentialDensity(samplePosition, incomingLayer));
            potentialDensity = potentialDensity || (!fullySampleLowInterval && lowLayerEnabled
                && openmwseCloudPotentialDensity(samplePosition, lowLayer));
            if (!potentialDensity)
            {
                const float emptySpaceStepCount = 2.0;
                float skipLength = min(fineStepLength * emptySpaceStepCount, remainingIntervalLength);
                if (lowIntervalValid && distanceAlongRay < lowInterval.x)
                    skipLength = min(skipLength, lowInterval.x - distanceAlongRay);
                distanceAlongCloudIntervals += skipLength;
                continue;
            }
        }
        float mainDensity = 0.0;
        float outgoingDensityForHeight = 0.0;
        float incomingDensityForHeight = 0.0;
        float lowDensity = 0.0;
        float outgoingPrecipitation = 0.0;
        float incomingPrecipitation = 0.0;
        float lowPrecipitation = 0.0;
        if (sampleQuadrature)
        {
            if (outgoingLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudSegmentDensity(
                    rayOrigin, worldDirection, distanceAlongRay, segmentLength, mainLayer);
                mainDensity = sample.density;
                outgoingDensityForHeight = mainDensity;
                outgoingPrecipitation = sample.precipitation;
            }
            if (incomingLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudSegmentDensity(
                    rayOrigin, worldDirection, distanceAlongRay, segmentLength, incomingLayer);
                float incomingDensity = sample.density;
                incomingPrecipitation = sample.precipitation;
                incomingDensityForHeight = incomingDensity;
                mainDensity += incomingDensity;
            }
            if (sampleLowLayer)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudSegmentDensity(
                    rayOrigin, worldDirection, distanceAlongRay, segmentLength, lowLayer);
                lowDensity = sample.density;
                lowPrecipitation = sample.precipitation;
            }
        }
        else
        {
            if (outgoingLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudDensitySample(
                    samplePosition, sampleDistance, 0.0, mainLayer);
                mainDensity = sample.density;
                outgoingDensityForHeight = mainDensity;
                outgoingPrecipitation = sample.precipitation;
            }
            if (incomingLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudDensitySample(
                    samplePosition, sampleDistance, 0.0, incomingLayer);
                incomingPrecipitation = sample.precipitation;
                incomingDensityForHeight = sample.density;
                mainDensity += sample.density;
            }
            if (sampleLowLayer)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudDensitySample(
                    samplePosition, sampleDistance, 0.0, lowLayer);
                lowDensity = sample.density;
                lowPrecipitation = sample.precipitation;
            }
        }
        float mainPresentationWeight = openmwseMainCloudPresentationWeight(sampleDistance);
        mainDensity *= mainPresentationWeight;
        outgoingDensityForHeight *= mainPresentationWeight;
        incomingDensityForHeight *= mainPresentationWeight;
        float density = mainDensity + lowDensity;
        vec3 mainExtinction = outgoingDensityForHeight * mainLayer.model.extinctionPerMeter
            + incomingDensityForHeight * incomingLayer.model.extinctionPerMeter;
        vec3 lowExtinction = lowDensity * lowLayer.model.extinctionPerMeter;
        vec3 combinedExtinction = mainExtinction + lowExtinction;
        vec3 extinction = clamp(combinedExtinction, 0.0, 1.0)
            * (segmentLength / openmwseAtmosphereWorldUnitsPerMeter);
        // Reject negligible optical depth, not density: long segments of thin condensate remain visible.
        if (any(greaterThan(extinction, vec3(1e-6))))
        {
            float relativeHeight = 0.0;
            if (mainDensity > 0.0)
            {
                float mainFieldHeight = openmwseCloudVirtualAltitude(samplePosition);
                if (outgoingDensityForHeight > 0.0)
                {
                    vec2 mainHeightRange = openmwseCloudLocalLayer(samplePosition, mainLayer);
                    relativeHeight += outgoingDensityForHeight
                        * clamp((mainFieldHeight - mainHeightRange.x) / mainHeightRange.y, 0.0, 1.0);
                }
                if (incomingDensityForHeight > 0.0)
                {
                    vec2 incomingHeightRange = openmwseCloudLocalLayer(samplePosition, incomingLayer);
                    relativeHeight += incomingDensityForHeight
                        * clamp((mainFieldHeight - incomingHeightRange.x) / incomingHeightRange.y, 0.0, 1.0);
                }
            }
            if (lowDensity > 0.0)
            {
                vec2 lowHeightRange = openmwseCloudLocalLayer(samplePosition, lowLayer);
                relativeHeight += lowDensity
                    * clamp((samplePosition.z - lowHeightRange.x) / lowHeightRange.y, 0.0, 1.0);
            }
            relativeHeight /= density;
            OpenMWSECloudSolarTransmittance sunTransmittance = openmwseCloudSunTransmittance(
                samplePosition, sunDirection, quality, mainLayer, incomingLayer, lowLayer);
            vec3 sampleTransmittance = exp(-extinction);
            vec3 cloudAlbedo = clamp(
                cloudModelSampleAlbedo(outgoingDensityForHeight, outgoingPrecipitation, mainLayer.model)
                    + cloudModelSampleAlbedo(incomingDensityForHeight, incomingPrecipitation, incomingLayer.model),
                0.0, 1.0);
            // Extinction already accounts for droplet concentration. Applying the source's density-driven
            // powder response to thin condensate would make its single-scattering albedo approach black.
            vec3 lowAlbedo = lowLayer.model.albedo * (1.0 - lowPrecipitation);
            float topEnvironmentAccess = clamp(0.75 + relativeHeight, 0.0, 1.0);
            // Use height-weighted skylight instead of replacing the cloud underside with the
            // brighter ground-irradiance term. Retain a bounded canonical ground bounce near the base.
            vec3 environmentLight = illumination.diffuseRadiance * topEnvironmentAccess;
            environmentLight += groundIrradiance * (1.0 - topEnvironmentAccess) * 0.25;
            vec3 primaryLight
                = environmentLight + illumination.solarIrradiance * (sunTransmittance.primary * primaryPhase);

            // The second scattering order retains the first-order scattering coefficient, reduces
            // extinction by 0.1, and broadens the phase toward isotropic response. Integrate that order
            // analytically over this segment instead of reusing first-order opacity: thick exposed cloud
            // bodies then receive the characteristic soft interior fill without another shadow march.
            vec3 secondaryLight = illumination.solarIrradiance
                * (sunTransmittance.secondary * secondaryPhase);
            vec3 primaryContribution = transmittance * (vec3(1.0) - sampleTransmittance);
            vec3 secondaryContribution = transmittance
                * (vec3(1.0) - exp(-extinction * openmwseCloudSecondaryExtinctionScale))
                / openmwseCloudSecondaryExtinctionScale;
            vec3 radiance = cloudAlbedo
                * (primaryContribution * primaryLight + secondaryContribution * secondaryLight);
            vec3 mainFraction = mainExtinction / max(combinedExtinction, vec3(1e-7));
            vec3 lowFraction = lowExtinction / max(combinedExtinction, vec3(1e-7));
            float mainContribution = dot(primaryContribution * mainFraction, vec3(1.0 / 3.0));
            float lowContribution = dot(primaryContribution * lowFraction, vec3(1.0 / 3.0));
            mainScatteredLight += radiance * mainFraction;
            vec3 lowPrimaryLight = lowIllumination.diffuseRadiance
                + lowIllumination.solarIrradiance * (sunTransmittance.primary * primaryPhase);
            vec3 lowSecondaryLight = lowIllumination.solarIrradiance * (sunTransmittance.secondary * secondaryPhase);
            lowScatteredLight += lowAlbedo * lowFraction
                * (primaryContribution * lowPrimaryLight + secondaryContribution * lowSecondaryLight);
            mainOpacity += mainContribution;
            lowOpacity += lowContribution;
            mainRepresentativeDistanceSum += mainContribution * sampleDistance;
            lowRepresentativeDistanceSum += lowContribution * sampleDistance;
            transmittance *= sampleTransmittance;
            if (all(lessThan(transmittance, vec3(0.005))))
                break;
        }
        distanceAlongCloudIntervals += segmentLength;
    }

    openmwseAdvanceLocalMedia(localMedia, maximumDistance, localCursor, transmittance,
        localScatteredLight, localOpacity, localDistanceSum);
    vec3 cloudAtmosphericTarget = useAtmosphericTarget ? atmosphericTarget : viewSkyRadiance;
    const float mainCloudOpticalDistanceScale = 1.0;
    vec2 mainCloudOpticalLimits = useAtmosphericTarget ? vec2(0.72, 1.27296568) : vec2(0.88, 2.12026354);
    float mainCloudOpticalDistanceLimit
        = mix(mainCloudOpticalLimits.x, mainCloudOpticalLimits.y, step(0.5, atmosphere.exponentialFog));
    vec3 scatteredLight = openmwseCloudAtmosphericContribution(mainScatteredLight, mainOpacity,
        mainRepresentativeDistanceSum, cloudAtmosphericTarget, worldDirection, mainCloudOpticalDistanceScale,
        mainCloudOpticalDistanceLimit, atmosphere);
    scatteredLight += openmwseCloudAtmosphericContribution(lowScatteredLight, lowOpacity,
        lowRepresentativeDistanceSum, cloudAtmosphericTarget, worldDirection, 1.0, 1e20, atmosphere);
    scatteredLight += openmwseLocalMediaRadiance(
        localScatteredLight, localOpacity, localDistanceSum, worldDirection, atmosphere);
    visibleCloudOpacity = mainOpacity + lowOpacity;
    visibleDistanceSum = mainRepresentativeDistanceSum + lowRepresentativeDistanceSum + localDistanceSum;
    return vec4(scatteredLight, dot(transmittance, vec3(1.0 / 3.0)));
}

/// Return the ordered native atmosphere integral and visible cloud contribution for history metadata.
vec4 openmwseIntegrateClouds(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance,
    OpenMWSEAerialPerspectiveParameters atmosphere, float rayDither, bool quadratureSampling,
    OpenMWSELocalMedia localMedia, out float visibleDistanceSum, out float visibleCloudOpacity)
{
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, vec3(0.0), atmosphere, rayDither,
        quadratureSampling, false, localMedia, visibleDistanceSum, visibleCloudOpacity);
}

/// Integrate deterministic reflected clouds without frame-dependent sampling noise.
vec4 openmwseIntegrateCloudsStable(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    float visibleDistanceSum;
    float visibleCloudOpacity;
    OpenMWSELocalMedia localMedia;
    localMedia.count = 0;
    localMedia.startDistance = 0.0;
    localMedia.stepLength = 1.0;
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, atmosphericTarget, atmosphere, 0.5, true, true, localMedia, visibleDistanceSum, visibleCloudOpacity);
}

#endif
