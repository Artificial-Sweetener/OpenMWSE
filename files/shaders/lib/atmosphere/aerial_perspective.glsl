#ifndef OPENMWSE_ATMOSPHERE_AERIAL_PERSPECTIVE_GLSL
#define OPENMWSE_ATMOSPHERE_AERIAL_PERSPECTIVE_GLSL

#include "units.glsl"

/// Carry canonical scene-linear atmosphere state into shared aerial-perspective equations.
struct OpenMWSEAerialPerspectiveParameters
{
    bool scatteringEnabled;
    int appearance;
    vec3 inScattering;
    vec3 outScattering;
    vec3 skyColor;
    vec3 fogColor;
    vec3 sunDirection;
    vec3 solarRadiance;
    vec3 skyIrradiance;
    vec3 groundIrradiance;
    float weatherWeight;
    float fogStart;
    float fogEnd;
    float exponentialFog;
    float referenceHeight;
    float observerHeight;
    float weatherCloudTopHeight;
    float weatherCloudCoverage;
};

/// Approximate near-horizon astronomical refraction using NOAA's piecewise solar correction.
float openmwseAtmosphericRefraction(float geometricElevation)
{
    float elevationDegrees = degrees(geometricElevation);
    float correctionDegrees = 0.0;
    if (elevationDegrees > 5.0 && elevationDegrees < 85.0)
    {
        float tangent = tan(geometricElevation);
        correctionDegrees
            = (58.1 / tangent - 0.07 / (tangent * tangent * tangent)
                + 0.000086 / pow(tangent, 5.0)) / 3600.0;
    }
    else if (elevationDegrees > -0.575 && elevationDegrees <= 5.0)
    {
        correctionDegrees = (1735.0 - 518.2 * elevationDegrees + 103.4 * elevationDegrees * elevationDegrees
            - 12.79 * pow(elevationDegrees, 3.0) + 0.711 * pow(elevationDegrees, 4.0)) / 3600.0;
    }
    else if (elevationDegrees <= -0.575 && elevationDegrees > -6.0)
        correctionDegrees = (-20.774 / tan(geometricElevation)) / 3600.0;
    return radians(clamp(correctionDegrees, 0.0, 0.85));
}

/// Return the refracted apparent solar direction while retaining the physical azimuth.
vec3 openmwseApparentSunDirection(vec3 geometricDirection)
{
    geometricDirection = normalize(geometricDirection);
    float geometricElevation = asin(clamp(geometricDirection.z, -1.0, 1.0));
    float apparentElevation = geometricElevation + openmwseAtmosphericRefraction(geometricElevation);
    vec2 azimuth = geometricDirection.xy;
    float azimuthLength = length(azimuth);
    azimuth = azimuthLength > 0.0001 ? azimuth / azimuthLength : vec2(1.0, 0.0);
    return normalize(vec3(azimuth * cos(apparentElevation), sin(apparentElevation)));
}

/// Invert astronomical refraction so a screen ray can be tested against the geometric solar disc.
vec3 openmwseGeometricDirectionFromApparent(vec3 apparentDirection)
{
    apparentDirection = normalize(apparentDirection);
    float apparentElevation = asin(clamp(apparentDirection.z, -1.0, 1.0));
    float geometricElevation = apparentElevation;
    for (int iteration = 0; iteration < 2; ++iteration)
        geometricElevation = apparentElevation - openmwseAtmosphericRefraction(geometricElevation);
    vec2 azimuth = apparentDirection.xy;
    float azimuthLength = length(azimuth);
    azimuth = azimuthLength > 0.0001 ? azimuth / azimuthLength : vec2(1.0, 0.0);
    return normalize(vec3(azimuth * cos(geometricElevation), sin(geometricElevation)));
}

/// Approximate the slant optical path through the lower atmosphere, including twilight rays above the observer.
float openmwseSolarRelativeAirMass(float solarElevation)
{
    float elevationDegrees = degrees(solarElevation);
    float kastenYoungDenominator = sin(solarElevation)
        + 0.50572 * pow(max(elevationDegrees + 6.07995, 0.05), -1.6364);
    float daylightAirMass = clamp(1.0 / max(kastenYoungDenominator, 0.035), 1.0, 30.0);
    float twilightAirMass = mix(12.0, 30.0, smoothstep(-18.0, -0.5, elevationDegrees));
    return mix(twilightAirMass, daylightAirMass, smoothstep(-0.75, 0.0, elevationDegrees));
}

