#ifndef OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL

const float openmwseCloudMaximumDistance = 500000.0;
const float openmwseCloudBaseVariation = 0.22;
const float openmwseCloudMinimumThicknessScale = 0.45;
const float openmwseCloudMaximumThicknessScale = 1.45;
const float openmwseCloudConvectiveCoordinateScale = 1.8;

/// Carry the weather-column signals needed to reconstruct one typed cloud volume.
struct OpenMWSECloudColumnProperties
{
    float organizationPotential;
    float localCoverage;
    float baseVariation;
    float development;
    float family;
    float convectivePotential;
    float stormSystemWeight;
    float stormCellWeight;
    float stormOutflowWeight;
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

/// Reconstruct connected low-frequency cloud mass from packed Perlin-Worley and Worley channels.
float openmwseCloudBaseFromNoise(vec4 noiseSample)
{
    float worleyFbm = dot(noiseSample.gba, vec3(0.625, 0.25, 0.125));
    return openmwseCloudRemap(noiseSample.r, -(1.0 - worleyFbm), 1.0);
}

/// Convert requested coverage and ranked organization potential into column occupancy.
float openmwseCloudLocalCoverage(float organizationPotential, float requestedCoverage);

/// Convert requested coverage into conservative binary support for organized storm columns.
float openmwseCloudColumnOccupancy(float organizationPotential, float requestedCoverage);

/// Map an advected world position into the shared low-frequency weather field.
vec2 openmwseCloudWeatherCoordinates(vec2 advectedWorldPosition, vec4 shape)
{
    float featureScale = max(shape.y, 0.25);
    return advectedWorldPosition * (0.0000032 / featureScale) + vec2(0.37, -0.21);
}

/// Map broad weather coordinates into the smaller cells that organize convective updrafts.
vec2 openmwseCloudConvectiveWeatherCoordinates(vec2 weatherCoordinates)
{
    return weatherCoordinates * openmwseCloudConvectiveCoordinateScale + vec2(-0.31, 0.27);
}

/// Evaluate the authoritative world-space weather field for column occupancy and morphology.
OpenMWSECloudColumnProperties openmwseCloudColumnProperties(
    vec2 advectedWorldPosition, vec4 parameters, vec4 shape)
{
    vec2 weatherCoordinates = openmwseCloudWeatherCoordinates(advectedWorldPosition, shape);
    vec4 weather = openmwseCloudWeatherSample(weatherCoordinates);
    float requestedFamily = clamp(shape.x, 0.0, 1.0);
    float requestedDevelopment = clamp(shape.z, 0.0, 1.0);
    float convectiveBias = smoothstep(0.34, 0.72, requestedFamily);
    float stormBias = smoothstep(0.78, 1.0, requestedFamily);
    float cellularDevelopmentWeight = max(stormBias, convectiveBias * 0.72);
    vec4 convectiveWeather = weather;
    if (cellularDevelopmentWeight > 0.001)
        convectiveWeather
            = openmwseCloudWeatherSample(openmwseCloudConvectiveWeatherCoordinates(weatherCoordinates));
    float convectiveSupportPotential
        = mix(weather.b, convectiveWeather.b, cellularDevelopmentWeight);
    float convectiveDevelopmentPotential
        = clamp(convectiveWeather.b * mix(0.8, 1.3, convectiveWeather.r), 0.0, 1.0);
    float developmentPotential
        = mix(weather.b, convectiveDevelopmentPotential, cellularDevelopmentWeight);
    float convectiveSupport = smoothstep(0.36, 0.88, convectiveSupportPotential);
    float cellularPotential = weather.r * mix(0.18, 1.0, convectiveSupport);
    float stormPotential = max(cellularPotential, weather.r * 0.74);
    float organizationPotential = mix(weather.r, cellularPotential, convectiveBias);
    organizationPotential = mix(organizationPotential, stormPotential, stormBias);
    float columnOccupancy = openmwseCloudLocalCoverage(organizationPotential, parameters.x);
    float outflowOccupancy = openmwseCloudLocalCoverage(
        organizationPotential, clamp(parameters.x + 0.14 * stormBias, 0.0, 1.0));
    float familyVariation = mix(0.12, 0.46, smoothstep(0.28, 0.7, requestedFamily));
    float variedFamily = mix(requestedFamily, weather.a, familyVariation);
    float nonStormConvectiveUplift = (1.0 - stormBias) * convectiveBias
        * smoothstep(0.82, 0.98, developmentPotential) * 0.34;
    variedFamily = clamp(variedFamily + nonStormConvectiveUplift, 0.0, 1.0);
    float stormDeckFamily = mix(0.12, 0.26, weather.a);
    float stormSystemWeight = smoothstep(0.34, 0.76, weather.b)
        * smoothstep(0.04, 0.64, outflowOccupancy) * requestedDevelopment;
    float stormCellPotential = max(convectiveWeather.b, weather.b * 0.82);
    float stormOutflowPotential = max(convectiveWeather.b, weather.b * 0.92);
    float stormCellWeight = smoothstep(0.68, 0.94, stormCellPotential)
        * smoothstep(0.12, 0.72, columnOccupancy) * requestedDevelopment;
    float stormOutflowWeight = smoothstep(0.52, 0.88, stormOutflowPotential)
        * smoothstep(0.06, 0.62, outflowOccupancy) * requestedDevelopment;
    float stormFamily = mix(stormDeckFamily, requestedFamily, stormCellWeight);
    float localFamily = mix(variedFamily, stormFamily, stormBias);
    float developedColumn = smoothstep(0.3, 0.9, developmentPotential);
    float deckDevelopment = requestedDevelopment * mix(0.2, 0.48, developedColumn);
    float cellDevelopment = requestedDevelopment * mix(0.62, 1.0, developedColumn);
    float stormDevelopment = mix(deckDevelopment, cellDevelopment, stormCellWeight);
    float convectiveDevelopmentScale = mix(0.35, 1.3, developedColumn);
    float nonStormDevelopmentScale = mix(developedColumn, convectiveDevelopmentScale, convectiveBias);
    float nonStormDevelopment = requestedDevelopment * nonStormDevelopmentScale;
    float localDevelopment = clamp(mix(nonStormDevelopment, stormDevelopment, stormBias), 0.0, 1.0);
    return OpenMWSECloudColumnProperties(organizationPotential, columnOccupancy, weather.g, localDevelopment,
        localFamily, stormCellPotential, stormSystemWeight, stormCellWeight, stormOutflowWeight);
}

/// Evaluate the broad weather column shared by cloud density and surface shadows.
float openmwseCloudColumnNoise(vec2 advectedWorldPosition)
{
    return openmwseCloudColumnProperties(advectedWorldPosition, openmwseCloudParameters(),
        openmwseCloudShapeParameters()).organizationPotential;
}

/// Convert requested coverage and ranked organization potential into column occupancy.
float openmwseCloudLocalCoverage(float organizationPotential, float requestedCoverage)
{
    requestedCoverage = clamp(requestedCoverage, 0.0, 1.0);
    float occupancyThreshold = 1.0 - requestedCoverage;
    float transitionWidth = mix(0.045, 0.11, smoothstep(0.72, 1.0, requestedCoverage));
    float blanketWeight = smoothstep(0.78, 0.98, requestedCoverage);
    float fullDensityThreshold = mix(1.0, occupancyThreshold + transitionWidth, blanketWeight);
    return smoothstep(occupancyThreshold - transitionWidth, fullDensityThreshold, organizationPotential);
}

/// Convert requested coverage into conservative binary support for organized storm columns.
float openmwseCloudColumnOccupancy(float organizationPotential, float requestedCoverage)
{
    requestedCoverage = clamp(requestedCoverage, 0.0, 1.0);
    float occupancyThreshold = 1.0 - requestedCoverage;
    float transitionWidth = mix(0.045, 0.11, smoothstep(0.72, 1.0, requestedCoverage));
    return smoothstep(occupancyThreshold - transitionWidth, occupancyThreshold + transitionWidth,
        organizationPotential);
}

/// Convert shared broad weather noise into normalized cloud-column optical density.
float openmwseCloudColumnDensity(vec2 worldPosition)
{
    vec4 parameters = openmwseCloudParameters();
    OpenMWSECloudColumnProperties properties = openmwseCloudColumnProperties(
        openmwseCloudAdvectedPosition(vec3(worldPosition, 0.0)).xy, parameters,
        openmwseCloudShapeParameters());
    float normalizedDensity = smoothstep(0.08, 0.82, properties.localCoverage);
    return normalizedDensity * max(parameters.y, 0.0);
}

#endif
