#ifndef OPENMWSE_CLOUD_MODEL_GLSL
#define OPENMWSE_CLOUD_MODEL_GLSL

// This model uses metres and Y-up coordinates. OpenMW placement and atmosphere transport
// belong to the caller; neither changes the density equations here.

#include "cloud_model.h.glsl"

/// Decode SceneUtil's fourteen-vec4 publication and integrate each layer's independent source wind.
CloudModelLayer cloudModelDecodeLayer(vec4 parameters[14], float animationSeconds)
{
    CloudModelLayer layer;
    layer.physicalScale = parameters[13].y;
    layer.albedo = parameters[0].rgb;
    layer.extinctionPerMeter = parameters[1].rgb;
    layer.skewAlongWindDirection = parameters[2].x;
    layer.totalNoiseScale = parameters[2].y;
    layer.curlScale = parameters[2].z;
    layer.curlNoiseHeightFraction = parameters[2].w;
    layer.curlNoiseModifier = parameters[3].x;
    layer.detailScale = parameters[3].y;
    layer.detailNoiseHeightFraction = parameters[3].z;
    layer.detailNoiseModifier = parameters[3].w;
    layer.skewAlongCoverageWindDirection = parameters[4].x;
    layer.weatherScale = parameters[4].y;
    layer.coverageAmount = parameters[4].z;
    layer.coverageMinimum = parameters[4].w;
    layer.typeAmount = parameters[5].x;
    layer.typeMinimum = parameters[5].y;
    layer.rainAmount = parameters[5].z;
    layer.rainMinimum = parameters[5].w;
    layer.gradientSmall = parameters[6];
    layer.gradientMedium = parameters[7];
    layer.gradientLarge = parameters[8];
    layer.anvilDeformationSmall = parameters[9];
    layer.anvilDeformationMedium = parameters[10];
    layer.anvilDeformationLarge = parameters[11];
    layer.windDirection = vec3(cos(parameters[12].y), -parameters[12].z, sin(parameters[12].y));
    layer.windOffset = parameters[12].x * layer.windDirection * animationSeconds;
    layer.coverageWindDirection = vec2(cos(parameters[13].x), sin(parameters[13].x));
    layer.coverageWindOffset = parameters[12].w * layer.coverageWindDirection * animationSeconds;
    return layer;
}

/// Select the continuous small, medium, and large type weights from weather-map G.
vec3 cloudModelWeatherTypeMask(vec3 weatherData)
{
    float weatherType = weatherData.g;
    return vec3(1.0 - clamp(weatherType * 2.0, 0.0, 1.0),
        1.0 - abs(weatherType - 0.5) * 2.0,
        clamp(weatherType - 0.5, 0.0, 1.0) * 2.0);
}

/// Blend the layer's authored height support using the weather-map type.
vec4 cloudModelHeightGradient(vec3 weatherData, CloudModelLayer layer)
{
    vec3 mask = cloudModelWeatherTypeMask(weatherData);
    return layer.gradientSmall * mask.r + layer.gradientMedium * mask.g + layer.gradientLarge * mask.b;
}

/// Evaluate the black-white-white-black height gradient before coverage erosion.
float cloudModelSampleHeightGradient(float heightFraction, vec3 weatherData, CloudModelLayer layer)
{
    vec4 gradient = cloudModelHeightGradient(weatherData, layer);
    return smoothstep(gradient.x, gradient.y, heightFraction)
        - smoothstep(gradient.z, gradient.w, heightFraction);
}

/// Apply the source's independent top and bottom coverage deformations within the typed gradient.
float cloudModelSampleAnvilDeformation(float heightFraction, vec3 weatherData, CloudModelLayer layer)
{
    vec3 mask = cloudModelWeatherTypeMask(weatherData);
    vec4 gradient = cloudModelHeightGradient(weatherData, layer);
    vec4 deformation = layer.anvilDeformationSmall * mask.r
        + layer.anvilDeformationMedium * mask.g + layer.anvilDeformationLarge * mask.b;
    // GLSL leaves pow(0, 0) undefined; zero-exponent profiles require a constant one.
    float top = deformation.r == 0.0 ? 1.0
        : pow(clamp(heightFraction + deformation.g + (1.0 - gradient.a), 0.0, 1.0), deformation.r);
    float bottom = deformation.b == 0.0 ? 1.0
        : pow(clamp((1.0 - heightFraction) + deformation.a + gradient.r, 0.0, 1.0), deformation.b);
    return clamp(top + bottom, 0.0, 1.0);
}