/// Evaluate the normalized molecular scattering phase function.
float openmwseRayleighPhase(float cosineAngle)
{
    return 0.0596831 * (1.0 + cosineAngle * cosineAngle);
}

/// Evaluate a normalized aerosol phase lobe with bounded forward anisotropy.
float openmwseAtmosphereMiePhase(float cosineAngle)
{
    const float anisotropy = 0.76;
    float anisotropySquared = anisotropy * anisotropy;
    float denominator = max(1.0 + anisotropySquared - 2.0 * anisotropy * cosineAngle, 0.0025);
    return 0.0795775 * (1.0 - anisotropySquared) / pow(denominator, 1.5);
}

/// Give the horizon glow a smooth crest while preserving the vertical energy of its authored falloff.
float openmwseAtmosphereHorizonGlow(float verticalDirection, float falloff)
{
    const float halfSqrtPi = 0.886226925;
    float scaledHeight = verticalDirection * falloff * halfSqrtPi;
    return exp(-scaledHeight * scaledHeight);
}

/// Evaluate low-sun single and bounded multiple scattering from canonical atmosphere illumination.
vec3 openmwseLowSunScattering(vec3 worldDirection, float sunCosine, float solarElevation,
    float opticalDistance, OpenMWSEAerialPerspectiveParameters parameters)
{
    const vec3 rayleighExtinction = vec3(0.0085, 0.0210, 0.0520);
    const vec3 aerosolExtinction = vec3(0.0105, 0.0120, 0.0150);
    const vec3 ozoneExtinction = vec3(0.0025, 0.0115, 0.0035);
    vec3 totalExtinction = rayleighExtinction + aerosolExtinction + ozoneExtinction;
    float relativeAirMass = openmwseSolarRelativeAirMass(solarElevation);
    vec3 solarTransmittance = exp(-relativeAirMass * totalExtinction);

    float civilTwilight = smoothstep(radians(-6.5), radians(0.5), solarElevation);
    float nauticalTwilight = smoothstep(radians(-12.0), radians(-4.0), solarElevation)
        * (1.0 - smoothstep(radians(-0.5), radians(2.0), solarElevation));
    float astronomicalTwilight = smoothstep(radians(-18.0), radians(-10.0), solarElevation)
        * (1.0 - smoothstep(radians(-5.0), radians(-2.0), solarElevation));
    float illuminatedAtmosphere = max(civilTwilight, max(0.55 * nauticalTwilight, 0.2 * astronomicalTwilight));

    float horizonDensity = openmwseAtmosphereHorizonGlow(worldDirection.z, 6.5);
    float viewDepth = 1.0 + 2.4 * horizonDensity;
    float molecularPhase = openmwseRayleighPhase(sunCosine);
    float aerosolPhase = openmwseAtmosphereMiePhase(sunCosine);
    vec3 singleScattering = parameters.solarRadiance * solarTransmittance
        * (18.0 * rayleighExtinction * molecularPhase + 4.0 * aerosolExtinction * aerosolPhase)
        * viewDepth * illuminatedAtmosphere;

    vec3 removedSolarRadiance = parameters.solarRadiance * (vec3(1.0) - solarTransmittance);
    vec3 multipleScattering = mix(parameters.skyIrradiance, removedSolarRadiance, 0.38)
        * (0.055 + 0.13 * horizonDensity) * illuminatedAtmosphere;

    float antiSolar = pow(clamp(0.5 - 0.5 * sunCosine, 0.0, 1.0), 2.0);
    float postSunset = smoothstep(radians(-10.0), radians(-1.0), solarElevation)
        * (1.0 - smoothstep(radians(-0.5), radians(1.5), solarElevation));
    float venusAltitude = exp(-pow((worldDirection.z - 0.12) / 0.105, 2.0));
    vec3 ozoneFilteredSolar = parameters.solarRadiance * exp(-relativeAirMass * ozoneExtinction * 2.2);
    vec3 venusBelt = mix(parameters.skyIrradiance, ozoneFilteredSolar, 0.58)
        * antiSolar * venusAltitude * postSunset * 0.18;

    float groundHorizon = 1.0 - smoothstep(0.0, 0.42, max(worldDirection.z, 0.0));
    vec3 groundBounce = parameters.groundIrradiance * groundHorizon * civilTwilight * 0.035;
    float pathWeight = clamp(opticalDistance, 0.0, 1.0);
    return (singleScattering + multipleScattering + venusBelt + groundBounce) * pathWeight;
}

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

