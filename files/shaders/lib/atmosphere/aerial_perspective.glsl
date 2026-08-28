#ifndef OPENMWSE_ATMOSPHERE_AERIAL_PERSPECTIVE_GLSL
#define OPENMWSE_ATMOSPHERE_AERIAL_PERSPECTIVE_GLSL

/// Carry canonical scene-linear atmosphere state into shared aerial-perspective equations.
struct OpenMWSEAerialPerspectiveParameters
{
    vec3 inScattering;
    vec3 outScattering;
    vec3 skyColor;
    vec3 fogColor;
    vec3 sunDirection;
    float weatherWeight;
    float fogStart;
    float fogEnd;
    float exponentialFog;
    float referenceHeight;
};

/// Convert world distance into the fork's MGE-style exponential fog optical distance.
float openmwseExponentialFogOpticalDistance(float distanceFromEye, float fogStart, float fogEnd)
{
    const float mgeFogScale = 4.4;
    float curveStart = fogStart / mgeFogScale;
    float curveRange = max((fogEnd - curveStart) / mgeFogScale, 1e-4);
    return max(0.0, (distanceFromEye - curveStart) / curveRange);
}

/// Convert world distance into bounded linear fog optical distance.
float openmwseLinearFogOpticalDistance(float distanceFromEye, float fogStart, float fogEnd)
{
    return clamp((distanceFromEye - fogStart) / max(fogEnd - fogStart, 1e-4), 0.0, 1.0);
}

/// Select the configured fog transfer function for a world-space distance.
float openmwseFogOpticalDistance(float distanceFromEye, OpenMWSEAerialPerspectiveParameters parameters)
{
    float linearDistance = openmwseLinearFogOpticalDistance(distanceFromEye, parameters.fogStart, parameters.fogEnd);
    float exponentialDistance
        = openmwseExponentialFogOpticalDistance(distanceFromEye, parameters.fogStart, parameters.fogEnd);
    return mix(linearDistance, exponentialDistance, step(0.5, parameters.exponentialFog));
}

/// Convert optical distance into direct scene-radiance visibility.
float openmwseFogVisibility(float opticalDistance, OpenMWSEAerialPerspectiveParameters parameters)
{
    float linearVisibility = 1.0 - opticalDistance;
    float exponentialVisibility = exp(-opticalDistance);
    return mix(linearVisibility, exponentialVisibility, step(0.5, parameters.exponentialFog));
}

/// Evaluate directional scene-linear in-scattering from canonical weather and solar state.
vec3 openmwseAtmosphereScatteredLight(vec3 worldDirection, float opticalDistance, float visibility,
    vec3 baseColor, OpenMWSEAerialPerspectiveParameters parameters)
{
    worldDirection = normalize(worldDirection);
    vec3 sunDirection = normalize(parameters.sunDirection);
    float sunCosine = dot(worldDirection, sunDirection);
    float sunHeight = sunDirection.z;

    float altitude = pow(max(1.0 + sunHeight, 1e-4), 10.0);
    float altitudeBrightness = clamp(1.0 - exp2(-1.9 * altitude), 0.0, 1.0);
    float horizonSun = clamp(exp(-4.0 * sunHeight), 0.0, 1.0) * clamp(altitude, 0.0, 1.0);
    float forwardScatter = (1.58 / max(1.24 - sunCosine, 0.02)) * horizonSun;
    float diffuseScatter = 1.0 - 0.09 * forwardScatter;

    float atmosphereDepth = 1.33 * exp(-2.0 * clamp(worldDirection.z, 0.0, 1.0));
    vec3 directionalScatter = mix(parameters.inScattering, parameters.outScattering, 0.5 * (1.0 + sunCosine));
    float altitudeScale = 2.8 + 4.3 / max(altitude, 1e-4);
    vec3 attenuation = atmosphereDepth * directionalScatter * (altitudeScale + 0.7 * forwardScatter);
    vec3 integratedAttenuation
        = (vec3(1.0) - exp(-opticalDistance * attenuation)) / max(attenuation, vec3(1e-4));

    vec3 scatteredColor = vec3(0.125 * forwardScatter)
        + mix(parameters.skyColor, vec3(0.371, 0.637, 1.108), 0.44) * diffuseScatter;
    scatteredColor *= integratedAttenuation * (1.17 * atmosphereDepth + 0.89) * altitudeBrightness;

    return mix(baseColor * (1.0 - visibility), scatteredColor, parameters.weatherWeight);
}

