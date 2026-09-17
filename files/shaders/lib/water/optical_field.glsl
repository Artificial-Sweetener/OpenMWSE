#if defined(WATER_OPTICAL_NORMAL_VOLUME)
uniform sampler3D waterOpticalNormalMap;
#else
uniform sampler2D waterOpticalNormalMap;
#endif
uniform sampler3D waterOpticalVolume;

/// Sample the OpenMW optical normal through the binding selected by the consuming pass.
vec3 sampleWaterOpticalNormalMap(vec2 coordinates)
{
#if defined(WATER_OPTICAL_NORMAL_VOLUME)
    return texture3D(waterOpticalNormalMap, vec3(coordinates, 0.5)).rgb;
#else
    return texture2D(waterOpticalNormalMap, coordinates).rgb;
#endif
}

/// Sample the shared MGE XE volume in a stage-appropriate way.
vec4 sampleWaterOpticalVolume(vec3 coordinates)
{
#if defined(WATER_OPTICAL_VERTEX_STAGE)
    return texture3DLod(waterOpticalVolume, coordinates, 0.0);
#else
    return texture3D(waterOpticalVolume, coordinates);
#endif
}

struct WaterOpticalSample
{
    vec3 normal;
    vec3 scatterNormal;
    vec3 shoreBaseNormal;
    vec3 shoreLargeNormal;
    vec3 shoreSmallNormal;
};

/// Return one OpenMW normal-map coordinate with the authored inter-layer choppiness feedback.
vec2 waterOpticalNormalCoords(vec2 uv, float scale, float speed, float time,
    float timer1, float timer2, vec3 previousNormal)
{
    const vec2 windDirection = vec2(0.5, -0.8);
    return uv * (75.0 * scale) + windDirection * time * (0.2 * speed)
        - (previousNormal.xy / previousNormal.zz) * 0.05 + vec2(time * timer1, time * timer2);
}

/// Evaluate the authored geometric height field before receiver-specific blocker clipping.
float sampleWaterGeometricHeight(vec2 worldPosition, float time, int appearance, float cameraDistance,
    float waveHeight, float windSpeed, float fogStart, float fogEnd)
{
    if (waveHeight <= 0.0)
        return 0.0;

    float closeScale = appearance == 2 ? 860.0 : 1104.0;
    float farScale = appearance == 2 ? 2800.0 : 3900.0;
    float volumeTime = (appearance == 2 ? 0.325 : 0.4) * time;
    float closeHeight = sampleWaterOpticalVolume(vec3(worldPosition / closeScale, volumeTime)).a;
    float farHeight = sampleWaterOpticalVolume(vec3(worldPosition / farScale, volumeTime)).a;
    float displacement = waveHeight
        * (mix(closeHeight, farHeight, clamp(cameraDistance / 8000.0, 0.0, 1.0)) - 0.5)
        * clamp(1.0 - cameraDistance / 6400.0, 0.0, 1.0)
        * clamp(cameraDistance / 200.0, 0.0, 1.0);
    if (appearance == 2)
    {
        float windFactor = (abs(windSpeed) + 1.5) / 120.0;
        displacement += (0.4 + 30.0 * windFactor) * mix(closeHeight, farHeight, waveHeight);
        float fogFade = clamp((fogEnd - cameraDistance * 2.0) / max(fogEnd - fogStart, 0.0001), 0.0, 1.0);
        displacement = mix(-1.0, displacement, fogFade);
        float swell = sin(-worldPosition.x / 256.0 + 1.75 * time);
        swell += sin(-2.5 * worldPosition.x / 256.0 + 2.2 * time);
        swell += sin(-3.0 * worldPosition.x / 256.0 + 1.4 * time);
        swell += sin(-4.5 * worldPosition.x / 256.0 + 3.0 * time);
        displacement += (2.5 - 60.0 * windFactor) * waveHeight * swell / 32.0;
        displacement = 0.5 * displacement - 12.5;
    }
    return displacement;
}

