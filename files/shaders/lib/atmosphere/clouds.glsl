#ifndef OPENMWSE_ATMOSPHERE_CLOUDS_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUDS_GLSL

#include "cloud_field.glsl"

/// Evaluate the low-frequency cloud shape used where ray footprints cannot resolve billow detail.
float openmwseCloudBroadNoise(vec3 worldPosition)
{
    vec3 advectedPosition = openmwseCloudAdvectedPosition(worldPosition);
    float volumeShape = openmwseCloudValueNoise(advectedPosition * vec3(0.000018, 0.000018, 0.000036));
    float columnShape = openmwseCloudColumnNoise(advectedPosition.xy);
    return mix(volumeShape, columnShape, 0.28);
}

/// Evaluate multi-scale cloud shape while limiting detail to the caller's resolved footprint.
float openmwseCloudNoise(vec3 worldPosition, float billowDetailAmount)
{
    vec3 advectedPosition = openmwseCloudAdvectedPosition(worldPosition);
    float volumeShape = openmwseCloudValueNoise(advectedPosition * vec3(0.000018, 0.000018, 0.000036));
    float columnShape = openmwseCloudColumnNoise(advectedPosition.xy);
    float broadShape = mix(volumeShape, columnShape, 0.28);
    float mediumNoise = openmwseCloudValueNoise(
        advectedPosition * vec3(0.000071, 0.000071, 0.00012) + vec3(31.4, -18.6, 11.7));
    float mediumBillow = 1.0 - abs(mediumNoise * 2.0 - 1.0);
    float detailedShape = broadShape * 0.62 + mediumBillow * 0.38;
    if (billowDetailAmount > 0.01)
    {
        float fineNoise = openmwseCloudValueNoise(
            advectedPosition * vec3(0.00019, 0.00019, 0.00031) + vec3(-9.2, 44.1, -27.3));
        float fineBillow = 1.0 - abs(fineNoise * 2.0 - 1.0);
        detailedShape = detailedShape * 0.82 + fineBillow * 0.18;
    }
    return mix(broadShape, detailedShape, billowDetailAmount);
}

/// Return the locally varied cloud base height and thickness for a world-space column.
vec2 openmwseCloudLocalLayer(vec3 worldPosition)
{
    vec4 parameters = openmwseCloudParameters();
    float nominalBaseHeight = openmwseCloudWaterHeight() + parameters.z;
    float nominalThickness = max(parameters.w, 1.0);
    vec2 weatherPosition = openmwseCloudAdvectedPosition(worldPosition).xy * 0.000008;
    float baseShape = openmwseCloudValueNoise(weatherPosition + vec2(13.7, -29.1));
    float verticalDevelopment = openmwseCloudValueNoise(weatherPosition * 1.73 + vec2(-37.6, 8.4));
    float localBaseHeight
        = nominalBaseHeight + (baseShape * 2.0 - 1.0) * nominalThickness * openmwseCloudBaseVariation;
    float developedThickness = mix(openmwseCloudMinimumThicknessScale, openmwseCloudMaximumThicknessScale,
        smoothstep(0.12, 0.88, baseShape * 0.58 + verticalDevelopment * 0.42));
    return vec2(localBaseHeight, nominalThickness * developedThickness);
}

/// Return the forward ray interval intersecting the conservative cloud-height bounds.
vec2 openmwseCloudLayerIntersection(vec3 origin, vec3 direction, float lowerHeight, float upperHeight)
{
    if (abs(direction.z) < 0.0001)
        return origin.z >= lowerHeight && origin.z <= upperHeight ? vec2(0.0, openmwseCloudMaximumDistance)
                                                                  : vec2(1.0, 0.0);

    float first = (lowerHeight - origin.z) / direction.z;
    float second = (upperHeight - origin.z) / direction.z;
    return vec2(max(0.0, min(first, second)), min(openmwseCloudMaximumDistance, max(first, second)));
}

/// Shape normalized cloud noise by local vertical development and weather coverage.
float openmwseCloudDensityFromNoise(vec3 worldPosition, float cloudNoise)
{
    vec4 parameters = openmwseCloudParameters();
    vec2 localLayer = openmwseCloudLocalLayer(worldPosition);
    float relativeHeight = clamp((worldPosition.z - localLayer.x) / localLayer.y, 0.0, 1.0);
    float verticalDevelopment = cloudNoise - 0.5;
    float lowerShape = smoothstep(0.0, 0.12, relativeHeight + verticalDevelopment * 0.16);
    float upperShape = 1.0 - smoothstep(0.58, 1.0, relativeHeight - verticalDevelopment * 0.24);
    float heightShape = lowerShape * upperShape;
    float threshold = mix(0.86, 0.36, clamp(parameters.x, 0.0, 1.0));
    float lowerBoundaryErosion = (1.0 - smoothstep(0.0, 0.18, relativeHeight)) * 0.16;
    float upperBoundaryErosion = smoothstep(0.52, 1.0, relativeHeight) * 0.18;
    float densityField = cloudNoise - threshold - lowerBoundaryErosion - upperBoundaryErosion;
    float shapedNoise = smoothstep(0.0, max(0.22 * (1.0 - threshold), 0.04), densityField);
    return shapedNoise * heightShape * max(parameters.y, 0.0);
}

