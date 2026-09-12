#ifndef OPENMWSE_ATMOSPHERE_CLOUD_GEOMETRY_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_GEOMETRY_GLSL

#include "cloud_field.h.glsl"
#include "cloud_model_density.glsl"

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

// Use reduced extinction for the second-order solar path.
const float openmwseCloudSecondaryExtinctionScale = 0.1;

// Use two-point Gauss-Legendre density sampling for the main weather deck.
const float openmwseCloudQuadratureOffsetLow = 0.2113248654;
const float openmwseCloudQuadratureOffsetHigh = 0.7886751346;

// Resolve nonlinear source albedo and lighting within the density sampling allocation.
const int openmwseSkyCloudTransportSubdivisions = 3;

// Resolve thin condensate optics within the main deck's broader source sampling allocation.
const int openmwseCondensateTransportSubdivisions = 5;

// Resolve thin condensate with four-point Gaussian density sampling inside each lighting segment.
const vec4 openmwseCondensateQuadratureOffsets = vec4(0.0694318442, 0.3300094782, 0.6699905218, 0.9305681558);
const vec4 openmwseCondensateQuadratureWeights = vec4(0.1739274226, 0.3260725774, 0.3260725774, 0.1739274226);

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

/// Preserve the incoming optical crossfade and eliminate its inactive work in a published steady-weather variant.
OpenMWSECloudLayer openmwseIncomingCloudLayer()
{
    vec4 parameters = openmwseIncomingCloudParameters();
    vec4 scenario = openmwseCloudScenarioParameters();
    parameters.y *= clamp(scenario.z, 0.0, 1.0) * step(0.5, scenario.w);
#if defined(OPENMWSE_CLOUD_STEADY) && OPENMWSE_CLOUD_STEADY
    parameters.y = 0.0;
#endif
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
    return vec3(0.0, 0.0, openmwseCloudWaterHeight() - openmwseAtmospherePlanetRadiusWorldUnits);
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
        + (openmwseAtmospherePlanetRadiusWorldUnits + altitude) * direction.z;
    float radiusDifference = dot(origin.xy, origin.xy)
        + (altitude - shellHeight) * (2.0 * openmwseAtmospherePlanetRadiusWorldUnits + altitude + shellHeight);
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

#endif