/// Evaluate the same animated optical normal field used by the selected water appearance.
WaterOpticalSample sampleWaterOpticalField(vec2 worldPosition, float time, int appearance, vec2 observerPosition,
    bool enhancedDistantWaves, vec3 rippleAdd, float rainAmount)
{
    WaterOpticalSample result;
    result.scatterNormal = vec3(0.0, 0.0, 1.0);
    result.shoreBaseNormal = vec3(0.0);
    result.shoreLargeNormal = vec3(0.0);
    result.shoreSmallNormal = vec3(0.0);

    if (appearance == 0)
    {
        vec2 uv = worldPosition / (8192.0 * 5.0) * 3.0;
        vec3 normal0 = 2.0 * sampleWaterOpticalNormalMap(
            waterOpticalNormalCoords(uv, 0.05, 0.04, time, -0.015, -0.005, vec3(0.0))) - 1.0;
        vec3 normal1 = 2.0 * sampleWaterOpticalNormalMap(
            waterOpticalNormalCoords(uv, 0.1, 0.08, time, 0.02, 0.015, normal0)) - 1.0;
        vec3 normal2 = 2.0 * sampleWaterOpticalNormalMap(
            waterOpticalNormalCoords(uv, 0.25, 0.07, time, -0.04, -0.03, normal1)) - 1.0;
        vec3 normal3 = 2.0 * sampleWaterOpticalNormalMap(
            waterOpticalNormalCoords(uv, 0.5, 0.09, time, 0.03, 0.04, normal2)) - 1.0;
        vec3 normal4 = 2.0 * sampleWaterOpticalNormalMap(
            waterOpticalNormalCoords(uv, 1.0, 0.4, time, -0.02, 0.1, normal3)) - 1.0;
        vec3 normal5 = 2.0 * sampleWaterOpticalNormalMap(
            waterOpticalNormalCoords(uv, 2.0, 0.7, time, 0.1, -0.06, normal4)) - 1.0;
        vec2 middleWaves = mix(vec2(0.1), vec2(0.2), rainAmount);
        vec2 smallWaves = mix(vec2(0.1), vec2(0.3), rainAmount);
        float bump = mix(0.5, 2.5, rainAmount);
        vec3 combined = normal0 * 0.1 + normal1 * 0.1 + normal2 * middleWaves.x
            + normal3 * middleWaves.y + normal4 * smallWaves.x + normal5 * smallWaves.y + rippleAdd;
        vec3 scatterCombined = normal0 * 0.05 + normal1 * 0.05 + normal2 * middleWaves.x * 0.2
            + normal3 * middleWaves.y * 0.2 + normal4 * smallWaves.x * 0.1
            + normal5 * smallWaves.y * 0.1 + rippleAdd;
        result.normal = normalize(vec3(-combined.x * bump, -combined.y * bump, combined.z));
        result.scatterNormal
            = normalize(vec3(-scatterCombined.x * bump, -scatterCombined.y * bump, scatterCombined.z));
        result.shoreBaseNormal = normal2;
        result.shoreLargeNormal = normal3;
        result.shoreSmallNormal = normal4;
        return result;
    }

    if (appearance == 2)
    {
        float volumeTime = time * 0.315;
        vec3 farCoords = vec3(worldPosition / 2600.0 * 0.5, volumeTime * 0.5);
        vec3 normal0 = sampleWaterOpticalVolume(farCoords * 3.0).rgb;
        vec3 normal1 = sampleWaterOpticalVolume(farCoords).rgb;
        vec3 normal2 = sampleWaterOpticalVolume(vec3(worldPosition / 440.0, volumeTime)).rgb;
        vec3 normal3 = sampleWaterOpticalVolume(vec3(worldPosition / 2600.0, volumeTime * 0.6)).rgb;
        if (enhancedDistantWaves)
        {
            float radialPhase = mod(length(worldPosition - vec2(20000.0, 40000.0)), 6144.0)
                / (6144.0 / 6.283185307) + time;
            normal0.xy += vec2(sin(radialPhase) / 16.0);
        }
        vec2 farNormal = 0.5 * (normal0.xy + normal1.xy);
        vec2 closeNormal = 0.5 * (normal2.xy + normal3.xy) + 0.5 * rippleAdd.xy;
        vec2 encodedNormal = mix(closeNormal, farNormal,
            clamp(length(worldPosition - observerPosition) / 11000.0, 0.0, 1.0));
        result.normal = normalize(vec3(2.0 * encodedNormal - 1.0, 0.295));
        result.scatterNormal = result.normal;
        return result;
    }

    float coordinateMultiplier = appearance == 3 ? 4.0 : 1.0;
    float volumeTime = time * 0.4;
    vec3 farNormal = sampleWaterOpticalVolume(
        vec3(worldPosition / 3900.0 * coordinateMultiplier, volumeTime)).rgb;
    vec3 closeNormal = sampleWaterOpticalVolume(
        vec3(worldPosition / 527.0 * coordinateMultiplier, volumeTime)).rgb;
    vec2 encodedNormal = mix(closeNormal.xy + 0.5 * rippleAdd.xy, farNormal.xy,
        clamp(length(worldPosition - observerPosition) / 8000.0, 0.0, 1.0));
    result.normal = normalize(vec3(2.0 * encodedNormal - 1.0, 1.0));
    result.scatterNormal = result.normal;
    return result;
}

/// Evaluate the active optical field with the fewest authored layers that preserve its caustic scale and motion.
vec3 sampleWaterCausticOpticalNormal(vec2 worldPosition, float time, int appearance, vec2 observerPosition,
    bool enhancedDistantWaves, float rainAmount)
{
    if (appearance != 2)
    {
        return sampleWaterOpticalField(worldPosition, time, appearance, observerPosition,
            enhancedDistantWaves, vec3(0.0), rainAmount).normal;
    }

    float volumeTime = time * 0.315;
    vec2 fineNormal = sampleWaterOpticalVolume(vec3(worldPosition / 440.0, volumeTime)).rg;
    vec2 broadNormal = sampleWaterOpticalVolume(vec3(worldPosition / 2600.0, volumeTime * 0.6)).rg;
    if (enhancedDistantWaves)
    {
        float radialPhase = mod(length(worldPosition - vec2(20000.0, 40000.0)), 6144.0)
            / (6144.0 / 6.283185307) + time;
        broadNormal.x += sin(radialPhase) / 16.0;
    }
    vec2 encodedNormal = 0.5 * (fineNormal + broadNormal);
    return normalize(vec3(2.0 * encodedNormal - 1.0, 0.295));
}

/// Sample OpenMW's rain-sensitive shore breakup from intermediates already evaluated for the surface normal.
float sampleWaterOpenMWShoreNoise(
    vec2 worldPosition, float time, vec3 shoreLargeNormal, vec3 shoreSmallNormal)
{
    vec2 uv = worldPosition / (8192.0 * 5.0) * 3.0;
    vec3 first = sampleWaterOpticalNormalMap(
        waterOpticalNormalCoords(uv, 2.0, 2.7, -time, 0.05, 0.1, shoreLargeNormal)) - 0.5;
    vec3 second = sampleWaterOpticalNormalMap(
        waterOpticalNormalCoords(uv, 2.0, 2.7, time, 0.04, -0.13, shoreSmallNormal)) - 0.5;
    return first.r + second.r;
}
