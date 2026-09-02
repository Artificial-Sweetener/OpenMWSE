#ifndef OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL

const float openmwseCloudMaximumDistance = 500000.0;
const float openmwseCloudBaseVariation = 0.22;
const float openmwseCloudMinimumThicknessScale = 0.45;
const float openmwseCloudMaximumThicknessScale = 1.45;
/// Carry the weather-column signals needed to reconstruct one typed cloud volume.
struct OpenMWSECloudColumnProperties
{
    float organizationPotential;
    float localCoverage;
    float baseVariation;
    float development;
    float family;
    float convectivePotential;
    float convectiveShoulderWeight;
    float stormSystemWeight;
    float stormCellWeight;
    float stormOutflowPotential;
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
    return advectedWorldPosition * (0.0000024 / featureScale) + vec2(0.37, -0.21);
}

/// Evaluate the authoritative world-space weather field for column occupancy and morphology.
OpenMWSECloudColumnProperties openmwseCloudColumnProperties(
    vec2 advectedWorldPosition, vec4 parameters, vec4 shape, vec4 organization, vec4 structure)
{
    vec2 weatherCoordinates = openmwseCloudWeatherCoordinates(advectedWorldPosition, shape);
    vec4 weather = openmwseCloudWeatherSample(weatherCoordinates);
    float requestedFamily = clamp(shape.x, 0.0, 1.0);
    float requestedDevelopment = clamp(shape.z, 0.0, 1.0);
    float deckContinuity = clamp(organization.x, 0.0, 1.0);
    float requestedConvectiveFrequency = clamp(organization.y, 0.0, 1.0);
    float requestedConvectiveMaturity = clamp(organization.z, 0.0, 1.0);
    float stormBias = clamp(organization.w, 0.0, 1.0);
    float organizationPotential = weather.r;
    float columnOccupancy = openmwseCloudLocalCoverage(organizationPotential, parameters.x);

    // The weather resource stores a normalized density-function description of each column:
    // R is deck organization, G is convective core height, B is spreading outflow, and A is base variation.
    float concentratedConvectiveCoverage
        = mix(requestedConvectiveFrequency, requestedConvectiveFrequency * 0.34, stormBias);
    float coreThreshold = mix(0.84, 0.12, sqrt(concentratedConvectiveCoverage));
    float coreOccupancy = smoothstep(coreThreshold, min(coreThreshold + 0.2, 1.0), weather.g);
    float coreShape = smoothstep(0.08, 0.94, weather.g);
    float maturity = smoothstep(0.04, 0.82, requestedConvectiveMaturity);
    float stormCellWeight = coreOccupancy * coreShape * maturity;

    float shoulderCoverage = clamp(concentratedConvectiveCoverage * 1.9, 0.0, 0.72);
    float outflowThreshold = mix(0.76, 0.08, sqrt(shoulderCoverage));
    float outflowOccupancy
        = smoothstep(outflowThreshold, min(outflowThreshold + 0.2, 1.0), weather.b);
    float convectiveShoulderWeight = outflowOccupancy * smoothstep(0.2, 0.88, weather.b)
        * maturity * mix(0.46, 0.82, stormBias);
    float stormSystemWeight = smoothstep(0.22, 0.76, organizationPotential)
        * requestedDevelopment * stormBias;
    float stormOutflowWeight = outflowOccupancy * smoothstep(0.18, 0.9, weather.b)
        * requestedDevelopment * maturity * stormBias * clamp(shape.w, 0.0, 1.0);

    float developedColumn = max(stormCellWeight, convectiveShoulderWeight * 0.58);
    float ambientConvection = smoothstep(0.14, 0.9, weather.g)
        * smoothstep(0.02, 0.52, requestedConvectiveFrequency) * maturity
        * (1.0 - stormBias) * mix(0.68, 0.28, deckContinuity);
    float localConvectiveDevelopment = max(developedColumn, ambientConvection);
    float localFamily = mix(requestedFamily, 1.0,
        localConvectiveDevelopment * smoothstep(0.08, 0.72, requestedConvectiveMaturity));
    float deckDevelopment = requestedDevelopment * mix(0.24, 0.42, deckContinuity);
    float localDevelopment = mix(deckDevelopment,
        max(requestedDevelopment, requestedConvectiveMaturity), localConvectiveDevelopment);
    localDevelopment = clamp(localDevelopment, 0.0, 1.0);
    float baseVariation = mix(0.5, weather.a, clamp(structure.y, 0.0, 1.0));
    return OpenMWSECloudColumnProperties(organizationPotential, columnOccupancy, baseVariation, localDevelopment,
        localFamily, weather.g, convectiveShoulderWeight, stormSystemWeight, stormCellWeight,
        weather.b, stormOutflowWeight);
}

/// Evaluate the broad weather column shared by cloud density and surface shadows.
float openmwseCloudColumnNoise(vec2 advectedWorldPosition)
{
    return openmwseCloudColumnProperties(advectedWorldPosition, openmwseCloudParameters(),
        openmwseCloudShapeParameters(), openmwseCloudOrganizationParameters(),
        openmwseCloudStructureParameters()).organizationPotential;
}

/// Convert requested coverage and ranked organization potential into column occupancy.
float openmwseCloudLocalCoverage(float organizationPotential, float requestedCoverage)
{
    requestedCoverage = clamp(requestedCoverage, 0.0, 1.0);
    float occupancyThreshold = 1.0 - requestedCoverage;
    float transitionWidth = mix(0.045, 0.11, smoothstep(0.72, 1.0, requestedCoverage));
    float blanketWeight = smoothstep(0.78, 0.98, requestedCoverage);
    float fullDensityThreshold
        = min(1.0, occupancyThreshold + mix(0.22, transitionWidth, blanketWeight));
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
        openmwseCloudShapeParameters(), openmwseCloudOrganizationParameters(),
        openmwseCloudStructureParameters());
    float normalizedDensity = smoothstep(0.08, 0.82, properties.localCoverage);
    return normalizedDensity * max(parameters.y, 0.0);
}

#endif
