#ifndef OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL

#include "units.glsl"

// Distinguish the empty approach to a visible shell from the bounded march inside it.
const float openmwseCloudMaximumMarchingDistance = 30000.0 * openmwseAtmosphereWorldUnitsPerMeter;
const float openmwseCloudMaximumEntryDistance = 70000.0 * openmwseAtmosphereWorldUnitsPerMeter;
const float openmwseCloudMaximumRayDistance
    = openmwseCloudMaximumEntryDistance + openmwseCloudMaximumMarchingDistance;
const float openmwseCloudBaseVariation = 0.22;
const float openmwseCloudMinimumThicknessScale = 0.45;
const float openmwseCloudMaximumThicknessScale = 1.45;
const float openmwseCloudReferenceShapeNoiseScale = 0.0006 / openmwseAtmosphereWorldUnitsPerMeter;
const float openmwseCloudReferenceWeatherScale = 0.00002 / openmwseAtmosphereWorldUnitsPerMeter;
const float openmwseCloudLowFormationScale = 0.18;
/// Carry the weather-column signals needed to reconstruct one typed cloud volume.
struct OpenMWSECloudColumnProperties
{
    float organizationPotential;
    float localCoverage;
    float baseVariation;
    float family;
    float precipitation;
};

/// Produce a deterministic scalar for a two-dimensional cloud-noise lattice point.
float openmwseCloudHash(vec2 position)
{
    vec3 value = fract(vec3(position.xyx) * vec3(0.1031, 0.1030, 0.0973));
    value += dot(value, value.yzx + 33.33);
    return fract((value.x + value.y) * value.z);
}

/// Interpolate smooth deterministic noise across a two-dimensional cloud-noise lattice.
float openmwseCloudValueNoise(vec2 position)
{
    vec2 cell = floor(position);
    vec2 offset = fract(position);
    vec2 blend = offset * offset * (3.0 - 2.0 * offset);
    float lower = mix(openmwseCloudHash(cell), openmwseCloudHash(cell + vec2(1.0, 0.0)), blend.x);
    float upper
        = mix(openmwseCloudHash(cell + vec2(0.0, 1.0)), openmwseCloudHash(cell + vec2(1.0)), blend.x);
    return mix(lower, upper, blend.y);
}

/// Move a world-space cloud sample with the shared weather wind field.
vec3 openmwseCloudAdvectedPosition(vec3 worldPosition)
{
    worldPosition.xy += openmwseCloudAdvectionOffsetWorldUnits();
    return worldPosition;
}

/// Remap a bounded scalar interval while protecting degenerate ranges.
float openmwseCloudRemap(float value, float lowerBound, float upperBound)
{
    return clamp((value - lowerBound) / max(upperBound - lowerBound, 0.0001), 0.0, 1.0);
}

/// Remap an intermediate cloud-density interval without clipping its erosion response.
float openmwseCloudRemapUnclamped(float value, float lowerBound, float upperBound)
{
    return (value - lowerBound) / max(upperBound - lowerBound, 0.0001);
}

/// Reconstruct connected low-frequency cloud mass from packed Perlin-Worley and Worley channels.
float openmwseCloudBaseFromNoise(vec4 noiseSample)
{
    float worleyFbm = dot(noiseSample.gba, vec3(0.625, 0.25, 0.125));
    return openmwseCloudRemapUnclamped(noiseSample.r, -(1.0 - worleyFbm), 1.0);
}

/// Convert requested coverage and ranked organization potential into column occupancy.
float openmwseCloudLocalCoverage(float organizationPotential, float requestedCoverage);

/// Convert requested coverage into conservative binary support for organized storm columns.
float openmwseCloudColumnOccupancy(float organizationPotential, float requestedCoverage);

/// Map an advected world position into a physically scaled weather field.
vec2 openmwseCloudWeatherCoordinatesForScale(
    vec2 advectedWorldPosition, vec4 shape, float formationScale)
{
    float featureScale = max(shape.y, 0.25);
    return advectedWorldPosition * (openmwseCloudReferenceWeatherScale
        / (featureScale * max(formationScale, 0.01))) + vec2(0.37, -0.21);
}

/// Map a main-cloud position into the shared physically calibrated weather field.
vec2 openmwseCloudWeatherCoordinates(vec2 advectedWorldPosition, vec4 parameters, vec4 shape)
{
    return openmwseCloudWeatherCoordinatesForScale(advectedWorldPosition, shape, 1.0);
}

