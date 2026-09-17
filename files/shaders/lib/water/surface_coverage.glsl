uniform sampler2D waterSurfaceCoverageMap;
uniform vec2 waterSurfaceCoverageCenter;
uniform float waterSurfaceCoverageInverseSize;
uniform float waterSurfaceCoverageResolution;
uniform float waterCausticMaximumClearance;

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

#ifndef WATER_SURFACE_COVERAGE_VERTEX_STAGE
/// Decode the highest cached blocker relative to the shared water plane.
float waterCausticBlockerHeight(vec4 coverage)
{
    return waterHeight - 4.0 + coverage.g * (waterCausticMaximumClearance + 4.0);
}

/// Identify authored surfaces close enough to the plane to hide rather than merely overhang water.
float waterCausticDryCover(vec4 coverage)
{
    float blockerHeight = waterCausticBlockerHeight(coverage);
    return coverage.b * (1.0 - step(waterHeight + 512.0, blockerHeight));
}

/// Reject blocked rays and paths that cross a sustained dry cover over the global water plane.
float sampleWaterCausticPathExposure(vec3 surfacePosition, vec3 receiverPosition)
{
    vec3 path = receiverPosition - surfacePosition;
    vec4 sourceCoverage = texture2D(waterSurfaceCoverageMap, waterSurfaceCoverageUv(surfacePosition.xy));
    float horizontalDistance = length(path.xy);
    float nearSourceFraction = clamp(128.0 / max(horizontalDistance, 128.0), 0.06, 0.25);
    vec4 nearSourceCoverage = texture2D(waterSurfaceCoverageMap,
        waterSurfaceCoverageUv(surfacePosition.xy + nearSourceFraction * path.xy));
    vec4 middleCoverage = texture2D(
        waterSurfaceCoverageMap, waterSurfaceCoverageUv(surfacePosition.xy + 0.5 * path.xy));
    vec4 farCoverage = texture2D(
        waterSurfaceCoverageMap, waterSurfaceCoverageUv(surfacePosition.xy + 0.75 * path.xy));
    float nearReceiverFraction = clamp(1.0 - 128.0 / max(horizontalDistance, 128.0), 0.0, 0.94);
    vec4 nearReceiverCoverage = texture2D(waterSurfaceCoverageMap,
        waterSurfaceCoverageUv(surfacePosition.xy + nearReceiverFraction * path.xy));
    float sourceBlocked = waterCausticDryCover(sourceCoverage);
    float nearSourceBlocked = nearSourceCoverage.b * step(
        surfacePosition.z + nearSourceFraction * path.z + 4.0,
        waterCausticBlockerHeight(nearSourceCoverage));
    float middleBlocked = middleCoverage.b
        * step(surfacePosition.z + 0.5 * path.z + 4.0, waterCausticBlockerHeight(middleCoverage));
    float farBlocked = farCoverage.b
        * step(surfacePosition.z + 0.75 * path.z + 4.0, waterCausticBlockerHeight(farCoverage));
    float nearReceiverBlocked = nearReceiverCoverage.b * step(
        surfacePosition.z + nearReceiverFraction * path.z + 4.0,
        waterCausticBlockerHeight(nearReceiverCoverage));
    float blocked = max(max(sourceBlocked, nearSourceBlocked),
        max(max(middleBlocked, farBlocked), nearReceiverBlocked));
    float drySpan = max(sourceBlocked,
        max(min(waterCausticDryCover(nearSourceCoverage), waterCausticDryCover(middleCoverage)),
            max(min(waterCausticDryCover(middleCoverage), waterCausticDryCover(farCoverage)),
                min(waterCausticDryCover(farCoverage), waterCausticDryCover(nearReceiverCoverage)))));
    return 1.0 - clamp(max(blocked, drySpan), 0.0, 1.0);
}
#endif

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