/// Integrate a constant-below-datum, exponential-above-datum density profile along a straight view segment.
vec2 openmwseAtmosphereMeanLayerDensities(float startHeight, float endHeight)
{
    float lowHeight = min(startHeight, endHeight);
    float highHeight = max(startHeight, endHeight);
    if (highHeight <= 0.0)
        return vec2(1.0);
    const vec2 scaleHeights = vec2(8000.0, 1200.0) * openmwseAtmosphereWorldUnitsPerMeter;
    float heightSpan = highHeight - lowHeight;
    vec2 lowerDensity = exp(-max(lowHeight, 0.0) / scaleHeights);
    vec2 aboveSpan = (highHeight - max(lowHeight, 0.0)) / scaleHeights;
    // The Taylor limit avoids cancellation for horizontal and nearly horizontal rays.
    vec2 integral = vec2(1.0) - aboveSpan * 0.5 + aboveSpan * aboveSpan / 6.0;
    if (max(aboveSpan.x, aboveSpan.y) > 0.01)
        integral = (vec2(1.0) - exp(-aboveSpan)) / max(aboveSpan, vec2(1e-6));
    float belowFraction = clamp(-lowHeight / max(heightSpan, 1e-6), 0.0, 1.0);
    return mix(lowerDensity * integral, vec2(1.0), belowFraction);
}

/// Integrate the configured view-path atmosphere while preserving its reference-height fog transfer.
float openmwseFogOpticalDistance(float distanceFromEye, vec3 worldDirection,
    OpenMWSEAerialPerspectiveParameters parameters)
{
    if (!parameters.scatteringEnabled || parameters.appearance == 1)
        return parameters.exponentialFog > 0.5
            ? openmwseExponentialFogOpticalDistance(distanceFromEye, parameters.fogStart, parameters.fogEnd)
            : openmwseLinearFogOpticalDistance(distanceFromEye, parameters.fogStart, parameters.fogEnd);
    float exponential = step(0.5, parameters.exponentialFog);
    float curveStart = parameters.fogStart / mix(1.0, 4.4, exponential);
    float curveRange = max((parameters.fogEnd - curveStart) / mix(1.0, 4.4, exponential), 1e-4);
    float activeDistance = max(distanceFromEye - curveStart, 0.0);
    if (activeDistance <= 0.0)
        return 0.0;
    float relativeObserverHeight = parameters.observerHeight - parameters.referenceHeight;
    vec2 meanDensities = openmwseAtmosphereMeanLayerDensities(
        relativeObserverHeight + worldDirection.z * curveStart,
        relativeObserverHeight + worldDirection.z * distanceFromEye);
    // Retain the canonical scalar extinction at the reference height. Molecular green-band extinction
    // takes its physical share; weather visibility supplies the remaining lower-atmosphere aerosol.
    const float molecularExtinction = 0.013558 / (1000.0 * openmwseAtmosphereWorldUnitsPerMeter);
    float molecularFraction = clamp(molecularExtinction * curveRange, 0.0, 1.0);
    float meanDensity = mix(meanDensities.y, meanDensities.x, molecularFraction);
    float opticalDistance = activeDistance * meanDensity / curveRange;
    return mix(min(opticalDistance, 1.0), opticalDistance, exponential);
}

