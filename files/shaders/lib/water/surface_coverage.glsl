uniform sampler2D waterSurfaceCoverageMap;
uniform vec2 waterSurfaceCoverageCenter;
uniform float waterSurfaceCoverageInverseSize;
uniform float waterSurfaceCoverageResolution;

/// Map one world-space water position into the cached near-surface coverage field.
vec2 waterSurfaceCoverageUv(vec2 surfacePosition)
{
    return (surfacePosition - waterSurfaceCoverageCenter) * waterSurfaceCoverageInverseSize + vec2(0.5);
}

/// Sample fine near-surface coverage for local crest constraints.
float sampleWaterSurfaceCoverage(vec2 surfacePosition)
{
    return texture2D(waterSurfaceCoverageMap, waterSurfaceCoverageUv(surfacePosition)).r;
}

/// Classify a fully covered 512-unit neighborhood while preserving open shores and river banks.
float sampleWaterEnclosedCoverage(vec2 surfacePosition)
{
#ifdef WATER_SURFACE_COVERAGE_VERTEX_STAGE
    float coverage = texture2DLod(waterSurfaceCoverageMap, waterSurfaceCoverageUv(surfacePosition), 2.0).r;
#else
    vec2 coverageUv = waterSurfaceCoverageUv(surfacePosition);
    float footprint = waterSurfaceCoverageResolution * max(length(dFdx(coverageUv)), length(dFdy(coverageUv)));
    float automaticLod = log2(max(footprint, 0.000001));
    float coverage = texture2D(waterSurfaceCoverageMap, coverageUv, 2.0 - automaticLod).r;
#endif
    return smoothstep(0.995, 1.0, coverage);
}