/// Keep the weather-map domain independent from shape advection and shape-noise frequency.
vec2 cloudModelWeatherCoordinates(vec3 positionMeters, float heightFraction, CloudModelLayer layer)
{
    vec2 position = positionMeters.xz + layer.coverageWindOffset;
    position += heightFraction * layer.coverageWindDirection * layer.skewAlongCoverageWindDirection;
    return position * layer.weatherScale;
}

/// Apply independent amount/minimum remaps to coverage, type, and precipitation.
vec3 cloudModelRemapWeather(vec3 weatherData, CloudModelLayer layer)
{
    vec3 amount = vec3(layer.coverageAmount, layer.typeAmount, layer.rainAmount);
    vec3 minimum = vec3(layer.coverageMinimum, layer.typeMinimum, layer.rainMinimum);
    return minimum + clamp(weatherData * amount, 0.0, 1.0) * (vec3(1.0) - minimum);
}

/// Reject empty weather and unsupported heights before sampling either three-dimensional noise volume.
bool cloudModelValidDensity(float heightFraction, vec3 weatherData, CloudModelLayer layer)
{
    vec4 gradient = cloudModelHeightGradient(weatherData, layer);
    float coverage = weatherData.r * cloudModelSampleAnvilDeformation(heightFraction, weatherData, layer);
    return coverage > 0.05 && gradient.r < heightFraction && gradient.a > heightFraction;
}

/// Warp the stationary shape domain before adding the independent shape-wind displacement.
vec3 cloudModelShapeCoordinates(vec3 positionMeters, float heightFraction, CloudModelLayer layer)
{
    vec3 position = vec3(positionMeters.x,
        positionMeters.y + (positionMeters.x / 3.0 + positionMeters.z / 7.0), positionMeters.z);
    position += layer.windOffset;
    position += heightFraction * layer.windDirection * layer.skewAlongWindDirection;
    return position * layer.totalNoiseScale;
}

/// Keep curl fixed in the unadvected horizontal noise domain.
vec2 cloudModelCurlCoordinates(vec3 positionMeters, CloudModelLayer layer)
{
    return positionMeters.xz * layer.curlScale * layer.totalNoiseScale;
}

/// Displace the independently advected detail domain with the decoded curl vector.
vec3 cloudModelDetailCoordinates(vec3 positionMeters, float heightFraction, vec3 encodedCurl,
    CloudModelLayer layer)
{
    vec3 position = positionMeters + layer.windOffset;
    position += heightFraction * layer.windDirection * layer.skewAlongWindDirection;
    vec3 curl = (encodedCurl - 0.5) * 2.0;
    position += curl.rbg * clamp(pow(1.0 - heightFraction, layer.curlNoiseHeightFraction), 0.0, 1.0)
        * layer.curlNoiseModifier;
    return position * layer.detailScale * layer.totalNoiseScale;
}

/// Reconstruct Perlin-Worley mass, then apply the source's height, coverage, and anvil operations in order.
float cloudModelShapeDensity(vec4 shapeNoise, float heightFraction, vec3 weatherData, CloudModelLayer layer)
{
    float fbm = clamp(shapeNoise.g * 0.625 + shapeNoise.b * 0.25 + shapeNoise.a * 0.125, 0.0, 1.0);
    float density = (shapeNoise.r + (1.0 - fbm)) / (2.0 - fbm);
    density *= cloudModelSampleHeightGradient(heightFraction, weatherData, layer);
    density = (density - (1.0 - weatherData.r)) / weatherData.r;
    density *= weatherData.r;
    density *= cloudModelSampleAnvilDeformation(heightFraction, weatherData, layer);
    return density;
}

/// Erode every positive shape sample using the layer's full height-dependent Worley detail response.
float cloudModelErodeDensity(float density, vec3 detailNoise, float heightFraction, CloudModelLayer layer)
{
    float fbm = clamp(detailNoise.r * 0.625 + detailNoise.g * 0.25 + detailNoise.b * 0.125, 0.0, 1.0);
    float modifier = mix(1.0 - fbm, fbm, clamp(heightFraction * layer.detailNoiseHeightFraction, 0.0, 1.0));
    float threshold = modifier * layer.detailNoiseModifier;
    // A quantized noise endpoint can make the erosion interval empty; its limiting density is zero.
    if (threshold >= 1.0)
        return 0.0;
    return max((density - threshold) / (1.0 - threshold), 0.0);
}

/// Evaluate per-layer beer-powder albedo and independent precipitation absorption.
vec3 cloudModelSampleAlbedo(float density, float precipitation, CloudModelLayer layer)
{
    return sqrt(clamp(density * layer.albedo * 20.0, 0.0, 1.0)) * (1.0 - precipitation);
}

#endif