/// Convert optical distance into direct scene-radiance visibility.
float openmwseFogVisibility(float opticalDistance, OpenMWSEAerialPerspectiveParameters parameters)
{
    float linearVisibility = 1.0 - opticalDistance;
    float exponentialVisibility = exp(-opticalDistance);
    return mix(linearVisibility, exponentialVisibility, step(0.5, parameters.exponentialFog));
}

/// Evaluate the original MGE-style color response or its native twilight and highlight refinements.
vec3 openmwseAtmosphereScatteredLight(vec3 worldDirection, float opticalDistance, float visibility,
    vec3 baseColor, OpenMWSEAerialPerspectiveParameters parameters)
{
    if (!parameters.scatteringEnabled)
        return baseColor * (1.0 - visibility);
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

    vec3 diffuseSkyColor = mix(parameters.skyColor, vec3(0.371, 0.637, 1.108), 0.44);
    vec3 scatteredColor = vec3(0.125 * forwardScatter) + diffuseSkyColor * diffuseScatter;
    scatteredColor *= integratedAttenuation * (1.17 * atmosphereDepth + 0.89) * altitudeBrightness;
    // Preserve the original parity response from 15ee5e5d84 before the native color refinements.
    if (parameters.appearance == 1)
        return mix(baseColor * (1.0 - visibility), scatteredColor, parameters.weatherWeight);
    if (sunHeight < 0.34)
    {
        // Long horizontal paths redden the horizon; upper twilight retains the diffuse sky chromaticity.
        // Preserve the integrated MGE brightness and zero-distance limit while recovering that color.
        const vec3 luminanceWeights = vec3(0.2126, 0.7152, 0.0722);
        float upperElevation = max(worldDirection.z, 0.0);
        float upperSkyWeight = (1.0 - exp(-20.0 * upperElevation * upperElevation))
            * (1.0 - smoothstep(0.08, 0.34, sunHeight));
        vec3 diffuseChromaticity = diffuseSkyColor / max(dot(diffuseSkyColor, luminanceWeights), 1e-4);
        scatteredColor = mix(scatteredColor,
            diffuseChromaticity * dot(scatteredColor, luminanceWeights), upperSkyWeight);

        float solarElevation = asin(clamp(sunHeight, -1.0, 1.0));
        float relativeAirMass = openmwseSolarRelativeAirMass(solarElevation);
        vec3 longPathTransmittance = exp(-relativeAirMass * vec3(0.018, 0.041, 0.095));
        float twilight = smoothstep(-0.18, 0.02, sunHeight) * (1.0 - smoothstep(0.06, 0.34, sunHeight));
        float horizonBand = openmwseAtmosphereHorizonGlow(worldDirection.z, 7.5);
        float solarAfterglow = pow(clamp(0.5 + 0.5 * sunCosine, 0.0, 1.0), 3.0);
        float antiSolarArch = pow(clamp(0.5 - 0.5 * sunCosine, 0.0, 1.0), 2.0);
        vec3 sunsetRadiance
            = mix(vec3(1.22, 0.24, 0.055), vec3(1.0, 0.62, 0.24), longPathTransmittance) * solarAfterglow;
        vec3 twilightArch = vec3(0.22, 0.12, 0.34) * antiSolarArch;
        scatteredColor += (sunsetRadiance + twilightArch) * twilight * horizonBand
            * clamp(opticalDistance, 0.0, 1.0);

        float physicalLowSunWeight = 1.0 - smoothstep(0.02, 0.07, sunHeight);
        if (physicalLowSunWeight > 0.0)
            scatteredColor += openmwseLowSunScattering(
                worldDirection, sunCosine, solarElevation, opticalDistance, parameters) * physicalLowSunWeight;
    }

    // Roll bright channels smoothly into scene-color headroom instead of clipping the additive glow.
    // Dim haze keeps its MGE response, and the solar disc retains its separate radiometric range.
    vec3 excess = max(scatteredColor - vec3(0.8), vec3(0.0));
    scatteredColor = min(scatteredColor, vec3(0.8)) + 0.2 * excess / (vec3(0.2) + excess);
    return mix(baseColor * (1.0 - visibility), scatteredColor, parameters.weatherWeight);
}