/// Evaluate cloud density with high-frequency billows filtered beyond their spatial sampling footprint.
float openmwseCloudDensity(vec3 worldPosition, float distanceFromEye, float sampleFootprint)
{
    float distanceDetail = 1.0 - smoothstep(180000.0, 500000.0, distanceFromEye);
    float footprintDetail = 1.0 - smoothstep(900.0, 5000.0, sampleFootprint);
    float billowDetailAmount = distanceDetail * footprintDetail;
    return openmwseCloudDensityFromNoise(worldPosition, openmwseCloudNoise(worldPosition, billowDetailAmount));
}

/// Evaluate cloud density from only the stable low-frequency field.
float openmwseCloudBroadDensity(vec3 worldPosition)
{
    return openmwseCloudDensityFromNoise(worldPosition, openmwseCloudBroadNoise(worldPosition));
}

/// Evaluate a normalized Henyey-Greenstein phase lobe for cloud droplets.
float openmwseCloudMiePhase(float cosineAngle, float anisotropy)
{
    float denominator = 1.0 + anisotropy * anisotropy - 2.0 * anisotropy * cosineAngle;
    return (1.0 - anisotropy * anisotropy) / max(12.5663706 * pow(denominator, 1.5), 0.001);
}

/// Approximate cloud multiple scattering with progressively wider phase lobes.
float openmwseCloudMultipleScatteringPhase(float cosineAngle)
{
    float primaryScattering = openmwseCloudMiePhase(cosineAngle, 0.68);
    float secondaryScattering = openmwseCloudMiePhase(cosineAngle, 0.34);
    float tertiaryScattering = openmwseCloudMiePhase(cosineAngle, 0.17);
    return primaryScattering * 0.72 + secondaryScattering * 0.22 + tertiaryScattering * 0.06;
}

/// Approximate direct-sun access through the remaining broad cloud column.
float openmwseCloudSunAccess(vec3 samplePosition, vec3 sunDirection)
{
    vec2 localLayer = openmwseCloudLocalLayer(samplePosition);
    float layerTop = localLayer.x + localLayer.y;
    float distanceToTop = max(layerTop - samplePosition.z, 0.0) / max(sunDirection.z, 0.12);
    vec3 opticalDepthPosition = samplePosition + sunDirection * (distanceToTop * 0.5);
    float opticalDepth = openmwseCloudBroadDensity(opticalDepthPosition) * distanceToTop * 0.00015;
    return exp(-opticalDepth);
}

