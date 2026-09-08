#ifndef OPENMW_COMPONENTS_TESTS_CLOUD_MODEL_REFERENCE_H
#define OPENMW_COMPONENTS_TESTS_CLOUD_MODEL_REFERENCE_H

#include <string_view>

namespace ShaderTest
{
    /** Independent GLSL evaluator for the parameterized cloud density model.
     * The adapter uses OpenMW's texture readers and world-unit conversion only; it does not call
     * the production shader's coordinate, type, coverage, deformation, or erosion helpers. Geometry and weather
     * are explicit inputs so this reference tests density independently of OpenMW policy.
     */
    inline constexpr std::string_view cloudModelDensityReference = R"glsl(
/// Evaluate the source equations from raw layer parameters, independently of the production shader's decoding and modelling helpers.
float referenceCloudDensity(vec3 p, float h, vec3 weather, float lod, vec4 data[14], float animation)
{
    float type = weather.g;
    float coverage = weather.r;
    vec3 weights = vec3(1.0 - clamp(type * 2.0, 0.0, 1.0),
        1.0 - abs(type - 0.5) * 2.0, clamp(type - 0.5, 0.0, 1.0) * 2.0);
    vec4 gradient = data[6] * weights.x + data[7] * weights.y + data[8] * weights.z;
    vec4 deformation = data[9] * weights.x + data[10] * weights.y + data[11] * weights.z;
    float top = deformation.x == 0.0 ? 1.0
        : pow(clamp(h + deformation.y + (1.0 - gradient.w), 0.0, 1.0), deformation.x);
    float bottom = deformation.z == 0.0 ? 1.0
        : pow(clamp(1.0 - h + deformation.w + gradient.x, 0.0, 1.0), deformation.z);
    float anvil = clamp(top + bottom, 0.0, 1.0);
    if (coverage * anvil <= 0.05 || h <= gradient.x || h >= gradient.w)
        return 0.0;
    vec3 direction = vec3(cos(data[12].y), -data[12].z, sin(data[12].y));
    vec3 wind = data[12].x * direction * animation;
    vec3 lowPosition = vec3(p.x, p.y + (p.x / 3.0 + p.z / 7.0), p.z);
    lowPosition += wind;
    lowPosition += h * direction * data[2].x;
    vec4 lowNoise = openmwseCloudNoiseSample(lowPosition * data[2].y, lod);
    float lowFbm = clamp(lowNoise.g * 0.625 + lowNoise.b * 0.25 + lowNoise.a * 0.125, 0.0, 1.0);
    float density = (lowNoise.r + (1.0 - lowFbm)) / (2.0 - lowFbm);
    density *= smoothstep(gradient.x, gradient.y, h) - smoothstep(gradient.z, gradient.w, h);
    density = (density - (1.0 - coverage)) / coverage;
    density *= coverage;
    density *= anvil;
    if (density > 0.0)
    {
        vec3 highPosition = p + wind;
        highPosition += h * direction * data[2].x;
        vec3 curl = (openmwseCloudCurlNoiseSample(p.xz * data[2].z * data[2].y) - 0.5) * 2.0;
        highPosition += curl.xzy * clamp(pow(1.0 - h, data[2].w), 0.0, 1.0) * data[3].x;
        vec3 highNoise = openmwseCloudDetailNoiseSample(highPosition * data[3].y * data[2].y, lod).rgb;
        float highFbm = clamp(highNoise.r * 0.625 + highNoise.g * 0.25 + highNoise.b * 0.125, 0.0, 1.0);
        float erosion = mix(1.0 - highFbm, highFbm, clamp(h * data[3].z, 0.0, 1.0)) * data[3].w;
        density = (density - erosion) / (1.0 - erosion);
    }
    return max(density, 0.0);
}

)glsl";
}

#endif