/// Apply aerial perspective while retaining a caller-selected maximum haze opacity.
vec3 openmwseApplyAerialPerspectiveWithOpticalLimit(vec3 localRadiance, vec3 worldDirection,
    float distanceFromEye, float opticalDistanceLimit, OpenMWSEAerialPerspectiveParameters parameters)
{
    float fogOpticalDistance = openmwseFogOpticalDistance(distanceFromEye, parameters);
    fogOpticalDistance = min(fogOpticalDistance, opticalDistanceLimit);
    float visibility = openmwseFogVisibility(fogOpticalDistance, parameters);
    vec3 scatteredLight = openmwseAtmosphereScatteredLight(worldDirection,
        clamp(0.224 * fogOpticalDistance, 0.0, 1.0), visibility, parameters.fogColor, parameters);
    return localRadiance * visibility + scatteredLight;
}

/// Apply the configured aerial perspective to local scene-linear radiance.
vec3 openmwseApplyAerialPerspective(
    vec3 localRadiance, vec3 worldDirection, float distanceFromEye, OpenMWSEAerialPerspectiveParameters parameters)
{
    return openmwseApplyAerialPerspectiveWithOpticalLimit(
        localRadiance, worldDirection, distanceFromEye, 1e20, parameters);
}

/// Blend local radiance toward a precomputed atmospheric target under bounded fog opacity.
vec3 openmwseApplyAerialPerspectiveTowardTarget(vec3 localRadiance, vec3 atmosphericTarget,
    float distanceFromEye, float opticalDistanceLimit, OpenMWSEAerialPerspectiveParameters parameters)
{
    float fogOpticalDistance = min(openmwseFogOpticalDistance(distanceFromEye, parameters), opticalDistanceLimit);
    float visibility = openmwseFogVisibility(fogOpticalDistance, parameters);
    return mix(atmosphericTarget, localRadiance, visibility);
}

/// Resolve incident sunlight without attenuating weather-colored surface radiance a second time.
vec3 openmwseIncidentSolarRadiance(vec3 weatherSolarRadiance, OpenMWSEAerialPerspectiveParameters parameters)
{
    vec3 sunDirection = normalize(parameters.sunDirection);
    float horizonVisibility = smoothstep(-0.04, 0.02, sunDirection.z);
    return weatherSolarRadiance * horizonVisibility;
}

/// Estimate upper-hemisphere irradiance from the same directional sky model rendered behind the clouds.
vec3 openmwseAtmosphereEnvironmentIrradiance(
    vec3 weatherSkyIrradiance, OpenMWSEAerialPerspectiveParameters parameters)
{
    vec3 sunDirection = normalize(parameters.sunDirection);
    vec2 sunAzimuth = sunDirection.xy;
    float azimuthLength = length(sunAzimuth);
    sunAzimuth = azimuthLength > 1e-4 ? sunAzimuth / azimuthLength : vec2(1.0, 0.0);
    vec3 sunwardSkyDirection = normalize(vec3(sunAzimuth, 0.35));
    vec3 zenithRadiance
        = openmwseAtmosphereScatteredLight(vec3(0.0, 0.0, 1.0), 1.0, 0.0, parameters.skyColor, parameters);
    vec3 sunwardRadiance
        = openmwseAtmosphereScatteredLight(sunwardSkyDirection, 1.0, 0.0, parameters.skyColor, parameters);
    vec3 directionalSkyIrradiance = zenithRadiance * 0.62 + sunwardRadiance * 0.38;
    return mix(weatherSkyIrradiance, directionalSkyIrradiance, 0.65 * clamp(parameters.weatherWeight, 0.0, 1.0));
}

#endif
