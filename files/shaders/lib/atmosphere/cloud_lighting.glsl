#ifndef OPENMWSE_ATMOSPHERE_CLOUD_LIGHTING_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_LIGHTING_GLSL

#include "cloud_density.glsl"
#include "cloud_shadow_column.h.glsl"
#include "participating_media.glsl"

/// Evaluate a normalized Henyey-Greenstein phase lobe for cloud droplets.
float openmwseCloudMiePhase(float cosineAngle, float anisotropy)
{
    float denominator = 1.0 + anisotropy * anisotropy - 2.0 * anisotropy * cosineAngle;
    return (1.0 - anisotropy * anisotropy) / max(12.5663706 * pow(denominator, 1.5), 0.001);
}

/// Evaluate the forward/backward dual-lobe droplet phase function.
float openmwseCloudDualLobePhase(float cosineAngle)
{
    return mix(openmwseCloudMiePhase(cosineAngle, 0.5),
        openmwseCloudMiePhase(cosineAngle, -0.5), 0.2);
}

/// Retain nonlinear sun samples while rejecting wholly empty terrain-dependent paths before sampling.
vec3 openmwseCloudSunOpticalDepth(vec3 samplePosition, vec3 sunDirection, int quality,
    OpenMWSECloudLayer layer)
{
    if (!openmwseCloudLayerEnabled(layer))
        return vec3(0.0);
    float lightPathLength = 3000.0 * layer.model.physicalScale
        * openmwseAtmosphereWorldUnitsPerMeter;
    if (layer.terrainWeight > 0.0)
    {
        vec2 heightBounds = openmwseCloudLayerHeightBounds(layer);
        // Density normalizes thickness to at least one world unit, including authored sub-unit layers.
        heightBounds.y += max(1.0 - layer.parameters.w, 0.0);
        float lastHeight = samplePosition.z + sunDirection.z * lightPathLength;
        float padding = max(1.0, max(abs(samplePosition.z), abs(lastHeight)) * 0.00001);
        // All fixed light samples lie between these endpoints; the terrain envelope covers the entire field.
        if (max(samplePosition.z, lastHeight) < heightBounds.x - padding
            || min(samplePosition.z, lastHeight) > heightBounds.y + padding)
            return vec3(0.0);
    }
    float distanceFromEye = length(samplePosition - openmwseCloudEyePosition());

    int shadowSampleCount = quality >= 2 ? 5 : quality >= 1 ? 4 : 3;
    vec3 opticalDepth = vec3(0.0);
    const int maximumShadowSampleCount = 5;
    for (int sampleIndex = 0; sampleIndex < maximumShadowSampleCount; ++sampleIndex)
    {
        if (sampleIndex >= shadowSampleCount)
            break;
        float intervalStart = float(sampleIndex) / float(shadowSampleCount);
        float intervalEnd = float(sampleIndex + 1) / float(shadowSampleCount);
        intervalStart *= intervalStart;
        intervalEnd *= intervalEnd;
        float intervalFraction = intervalEnd - intervalStart;
        float sampleFraction = intervalStart + intervalFraction * 0.5;
        // Keep the source offsets fixed even when later samples leave the layer. Renormalizing the
        // lattice to the shell exit changes self-shadowing as a cloud surface crosses the light ray.
        float sampleDistance = lightPathLength * sampleFraction;
        vec3 shadowPosition = samplePosition + sunDirection * sampleDistance;
        // Empty samples do not change the spatial footprint of later light samples.
        float shadowLodBias = 0.5 * float(sampleIndex + 1);
        float shadowDensity = openmwseCloudDensity(
            shadowPosition, distanceFromEye, shadowLodBias, layer);
        opticalDepth += clamp(shadowDensity * layer.model.extinctionPerMeter, 0.0, 1.0)
            * (lightPathLength * intervalFraction / openmwseAtmosphereWorldUnitsPerMeter);
    }
    return opticalDepth;
}

/// Retain independent RGB solar attenuation for both scattering orders.
struct OpenMWSECloudSolarTransmittance
{
    vec3 primary;
    vec3 secondary;
};

/// Return source spectral solar attenuation for the first and reduced-extinction scattering orders.
OpenMWSECloudSolarTransmittance openmwseCloudSunTransmittance(vec3 samplePosition, vec3 sunDirection, int quality,
    OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer incomingLayer, OpenMWSECloudLayer lowLayer)
{
    vec3 opticalDepth = openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, mainLayer);
    opticalDepth += openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, incomingLayer);
    opticalDepth += openmwseCloudSunOpticalDepth(
        samplePosition, sunDirection, quality, lowLayer);
    return OpenMWSECloudSolarTransmittance(exp(-opticalDepth),
        exp(-opticalDepth * openmwseCloudSecondaryExtinctionScale));
}