/// Return the clear-air recovery visible above the weather-cloud layer.
float openmwseClearAirRecovery(OpenMWSEAerialPerspectiveParameters parameters)
{
    float cloudLayerDepth = max(parameters.weatherCloudTopHeight - parameters.referenceHeight, 1.0);
    float altitudeBlendWidth = clamp(cloudLayerDepth * 0.12, 1800.0, 9000.0);
    float aboveClouds = smoothstep(parameters.weatherCloudTopHeight - altitudeBlendWidth * 0.2,
        parameters.weatherCloudTopHeight + altitudeBlendWidth, parameters.observerHeight);
    float cloudWeatherInfluence = smoothstep(0.18, 0.82, parameters.weatherCloudCoverage);
    float daylight = smoothstep(-0.04, 0.18, normalize(parameters.sunDirection).z);
    return aboveClouds * cloudWeatherInfluence * daylight;
}

/// Recover top-of-cloud solar radiance from weather-darkened ground lighting without changing its chromaticity.
vec3 openmwseClearAirSolarRadiance(
    vec3 weatherSolarRadiance, OpenMWSEAerialPerspectiveParameters parameters)
{
    const vec3 luminanceWeights = vec3(0.2126, 0.7152, 0.0722);
    float recovery = openmwseClearAirRecovery(parameters);
    float weatherLuminance = max(dot(weatherSolarRadiance, luminanceWeights), 0.001);
    float daylight = smoothstep(-0.08, 0.28, normalize(parameters.sunDirection).z);
    float clearLuminance = max(weatherLuminance, mix(weatherLuminance, 0.95, daylight));
    return mix(weatherSolarRadiance,
        weatherSolarRadiance * (clearLuminance / weatherLuminance), recovery);
}

/// Recover upper-hemisphere irradiance above an optically thick weather deck from the canonical sky state.
vec3 openmwseClearAirSkyIrradiance(
    vec3 weatherSkyIrradiance, OpenMWSEAerialPerspectiveParameters parameters)
{
    const vec3 luminanceWeights = vec3(0.2126, 0.7152, 0.0722);
    float recovery = openmwseClearAirRecovery(parameters);
    float weatherLuminance = max(dot(weatherSkyIrradiance, luminanceWeights), 0.001);
    float daylight = smoothstep(-0.08, 0.28, normalize(parameters.sunDirection).z);
    float clearLuminance = max(weatherLuminance, mix(0.035, 0.34, daylight));
    return mix(weatherSkyIrradiance,
        weatherSkyIrradiance * (clearLuminance / weatherLuminance), recovery);
}

/// Recover molecular clear-air chromaticity without introducing a second authored sky palette.
vec3 openmwseClearAirSkyColor(vec3 weatherSkyColor, float skyAmount)
{
    const vec3 luminanceWeights = vec3(0.2126, 0.7152, 0.0722);
    const vec3 molecularZenithChromaticity = vec3(0.371, 0.637, 1.108);
    float weatherLuminance = max(dot(weatherSkyColor, luminanceWeights), 0.001);
    vec3 normalizedZenith = molecularZenithChromaticity
        / dot(molecularZenithChromaticity, luminanceWeights);
    vec3 normalizedHorizon = mix(vec3(1.0), normalizedZenith, 0.38);
    vec3 clearChromaticity = mix(normalizedHorizon, normalizedZenith, skyAmount);
    return clearChromaticity * weatherLuminance * mix(1.08, 1.2, skyAmount);
}