/// Evaluate the authoritative world-space weather field at one formation scale.
OpenMWSECloudColumnProperties openmwseCloudColumnPropertiesForScale(
    vec2 advectedWorldPosition, vec4 parameters, vec4 shape, vec4 organization,
    vec4 structure, float formationScale)
{
    vec2 weatherCoordinates
        = openmwseCloudWeatherCoordinatesForScale(advectedWorldPosition, shape, formationScale);
    vec4 weather = openmwseCloudWeatherSample(weatherCoordinates);
    float requestedFamily = clamp(shape.x, 0.0, 1.0);
    float requestedDeckFamily = clamp(structure.z, 0.0, 1.0);
    float deckContinuity = clamp(organization.x, 0.0, 1.0);
    float convectiveMaturity = clamp(organization.z, 0.0, 1.0);
    float stormBias = clamp(organization.w, 0.0, 1.0);
    float organizationPotential = weather.r;

    // Translate OpenMW's weather presentation into the amount/minimum parameterization.
    // Only nearly continuous weather raises the minimum substantially; ordinary Cloudy weather retains
    // the holes and connected boundaries authored by the weather map.
    float requestedCoverage = clamp(parameters.x, 0.0, 1.0);
    float coverageAmount = mix(0.65, 1.85, requestedCoverage);
    float coverageMinimum = requestedCoverage * pow(deckContinuity, 4.0)
        * mix(0.55, 0.8, requestedCoverage);
    float deckCoverage = mix(coverageMinimum, 1.0,
        clamp(organizationPotential * coverageAmount, 0.0, 1.0));

    // The G channel continuously selects small, medium, and large profiles inside one deep shell.
    // Convective frequency controls the type amount without thresholding the weather field into terrain-like
    // height contours, while the deck and convective types remain the two weather-owned endpoints.
    float convectiveFrequency = clamp(organization.y, 0.0, 1.0);
    float effectiveFrequency = convectiveFrequency * mix(0.35, 1.0, convectiveMaturity);
    float typeAmount = mix(1.35, 2.05, sqrt(effectiveFrequency))
        * mix(0.92, 1.08, requestedFamily);
    float typeMinimum = 0.14 * stormBias * convectiveMaturity;
    float typeSelection = mix(typeMinimum, 1.0,
        clamp(weather.g * typeAmount, 0.0, 1.0));
    float weatherFamily = mix(requestedDeckFamily, requestedFamily, typeSelection);
    float baseVariation = mix(0.5, weather.a, clamp(structure.y, 0.0, 1.0));
    // Precipitation is independent from coverage and type in weather-map B.
    // OpenMW's presentation state controls its amount while the map keeps wet optical regions local.
    float precipitationAmount = mix(0.0, 0.65, stormBias);
    float precipitationMinimum = openmwseCloudRemap(stormBias, 0.55, 1.0) * 0.06;
    float precipitation = mix(precipitationMinimum, 1.0,
        clamp(weather.b * precipitationAmount, 0.0, 1.0));
    return OpenMWSECloudColumnProperties(organizationPotential, deckCoverage,
        baseVariation, weatherFamily, precipitation);
}

/// Evaluate the authoritative main-cloud weather field for column occupancy and morphology.
OpenMWSECloudColumnProperties openmwseCloudColumnProperties(
    vec2 advectedWorldPosition, vec4 parameters, vec4 shape, vec4 organization, vec4 structure)
{
    return openmwseCloudColumnPropertiesForScale(
        advectedWorldPosition, parameters, shape, organization, structure, 1.0);
}

/// Evaluate the broad weather column shared by cloud density and surface shadows.
float openmwseCloudColumnNoise(vec2 advectedWorldPosition)
{
    return openmwseCloudColumnProperties(advectedWorldPosition, openmwseCloudParameters(),
        openmwseCloudShapeParameters(), openmwseCloudOrganizationParameters(),
        openmwseCloudStructureParameters()).organizationPotential;
}

/// Convert raw weather coverage into its amount/minimum presentation value.
float openmwseCloudLocalCoverage(float organizationPotential, float requestedCoverage)
{
    requestedCoverage = clamp(requestedCoverage, 0.0, 1.0);
    float coverageAmount = mix(0.65, 1.85, requestedCoverage);
    float coverageMinimum = pow(requestedCoverage, 4.0) * 0.78;
    return mix(coverageMinimum, 1.0, clamp(organizationPotential * coverageAmount, 0.0, 1.0));
}

/// Convert requested coverage into conservative binary support for organized storm columns.
float openmwseCloudColumnOccupancy(float organizationPotential, float requestedCoverage)
{
    requestedCoverage = clamp(requestedCoverage, 0.0, 1.0);
    return smoothstep(0.05, 0.12,
        openmwseCloudLocalCoverage(organizationPotential, requestedCoverage));
}

/// Convert shared broad weather noise into normalized cloud-column optical density.
float openmwseCloudColumnDensity(vec2 worldPosition)
{
    vec4 parameters = openmwseCloudParameters();
    OpenMWSECloudColumnProperties properties = openmwseCloudColumnProperties(
        openmwseCloudAdvectedPosition(vec3(worldPosition, 0.0)).xy, parameters,
        openmwseCloudShapeParameters(), openmwseCloudOrganizationParameters(),
        openmwseCloudStructureParameters());
    float normalizedDensity = smoothstep(0.08, 0.82, properties.localCoverage);
    return normalizedDensity * max(parameters.y, 0.0);
}

#endif