/// Evaluate the shared broad shadow column at the cloud stratum's physical local placement.
float openmwsePlacedCloudColumnOpticalDepth(vec3 position, vec3 sunDirection, OpenMWSECloudLayer layer)
{
    if (!openmwseCloudLayerEnabled(layer))
        return 0.0;
    vec4 parameters = layer.parameters;
    parameters.z = openmwseCloudNominalBaseHeight(position, layer) - openmwseCloudWaterHeight();
    return openmwseCloudShadowColumnOpticalDepth(position, sunDirection, parameters,
        openmwseCloudWaterHeight(), layer.fieldOffsetWorldUnits, layer.model, openmwseCloudWeatherTexture);
}

/// Light broad mist and fog with the same weather-column shadow approximation used by scene surfaces.
float openmwseLocalMediaSunTransmittance(vec3 position, vec3 sunDirection,
    OpenMWSECloudLayer mainLayer, OpenMWSECloudLayer incomingLayer, OpenMWSECloudLayer lowLayer)
{
    float opticalDepth = openmwsePlacedCloudColumnOpticalDepth(position, sunDirection, mainLayer);
    opticalDepth += openmwsePlacedCloudColumnOpticalDepth(position, sunDirection, incomingLayer);
    opticalDepth += openmwsePlacedCloudColumnOpticalDepth(position, sunDirection, lowLayer);
    return exp(-opticalDepth);
}

/// Carry direct irradiance and angle-integrated diffuse radiance into participating-media transport.
struct OpenMWSEVolumeIllumination
{
    vec3 solarIrradiance;
    vec3 diffuseRadiance;
};

/// Convert canonical surface-light units to normalized-phase cloud lighting above the weather deck.
OpenMWSEVolumeIllumination openmwseCloudIllumination(vec3 solarRadiance, vec3 skyIrradiance,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    atmosphere.observerHeight = atmosphere.weatherCloudTopHeight + 9000.0;
    vec3 clearAirSolarRadiance = openmwseClearAirSolarRadiance(solarRadiance, atmosphere);
    vec3 clearAirSkyIrradiance = openmwseClearAirSkyIrradiance(skyIrradiance, atmosphere);
    vec3 incidentSolarRadiance = openmwseIncidentSolarRadiance(clearAirSolarRadiance, atmosphere);
    vec3 skyRadianceMean = openmwseAtmosphereEnvironmentIrradiance(clearAirSkyIrradiance, atmosphere);
    // Native unit-white Lambert lighting is L = sunColor * NdotL, so its incident irradiance is pi * sunColor.
    // The normalized phase consumes irradiance; the sky LUT includes the isotropic angular integral.
    const float lambertRadianceToIrradiance = 3.14159265;
    const float hemisphereIsotropicWeight = 0.5; // 2*pi steradians times 1/(4*pi).
    return OpenMWSEVolumeIllumination(incidentSolarRadiance * lambertRadianceToIrradiance,
        skyRadianceMean * hemisphereIsotropicWeight);
}

/// Average the rendered upper hemisphere with Gaussian elevation quadrature and four fixed azimuths.
vec3 openmwseLocalSkyRadiance(OpenMWSEAerialPerspectiveParameters atmosphere)
{
    vec3 radiance = vec3(0.0);
    for (int elevation = 0; elevation < 2; ++elevation)
    {
        float cosineZenith = elevation == 0 ? openmwseCloudQuadratureOffsetLow : openmwseCloudQuadratureOffsetHigh;
        float horizontalLength = sqrt(1.0 - cosineZenith * cosineZenith);
        vec2 azimuth = vec2(1.0, 0.0);
        for (int direction = 0; direction < 4; ++direction)
        {
            radiance += openmwseAtmosphereSkyRadiance(vec3(azimuth * horizontalLength, cosineZenith), atmosphere);
            azimuth = vec2(-azimuth.y, azimuth.x);
        }
    }
    return radiance * 0.125;
}

/// Illuminate nearby participating media from the visible sky and ground at their physical altitude.
OpenMWSEVolumeIllumination openmwseLocalVolumeIllumination(vec3 solarRadiance,
    vec3 groundIrradiance, float altitude, OpenMWSEAerialPerspectiveParameters atmosphere)
{
    atmosphere.observerHeight = altitude;
    return OpenMWSEVolumeIllumination(
        3.14159265 * openmwseIncidentSolarRadiance(solarRadiance, atmosphere),
        0.5 * (groundIrradiance + openmwseLocalSkyRadiance(atmosphere)));
}

/// Blend premultiplied segment radiance using the shared observer-path fog visibility.
vec3 openmwseCloudSegmentAtmosphericContribution(vec3 scatteredLight, float opacity,
    vec3 atmosphericTarget, float visibility)
{
    return mix(atmosphericTarget * opacity, scatteredLight, visibility);
}

/// Evaluate spectral scattering per metre with weather opacity applied once after the episode's powder response.
vec3 openmwseWeatherCloudScattering(float density, float precipitation, float weatherWeight,
    OpenMWSECloudLayer layer)
{
    float episodeDensity = density / max(weatherWeight, 1e-6);
    return density * layer.model.extinctionPerMeter
        * cloudModelSampleAlbedo(episodeDensity, precipitation, layer.model);
}

#endif
