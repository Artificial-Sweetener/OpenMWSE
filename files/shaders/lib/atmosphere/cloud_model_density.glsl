#ifndef OPENMWSE_CLOUD_MODEL_DENSITY_GLSL
#define OPENMWSE_CLOUD_MODEL_DENSITY_GLSL


#include "cloud_model.h.glsl"

/// Sample independent RGB weather channels at explicit mip zero in the coverage-wind domain.
vec3 cloudModelSampleWeather(vec3 positionMeters, float heightFraction, CloudModelLayer layer)
{
    vec3 weather = openmwseCloudWeatherSample(
        cloudModelWeatherCoordinates(positionMeters, heightFraction, layer)).rgb;
    return cloudModelRemapWeather(weather, layer);
}

/// Execute the complete shape and detail density pipeline in metres with Y-up coordinates.
float cloudModelSampleDensity(vec3 positionMeters, float heightFraction, CloudModelLayer layer,
    vec3 weatherData, float lod, bool sampleDetail)
{
    if (!cloudModelValidDensity(heightFraction, weatherData, layer))
        return 0.0;
    vec4 shapeNoise = openmwseCloudNoiseSample(
        cloudModelShapeCoordinates(positionMeters, heightFraction, layer), lod);
    float density = cloudModelShapeDensity(shapeNoise, heightFraction, weatherData, layer);
    if (density > 0.0 && sampleDetail)
    {
        vec3 curl = openmwseCloudCurlNoiseSample(cloudModelCurlCoordinates(positionMeters, layer));
        vec3 detailNoise = openmwseCloudDetailNoiseSample(
            cloudModelDetailCoordinates(positionMeters, heightFraction, curl, layer), lod).rgb;
        density = cloudModelErodeDensity(density, detailNoise, heightFraction, layer);
    }
    return max(density, 0.0);
}

#endif
