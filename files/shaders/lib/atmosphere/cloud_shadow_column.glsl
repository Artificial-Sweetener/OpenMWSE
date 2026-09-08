#ifndef OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_COLUMN_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_COLUMN_GLSL

#include "cloud_field.h.glsl"

/// Estimate surface-shadow optical depth only through the typed weather column above a Z-up receiver.
float openmwseCloudShadowColumnOpticalDepth(vec3 receiver, vec3 sunDirection,
    vec4 parameters, float waterDatum, vec2 episodeOffset, CloudModelLayer model,
    sampler2D weatherTexture)
{
    float base = waterDatum + parameters.z;
    float thickness = max(parameters.w, 1.0);
    float receiverHeight = (receiver.z - base) / thickness;
    if (parameters.y <= 0.001 || sunDirection.z <= 0.01 || receiverHeight >= 1.0)
        return 0.0;
    vec3 positionMeters = vec3((receiver.xy + episodeOffset)
        / openmwseAtmosphereWorldUnitsPerMeter, 0.0).xzy;
    vec2 coordinates = cloudModelWeatherCoordinates(positionMeters, 0.0, model);
    vec3 weather = cloudModelRemapWeather(texture2D(weatherTexture, coordinates).rgb, model);
    vec4 gradient = cloudModelHeightGradient(weather, model);
    float height = (gradient.y + gradient.z) * 0.5;
    float distance = max(base + thickness * height - receiver.z, 0.0) / max(sunDirection.z, 0.08);
    positionMeters.xz += sunDirection.xy * distance / openmwseAtmosphereWorldUnitsPerMeter;
    coordinates = cloudModelWeatherCoordinates(positionMeters, height, model);
    weather = cloudModelRemapWeather(texture2D(weatherTexture, coordinates).rgb, model);
    gradient = cloudModelHeightGradient(weather, model);
    float remainingHeight = max(gradient.w - max(gradient.x, receiverHeight), 0.0);
    height = (max(gradient.y, receiverHeight) + gradient.z) * 0.5;
    float columnDensity = weather.r * cloudModelSampleAnvilDeformation(height, weather, model)
        * remainingHeight * parameters.y;
    return columnDensity * thickness * 0.000105 / max(sunDirection.z, 0.22);
}

#endif