/// Evaluate the canonical directional sky radiance that also owns the fog-colored horizon.
vec3 openmwseAtmosphereSkyRadiance(
    vec3 worldDirection, OpenMWSEAerialPerspectiveParameters parameters)
{
    worldDirection = normalize(worldDirection);
    float skyAmount = 1.0 - pow(
        clamp(1.0 - 2.22 * clamp(worldDirection.z - 0.075, 0.0, 1.0), 0.0, 1.0), 1.15);
    vec3 directionalSky = mix(parameters.fogColor, parameters.skyColor, skyAmount);
    if (!parameters.scatteringEnabled)
        return directionalSky;
    if (parameters.appearance == 1)
        return openmwseAtmosphereScatteredLight(worldDirection, 1.0, 0.0, directionalSky, parameters);
    float clearAirRecovery = openmwseClearAirRecovery(parameters);
    parameters.solarRadiance = openmwseClearAirSolarRadiance(parameters.solarRadiance, parameters);
    parameters.skyIrradiance = openmwseClearAirSkyIrradiance(parameters.skyIrradiance, parameters);
    vec3 clearAirSky = openmwseClearAirSkyColor(directionalSky, skyAmount);
    directionalSky = mix(directionalSky, clearAirSky, clearAirRecovery);
    parameters.fogColor = mix(parameters.fogColor,
        openmwseClearAirSkyColor(parameters.fogColor, 0.0), clearAirRecovery);
    parameters.skyColor = mix(parameters.skyColor,
        openmwseClearAirSkyColor(parameters.skyColor, 1.0), clearAirRecovery);
    return openmwseAtmosphereScatteredLight(
        worldDirection, 1.0, 0.0, directionalSky, parameters);
}

/// Apply aerial perspective while retaining a caller-selected maximum haze opacity.
vec3 openmwseApplyAerialPerspectiveWithOpticalLimit(vec3 localRadiance, vec3 worldDirection,
    float distanceFromEye, float opticalDistanceLimit, OpenMWSEAerialPerspectiveParameters parameters)
{
    float fogOpticalDistance = openmwseFogOpticalDistance(distanceFromEye, worldDirection, parameters);
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

/// Blend local radiance toward a precomputed atmospheric target with a caller-owned distance compression.
vec3 openmwseApplyAerialPerspectiveTowardTargetWithScale(vec3 localRadiance, vec3 atmosphericTarget,
    vec3 worldDirection, float distanceFromEye, float opticalDistanceScale, float opticalDistanceLimit,
    OpenMWSEAerialPerspectiveParameters parameters)
{
    float fogOpticalDistance = min(
        openmwseFogOpticalDistance(distanceFromEye, worldDirection, parameters) * opticalDistanceScale,
        opticalDistanceLimit);
    float visibility = openmwseFogVisibility(fogOpticalDistance, parameters);
    return mix(atmosphericTarget, localRadiance, visibility);
}

/// Blend local radiance toward a precomputed atmospheric target under bounded fog opacity.
vec3 openmwseApplyAerialPerspectiveTowardTarget(vec3 localRadiance, vec3 atmosphericTarget,
    vec3 worldDirection, float distanceFromEye, float opticalDistanceLimit, OpenMWSEAerialPerspectiveParameters parameters)
{
    return openmwseApplyAerialPerspectiveTowardTargetWithScale(
        localRadiance, atmosphericTarget, worldDirection, distanceFromEye, 1.0, opticalDistanceLimit, parameters);
}

/// Resolve incident sunlight without attenuating weather-colored surface radiance a second time.
vec3 openmwseIncidentSolarRadiance(vec3 weatherSolarRadiance, OpenMWSEAerialPerspectiveParameters parameters)
{
    vec3 sunDirection = normalize(parameters.sunDirection);
    vec3 apparentSunDirection = openmwseApparentSunDirection(sunDirection);
    float horizonVisibility = smoothstep(-0.012, 0.018, apparentSunDirection.z);
    float relativeAirMass = openmwseSolarRelativeAirMass(asin(clamp(sunDirection.z, -1.0, 1.0)));
    vec3 spectralTransmittance = exp(-(relativeAirMass - 1.0) * vec3(0.008, 0.021, 0.052));
    return weatherSolarRadiance * spectralTransmittance * horizonVisibility;
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
        = openmwseAtmosphereSkyRadiance(vec3(0.0, 0.0, 1.0), parameters);
    vec3 sunwardRadiance
        = openmwseAtmosphereSkyRadiance(sunwardSkyDirection, parameters);
    vec3 directionalSkyIrradiance = zenithRadiance * 0.62 + sunwardRadiance * 0.38;
    return mix(weatherSkyIrradiance, directionalSkyIrradiance, 0.65 * clamp(parameters.weatherWeight, 0.0, 1.0));
}

#endif
