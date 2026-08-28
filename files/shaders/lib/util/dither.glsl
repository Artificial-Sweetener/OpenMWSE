#ifndef OPENMWSE_DITHER_GLSL
#define OPENMWSE_DITHER_GLSL

/// Produce deterministic screen-space noise without texture storage or sampling.
float openmwseDitherHash(vec2 pixelPosition)
{
    return fract(sin(dot(pixelPosition, vec2(12.9898, 78.233))) * 43758.5453);
}

/// Return a stable per-pixel ray-march offset for non-temporal rendering paths.
float openmwseRayMarchDither(vec2 pixelPosition)
{
    return openmwseDitherHash(pixelPosition);
}

/// Return a frame-rotated ray-march offset suitable for temporal accumulation.
float openmwseRayMarchDither(vec2 pixelPosition, float frameNumber)
{
    return fract(openmwseDitherHash(pixelPosition) + frameNumber * 0.61803398875);
}

/// Return zero-mean triangular RGB noise for suppressing visible color banding.
vec3 openmwseTriangularDither(vec2 pixelPosition)
{
    vec3 first = vec3(openmwseDitherHash(pixelPosition), openmwseDitherHash(pixelPosition + vec2(17.0, 53.0)),
        openmwseDitherHash(pixelPosition + vec2(71.0, 29.0)));
    vec3 second = vec3(openmwseDitherHash(pixelPosition + vec2(101.0, 37.0)),
        openmwseDitherHash(pixelPosition + vec2(43.0, 89.0)), openmwseDitherHash(pixelPosition + vec2(11.0, 131.0)));
    return first + second - 1.0;
}

#endif
