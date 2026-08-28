#ifndef OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_FIELD_GLSL

const float openmwseCloudMaximumDistance = 850000.0;
const float openmwseCloudBaseVariation = 0.22;
const float openmwseCloudMinimumThicknessScale = 0.45;
const float openmwseCloudMaximumThicknessScale = 1.55;

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

/// Produce a deterministic scalar for a three-dimensional cloud-noise lattice point.
float openmwseCloudHash(vec3 position)
{
    position = fract(position * 0.1031);
    position += dot(position, position.zyx + 31.32);
    return fract((position.x + position.y) * position.z);
}

/// Interpolate smooth deterministic noise across a three-dimensional cloud-noise lattice.
float openmwseCloudValueNoise(vec3 position)
{
    vec3 cell = floor(position);
    vec3 offset = fract(position);
    vec3 blend = offset * offset * (3.0 - 2.0 * offset);
    float lowerNear
        = mix(openmwseCloudHash(cell), openmwseCloudHash(cell + vec3(1.0, 0.0, 0.0)), blend.x);
    float lowerFar = mix(openmwseCloudHash(cell + vec3(0.0, 1.0, 0.0)),
        openmwseCloudHash(cell + vec3(1.0, 1.0, 0.0)), blend.x);
    float upperNear = mix(openmwseCloudHash(cell + vec3(0.0, 0.0, 1.0)),
        openmwseCloudHash(cell + vec3(1.0, 0.0, 1.0)), blend.x);
    float upperFar
        = mix(openmwseCloudHash(cell + vec3(0.0, 1.0, 1.0)), openmwseCloudHash(cell + vec3(1.0)), blend.x);
    return mix(mix(lowerNear, lowerFar, blend.y), mix(upperNear, upperFar, blend.y), blend.z);
}

/// Move a world-space cloud sample with the shared weather wind field.
vec3 openmwseCloudAdvectedPosition(vec3 worldPosition)
{
    worldPosition.xy += openmwseCloudAdvectionOffsetWorldUnits();
    return worldPosition;
}

/// Evaluate the broad weather column shared by cloud density and surface shadows.
float openmwseCloudColumnNoise(vec2 advectedWorldPosition)
{
    float broadShape = openmwseCloudValueNoise(advectedWorldPosition * 0.000018);
    float weatherShape = openmwseCloudValueNoise(advectedWorldPosition * 0.000007 + vec2(21.8, -7.3));
    return mix(broadShape, weatherShape, 0.28);
}

/// Convert shared broad weather noise into normalized cloud-column optical density.
float openmwseCloudColumnDensity(vec2 worldPosition)
{
    vec4 parameters = openmwseCloudParameters();
    float cloudShape = openmwseCloudColumnNoise(openmwseCloudAdvectedPosition(vec3(worldPosition, 0.0)).xy);
    float threshold = mix(0.86, 0.36, clamp(parameters.x, 0.0, 1.0));
    float normalizedDensity
        = smoothstep(0.0, max(0.22 * (1.0 - threshold), 0.04), cloudShape - threshold);
    return normalizedDensity * max(parameters.y, 0.0);
}

#endif