/// Integrate premultiplied cloud radiance and transmittance along a bounded world-space ray.
vec4 openmwseIntegrateCloudsInternal(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere, float rayDither, bool quadratureSampling,
    bool useAtmosphericTarget)
{
    vec4 parameters = openmwseCloudParameters();
    if (parameters.x <= 0.001 || worldDirection.z <= -0.01)
        return vec4(0.0, 0.0, 0.0, 1.0);

    float nominalBaseHeight = openmwseCloudWaterHeight() + parameters.z;
    float nominalThickness = max(parameters.w, 1.0);
    float minimumCloudHeight = nominalBaseHeight - nominalThickness * openmwseCloudBaseVariation;
    float maximumCloudHeight
        = nominalBaseHeight + nominalThickness * (openmwseCloudBaseVariation + openmwseCloudMaximumThicknessScale);
    vec2 interval = openmwseCloudLayerIntersection(rayOrigin, worldDirection, minimumCloudHeight, maximumCloudHeight);
    interval.y = min(interval.y, maximumDistance);
    if (interval.y <= interval.x)
        return vec4(0.0, 0.0, 0.0, 1.0);

    const int maximumSampleCount = 50;
    int sampleCount = 20 + quality * 10;
    if (sampleCount > maximumSampleCount)
        sampleCount = maximumSampleCount;
    float intervalLength = interval.y - interval.x;
    float samplesThroughNominalLayer = 10.0 + float(quality) * 5.0;
    float stepLength = max(nominalThickness / samplesThroughNominalLayer, intervalLength / float(sampleCount));
    float distanceAlongRay = interval.x;
    float transmittance = 1.0;
    vec3 scatteredLight = vec3(0.0);
    float representativeDistanceSum = 0.0;
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    float phase = 0.16 + 0.84 * openmwseCloudMultipleScatteringPhase(dot(worldDirection, sunDirection));
    vec3 incidentSolarRadiance = openmwseIncidentSolarRadiance(solarRadiance, atmosphere);
    vec3 directLight = incidentSolarRadiance * (0.35 + 3.2 * phase);
    vec3 viewSkyRadiance
        = openmwseAtmosphereScatteredLight(worldDirection, 1.0, 0.0, atmosphere.skyColor, atmosphere);
    vec3 environmentIrradiance = max(openmwseAtmosphereEnvironmentIrradiance(skyIrradiance, atmosphere),
        max(viewSkyRadiance * 1.05, atmosphere.skyColor * 0.9));
    environmentIrradiance += incidentSolarRadiance * 0.12;
    vec3 lowerHemisphereIrradiance = max(groundIrradiance, environmentIrradiance * 0.62);

    for (int sampleIndex = 0; sampleIndex < maximumSampleCount; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
            break;
        float segmentLength = min(stepLength, interval.y - distanceAlongRay);
        if (segmentLength <= 0.0)
            break;
        float nextDistanceAlongRay = distanceAlongRay + segmentLength;
        float sampleOffset = quadratureSampling ? 0.5 : fract(rayDither + float(sampleIndex) * 0.61803398875);
        float sampleDistance = distanceAlongRay + segmentLength * sampleOffset;
        vec3 samplePosition = rayOrigin + worldDirection * sampleDistance;
        float density;
        if (quadratureSampling)
        {
            const float firstQuadratureOffset = 0.2113248654;
            const float secondQuadratureOffset = 0.7886751346;
            float firstDistance = distanceAlongRay + segmentLength * firstQuadratureOffset;
            float secondDistance = distanceAlongRay + segmentLength * secondQuadratureOffset;
            density = 0.5
                * (openmwseCloudDensity(
                       rayOrigin + worldDirection * firstDistance, firstDistance, 0.0)
                    + openmwseCloudDensity(
                        rayOrigin + worldDirection * secondDistance, secondDistance, 0.0));
        }
        else
            density = openmwseCloudDensity(samplePosition, sampleDistance, segmentLength);
        if (density > 0.001)
        {
            float sunAccess = openmwseCloudSunAccess(samplePosition, sunDirection);
            float extinction = density * segmentLength * 0.0002;
            float sampleOpacity = 1.0 - exp(-extinction);
            vec2 localLayer = openmwseCloudLocalLayer(samplePosition);
            float relativeHeight = clamp((samplePosition.z - localLayer.x) / localLayer.y, 0.0, 1.0);
            vec3 ambientLight
                = mix(lowerHemisphereIrradiance, environmentIrradiance, smoothstep(0.08, 0.92, relativeHeight));
            float ambientAccess = mix(0.85, 1.1, smoothstep(0.04, 0.96, relativeHeight));
            float multipleScatteringRecovery = (1.0 - sunAccess) * (0.38 + 0.3 * (1.0 - density));
            vec3 sampleLight
                = ambientLight * ambientAccess + directLight * (sunAccess * 0.85 + multipleScatteringRecovery);
            float contribution = transmittance * sampleOpacity;
            scatteredLight += contribution * sampleLight;
            representativeDistanceSum += contribution * sampleDistance;
            transmittance *= 1.0 - sampleOpacity;
            if (transmittance < 0.015)
                break;
        }
        distanceAlongRay = nextDistanceAlongRay;
    }

    float opacity = 1.0 - transmittance;
    if (opacity > 1e-5)
    {
        float representativeDistance = representativeDistanceSum / opacity;
        vec3 meanRadiance = scatteredLight / opacity;
        // High cloud silhouettes remain perceptible after the denser ground-level haze has reached full opacity.
        vec2 mainCloudOpticalLimits = vec2(0.88, 2.12026354);
        vec2 reflectedCloudOpticalLimits = vec2(0.72, 1.27296568);
        vec2 cloudOpticalLimits = useAtmosphericTarget ? reflectedCloudOpticalLimits : mainCloudOpticalLimits;
        float cloudOpticalDistanceLimit
            = mix(cloudOpticalLimits.x, cloudOpticalLimits.y, step(0.5, atmosphere.exponentialFog));
        vec3 cloudAtmosphericTarget = useAtmosphericTarget ? atmosphericTarget : viewSkyRadiance;
        vec3 atmosphericRadiance = openmwseApplyAerialPerspectiveTowardTarget(
            meanRadiance, cloudAtmosphericTarget, representativeDistance, cloudOpticalDistanceLimit, atmosphere);
        scatteredLight = atmosphericRadiance * opacity;
    }
    return vec4(scatteredLight, transmittance);
}

/// Integrate clouds with one deterministic or temporal sample per segment, or deterministic quadrature when requested.
vec4 openmwseIntegrateClouds(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance,
    OpenMWSEAerialPerspectiveParameters atmosphere, float rayDither, bool quadratureSampling)
{
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, vec3(0.0), atmosphere, rayDither, quadratureSampling, false);
}

/// Integrate deterministic reflected clouds without frame-dependent sampling noise.
vec4 openmwseIntegrateCloudsStable(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, atmosphericTarget, atmosphere, 0.5, true, true);
}

#endif
