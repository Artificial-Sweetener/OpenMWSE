#ifndef OPENMWSE_ATMOSPHERE_CLOUD_RAYMARCH_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_RAYMARCH_GLSL

#include "cloud_lighting.glsl"

/// Preserve distance-based step allocation at the selected physical feature scale.
float openmwseCloudMarchStepLength(float intervalLength, float physicalScale, int quality)
{
    float allocationDistance = 15000.0 * physicalScale
        * openmwseAtmosphereWorldUnitsPerMeter;
    float maximumSteps = float(48 + quality * 16);
    // OpenMW also has terrain condensate much thinner than the kilometre-scale shell. Retain
    // one eighth of the quality budget in short intersections to resolve their shallow density support.
    return min(intervalLength / (maximumSteps * 0.125), max(intervalLength, allocationDistance) / maximumSteps);
}

/// Keep condensate quadrature on fixed world planes when the observer moves along a ray.
float openmwseCondensateLatticeSegmentLength(vec3 origin, vec3 direction,
    float distanceAlongRay, float stepLength)
{
    float phase = fract((dot(origin, direction) + distanceAlongRay) / stepLength);
    float remaining = (1.0 - phase) * stepLength;
    // Treat a numerically completed lattice plane as a full step instead of stalling on its boundary.
    return remaining > stepLength * 1e-4 ? remaining : stepLength;
}

/// Finish a clipped interval at its explicit endpoint so rounded cumulative distances cannot stall or replay it.
float openmwseAdvanceCloudMarch(float distanceAlongIntervals, float stepLength,
    float remainingIntervalLength, float intervalEndDistance)
{
    return stepLength >= remainingIntervalLength ? intervalEndDistance : distanceAlongIntervals + stepLength;
}

/// March each cloud interval with one shared sample budget and interleaved local-medium transport.
vec4 openmwseIntegrateCloudsInternal(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, OpenMWSEVolumeIllumination lowIllumination, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere,
    bool useAtmosphericTarget, OpenMWSELocalMedia localMedia,
    out float visibleDistanceSum, out float visibleCloudOpacity)
{
    visibleDistanceSum = 0.0;
    visibleCloudOpacity = 0.0;
    OpenMWSECloudLayer mainLayer = openmwseMainCloudLayer();
    OpenMWSECloudLayer incomingLayer = openmwseIncomingCloudLayer();
    OpenMWSECloudLayer lowLayer = openmwseLowCloudLayer();
    bool outgoingLayerEnabled = openmwseCloudLayerEnabled(mainLayer);
    bool incomingLayerEnabled = openmwseCloudLayerEnabled(incomingLayer);
    bool mainLayerEnabled = outgoingLayerEnabled || incomingLayerEnabled;
    bool lowLayerEnabled = openmwseCloudLayerEnabled(lowLayer);
    if (!mainLayerEnabled && !lowLayerEnabled)
        return openmwseIntegrateLocalMedia(localMedia, maximumDistance, visibleDistanceSum);

    vec2 mainInterval = vec2(1.0, 0.0);
    vec2 lowInterval = vec2(1.0, 0.0);
    vec2 lowHeightBounds = vec2(1e20, -1e20);
    if (mainLayerEnabled)
    {
        vec2 mainDensityBounds = outgoingLayerEnabled
            ? openmwseCloudDensityHeightBounds(mainLayer) : openmwseCloudDensityHeightBounds(incomingLayer);
        if (incomingLayerEnabled && outgoingLayerEnabled)
        {
            vec2 incomingDensityBounds = openmwseCloudDensityHeightBounds(incomingLayer);
            mainDensityBounds = vec2(min(mainDensityBounds.x, incomingDensityBounds.x),
                max(mainDensityBounds.y, incomingDensityBounds.y));
        }
        mainInterval = openmwseMainCloudLayerIntersection(
            rayOrigin, worldDirection, mainDensityBounds.x, mainDensityBounds.y);
        mainInterval.y = min(mainInterval.y, maximumDistance);
    }
    if (lowLayerEnabled)
    {
        lowHeightBounds = openmwseCloudDensityHeightBounds(lowLayer);
        lowInterval = openmwseCloudLayerIntersection(
            rayOrigin, worldDirection, lowHeightBounds.x, lowHeightBounds.y);
        lowInterval.y = min(lowInterval.y, maximumDistance);
        lowInterval.y = min(lowInterval.y,
            lowInterval.x + openmwseCloudLayerMaximumMarchingDistance(lowLayer));
    }
    bool mainIntervalValid = mainInterval.y > mainInterval.x;
    bool lowIntervalValid = lowInterval.y > lowInterval.x;
    if (!mainIntervalValid && !lowIntervalValid)
        return openmwseIntegrateLocalMedia(localMedia, maximumDistance, visibleDistanceSum);

    vec2 lowRayHeightBounds = lowIntervalValid
        ? openmwseLowCloudRayDensityHeightBounds(rayOrigin, worldDirection, lowInterval, lowLayer)
        : vec2(1e20, -1e20);

    vec2 firstInterval = mainIntervalValid ? mainInterval : lowInterval;
    vec2 secondInterval = vec2(1.0, 0.0);
    bool firstIntervalIsMain = mainIntervalValid;
    bool separatedIntervals = false;
    if (mainIntervalValid && lowIntervalValid)
    {
        firstIntervalIsMain = mainInterval.x <= lowInterval.x;
        firstInterval = firstIntervalIsMain ? mainInterval : lowInterval;
        secondInterval = firstIntervalIsMain ? lowInterval : mainInterval;
        if (secondInterval.x <= firstInterval.y)
            firstInterval.y = max(firstInterval.y, secondInterval.y);
        else
            separatedIntervals = true;
    }

    // Reserve each stratum's transport subdivisions and the split-boundary segments.
    const int maximumSampleCount = 96 * (openmwseSkyCloudTransportSubdivisions + openmwseCondensateTransportSubdivisions) + 8;
    float firstIntervalLength = min(firstInterval.y - firstInterval.x, openmwseCloudMaximumMarchingDistance);
    float secondIntervalLength = separatedIntervals
        ? min(secondInterval.y - secondInterval.x, openmwseCloudMaximumMarchingDistance - firstIntervalLength) : 0.0;
    float traversedIntervalLength = firstIntervalLength + secondIntervalLength;
    float mainPhysicalScale = min(outgoingLayerEnabled ? mainLayer.model.physicalScale : 1e20,
        incomingLayerEnabled ? incomingLayer.model.physicalScale : 1e20);
    float firstPhysicalScale = firstIntervalIsMain ? mainPhysicalScale : lowLayer.model.physicalScale;
    if (!separatedIntervals && mainIntervalValid && lowIntervalValid)
        firstPhysicalScale = min(mainPhysicalScale, lowLayer.model.physicalScale);
    float firstFineStepLength = openmwseCloudMarchStepLength(firstIntervalLength, firstPhysicalScale, quality);
    float secondFineStepLength = separatedIntervals
        ? openmwseCloudMarchStepLength(secondIntervalLength,
            firstIntervalIsMain ? lowLayer.model.physicalScale : mainPhysicalScale, quality)
        : firstFineStepLength;
    // Independent strata retain the source allocation without spending samples in the empty gap.
    // Continuous lengths prevent allocation-rounding contours as a ray crosses the layer horizon.
    float distanceAlongCloudIntervals = 0.0;
    vec3 transmittance = vec3(1.0);
    float localCursor = 0.0;
    vec3 localScatteredLight = vec3(0.0);
    float localOpacity = 0.0;
    float localDistanceSum = 0.0;
    vec3 mainScatteredLight = vec3(0.0);
    vec3 lowScatteredLight = vec3(0.0);
    float mainOpacity = 0.0;
    float lowOpacity = 0.0;
    float mainRepresentativeDistanceSum = 0.0;
    float lowRepresentativeDistanceSum = 0.0;
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    // The HG denominator uses -2*g*cosine; the equivalent +2*g*cosine form negates its input.
    // Both conventions must place the forward-scattering peak on view rays directed toward the sun.
    float primaryPhase = openmwseCloudDualLobePhase(dot(worldDirection, sunDirection));
    float secondaryPhase = mix(0.07957747, primaryPhase, 0.2);
    OpenMWSEVolumeIllumination illumination = openmwseCloudIllumination(solarRadiance, skyIrradiance, atmosphere);
    // Incident light is evaluated above the deck, but the cloud-to-eye path belongs to the real observer.
    vec3 viewSkyRadiance = openmwseAtmosphereSkyRadiance(worldDirection, atmosphere);
    float incomingWeatherWeight = clamp(openmwseCloudScenarioParameters().z, 0.0, 1.0);

    vec3 cloudAtmosphericTarget = useAtmosphericTarget ? atmosphericTarget : viewSkyRadiance;
    vec2 mainCloudOpticalLimits = useAtmosphericTarget ? vec2(0.72, 1.27296568) : vec2(0.88, 2.12026354);
    float mainCloudOpticalDistanceLimit
        = mix(mainCloudOpticalLimits.x, mainCloudOpticalLimits.y, step(0.5, atmosphere.exponentialFog));
    float mainCloudMinimumVisibility = openmwseFogVisibility(mainCloudOpticalDistanceLimit, atmosphere);
    int sampleIndex = 0;
    for (int intervalIndex = 0; intervalIndex < 2; ++intervalIndex)
    {
        if (sampleIndex >= maximumSampleCount || (intervalIndex == 1 && !separatedIntervals))
            break;
        bool sampleSecondInterval = intervalIndex == 1;
        vec2 activeInterval = sampleSecondInterval ? secondInterval : firstInterval;
        float activeIntervalLength = sampleSecondInterval ? secondIntervalLength : firstIntervalLength;
        float activeIntervalEnd = sampleSecondInterval ? traversedIntervalLength : firstIntervalLength;
        float fineStepLength = sampleSecondInterval ? secondFineStepLength : firstFineStepLength;
        for (; sampleIndex < maximumSampleCount; ++sampleIndex)
        {
            if (distanceAlongCloudIntervals >= activeIntervalEnd)
                break;
            float intervalPathOffset
                = sampleSecondInterval ? distanceAlongCloudIntervals - firstIntervalLength : distanceAlongCloudIntervals;
            float remainingIntervalLength = activeIntervalLength - intervalPathOffset;
            if (remainingIntervalLength <= 0.0)
            {
                distanceAlongCloudIntervals = activeIntervalEnd;
                continue;
            }
            float segmentLength = min(fineStepLength, remainingIntervalLength);
            float distanceAlongRay = activeInterval.x + intervalPathOffset;
            if (mainIntervalValid && distanceAlongRay < mainInterval.y)
            {
                if (distanceAlongRay < mainInterval.x)
                    segmentLength = min(segmentLength, mainInterval.x - distanceAlongRay);
                else
                    segmentLength = min(segmentLength, min(fineStepLength / float(openmwseSkyCloudTransportSubdivisions),
                        mainInterval.y - distanceAlongRay));
            }
            // Beer-powder albedo and sunlight must resolve the same shallow features as extinction.
            // Averaging fine density into a coarse lighting segment makes radiance change with its boundaries.
            if (lowIntervalValid && distanceAlongRay < lowInterval.y)
            {
                if (distanceAlongRay < lowInterval.x)
                    segmentLength = min(segmentLength, lowInterval.x - distanceAlongRay);
                else
                {
                    float lowStepLength = fineStepLength / float(openmwseCondensateTransportSubdivisions);
                    float latticeSegmentLength = openmwseCondensateLatticeSegmentLength(
                        rayOrigin, worldDirection, distanceAlongRay, lowStepLength);
                    segmentLength = min(min(latticeSegmentLength, segmentLength), lowInterval.y - distanceAlongRay);
                }
            }
            // Integrate the same segment in ordinary, repaired, and reflected rays. Changing the
            // abscissa between refreshes changes nearby cloud opacity even when the field is frozen.
            float sampleDistance = distanceAlongRay + segmentLength * 0.5;
            vec3 samplePosition = rayOrigin + worldDirection * sampleDistance;
            float sampleHeightRadius
                = abs(worldDirection.z) * segmentLength * (0.5 - openmwseCondensateQuadratureOffsets.x);
            // The main interval can continue beyond the low march limit into terrain outside this ray bound.
            bool boundedLowSegment = lowIntervalValid && distanceAlongRay >= lowInterval.x
                && distanceAlongRay + segmentLength <= lowInterval.y;
            vec2 sampleLowHeightBounds = boundedLowSegment
                ? lowRayHeightBounds : lowHeightBounds + vec2(-1.0, 1.0);
            bool sampleLowLayer = lowLayerEnabled
                && samplePosition.z + sampleHeightRadius >= sampleLowHeightBounds.x
                && samplePosition.z - sampleHeightRadius <= sampleLowHeightBounds.y;
            openmwseAdvanceLocalMedia(localMedia, sampleDistance, localCursor, transmittance,
                localScatteredLight, localOpacity, localDistanceSum);
            float mainDensity = 0.0;
            float outgoingDensityForHeight = 0.0;
            float incomingDensityForHeight = 0.0;
            float lowDensity = 0.0;
            float outgoingPrecipitation = 0.0;
            float incomingPrecipitation = 0.0;
            float lowPrecipitation = 0.0;
            if (outgoingLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudSegmentDensity(
                    rayOrigin, worldDirection, distanceAlongRay, segmentLength, mainLayer);
                mainDensity = sample.density;
                outgoingDensityForHeight = mainDensity;
                outgoingPrecipitation = sample.precipitation;
            }
            if (incomingLayerEnabled)
            {
                OpenMWSECloudDensitySample sample = openmwseCloudSegmentDensity(
                    rayOrigin, worldDirection, distanceAlongRay, segmentLength, incomingLayer);
                float incomingDensity = sample.density;
                incomingPrecipitation = sample.precipitation;
                incomingDensityForHeight = incomingDensity;
                mainDensity += incomingDensity;
            }
            if (sampleLowLayer)
            {
                OpenMWSECloudDensitySample sample = openmwseCondensateSegmentDensity(
                    rayOrigin, worldDirection, distanceAlongRay, segmentLength, lowLayer);
                lowDensity = sample.density;
                lowPrecipitation = sample.precipitation;
            }
            float mainPresentationWeight = openmwseMainCloudPresentationWeight(sampleDistance);
            mainDensity *= mainPresentationWeight;
            outgoingDensityForHeight *= mainPresentationWeight;
            incomingDensityForHeight *= mainPresentationWeight;
            float density = mainDensity + lowDensity;
            vec3 mainExtinction = outgoingDensityForHeight * mainLayer.model.extinctionPerMeter
                + incomingDensityForHeight * incomingLayer.model.extinctionPerMeter;
            vec3 lowExtinction = lowDensity * lowLayer.model.extinctionPerMeter;
            vec3 combinedExtinction = mainExtinction + lowExtinction;
            vec3 extinction = clamp(combinedExtinction, 0.0, 1.0)
                * (segmentLength / openmwseAtmosphereWorldUnitsPerMeter);
            // Reject negligible optical depth, not density: long segments of thin condensate remain visible.
            if (any(greaterThan(extinction, vec3(1e-6))))
            {
                float relativeHeight = 0.0;
                if (mainDensity > 0.0)
                {
                    float mainFieldHeight = openmwseCloudVirtualAltitude(samplePosition);
                    if (outgoingDensityForHeight > 0.0)
                    {
                        vec2 mainHeightRange = openmwseCloudLocalLayer(samplePosition, mainLayer);
                        relativeHeight += outgoingDensityForHeight
                            * clamp((mainFieldHeight - mainHeightRange.x) / mainHeightRange.y, 0.0, 1.0);
                    }
                    if (incomingDensityForHeight > 0.0)
                    {
                        vec2 incomingHeightRange = openmwseCloudLocalLayer(samplePosition, incomingLayer);
                        relativeHeight += incomingDensityForHeight
                            * clamp((mainFieldHeight - incomingHeightRange.x) / incomingHeightRange.y, 0.0, 1.0);
                    }
                }
                // Relative height lights only the main deck; condensate uses its own diffuse illumination.
                if (lowDensity > 0.0 && mainDensity > 0.0)
                {
                    vec2 lowHeightRange = openmwseCloudLocalLayer(samplePosition, lowLayer);
                    relativeHeight += lowDensity
                        * clamp((samplePosition.z - lowHeightRange.x) / lowHeightRange.y, 0.0, 1.0);
                }
                relativeHeight /= density;
                OpenMWSECloudSolarTransmittance sunTransmittance = openmwseCloudSunTransmittance(
                    samplePosition, sunDirection, quality, mainLayer, incomingLayer, lowLayer);
                vec3 sampleTransmittance = exp(-extinction);
                vec3 mainScattering = openmwseWeatherCloudScattering(
                    outgoingDensityForHeight, outgoingPrecipitation, 1.0 - incomingWeatherWeight, mainLayer)
                    + openmwseWeatherCloudScattering(
                        incomingDensityForHeight, incomingPrecipitation, incomingWeatherWeight, incomingLayer);
                vec3 cloudAlbedo = clamp(mainScattering / max(mainExtinction, vec3(1e-7)), 0.0, 1.0);
                // Extinction already accounts for droplet concentration. Applying the source's density-driven
                // powder response to thin condensate would make its single-scattering albedo approach black.
                vec3 lowAlbedo = lowLayer.model.albedo * (1.0 - lowPrecipitation);
                float topEnvironmentAccess = clamp(0.75 + relativeHeight, 0.0, 1.0);
                // Use height-weighted skylight instead of replacing the cloud underside with the
                // brighter ground-irradiance term. Retain a bounded canonical ground bounce near the base.
                vec3 environmentLight = illumination.diffuseRadiance * topEnvironmentAccess;
                environmentLight += groundIrradiance * (1.0 - topEnvironmentAccess) * 0.25;
                vec3 primaryLight
                    = environmentLight + illumination.solarIrradiance * (sunTransmittance.primary * primaryPhase);

                // Both scattering orders reach the observer through the same physical extinction.
                // Sharing the Beer-Lambert segment weight makes their camera-path transport invariant
                // to partitioning; the second order retains its reduced solar extinction and wider phase.
                vec3 secondaryLight = illumination.solarIrradiance
                    * (sunTransmittance.secondary * secondaryPhase);
                vec3 primaryContribution = transmittance * (vec3(1.0) - sampleTransmittance);
                vec3 radiance = cloudAlbedo * primaryContribution * (primaryLight + secondaryLight);
                vec3 mainFraction = mainExtinction / max(combinedExtinction, vec3(1e-7));
                vec3 lowFraction = lowExtinction / max(combinedExtinction, vec3(1e-7));
                float mainContribution = dot(primaryContribution * mainFraction, vec3(1.0 / 3.0));
                float lowContribution = dot(primaryContribution * lowFraction, vec3(1.0 / 3.0));
                float fogVisibility = openmwseFogVisibility(
                    openmwseFogOpticalDistance(sampleDistance, worldDirection, atmosphere), atmosphere);
                // Density fading alone leaves thick paths opaque until the radial cutoff. Converge their
                // radiance to the same atmospheric sky as the background before that finite boundary.
                mainScatteredLight += openmwseCloudSegmentAtmosphericContribution(radiance * mainFraction, mainContribution,
                    cloudAtmosphericTarget, max(fogVisibility, mainCloudMinimumVisibility) * mainPresentationWeight);
                vec3 lowPrimaryLight = lowIllumination.diffuseRadiance
                    + lowIllumination.solarIrradiance * (sunTransmittance.primary * primaryPhase);
                vec3 lowSecondaryLight = lowIllumination.solarIrradiance * (sunTransmittance.secondary * secondaryPhase);
                lowScatteredLight += openmwseCloudSegmentAtmosphericContribution(lowAlbedo * lowFraction
                    * primaryContribution * (lowPrimaryLight + lowSecondaryLight), lowContribution,
                    cloudAtmosphericTarget, fogVisibility);
                mainOpacity += mainContribution;
                lowOpacity += lowContribution;
                mainRepresentativeDistanceSum += mainContribution * sampleDistance;
                lowRepresentativeDistanceSum += lowContribution * sampleDistance;
                transmittance *= sampleTransmittance;
                if (all(lessThan(transmittance, vec3(0.005))))
                {
                    sampleIndex = maximumSampleCount;
                    break;
                }
            }
            distanceAlongCloudIntervals = openmwseAdvanceCloudMarch(
                distanceAlongCloudIntervals, segmentLength, remainingIntervalLength, activeIntervalEnd);
        }
    }

    openmwseAdvanceLocalMedia(localMedia, maximumDistance, localCursor, transmittance,
        localScatteredLight, localOpacity, localDistanceSum);
    vec3 scatteredLight = mainScatteredLight + lowScatteredLight;
    scatteredLight += localScatteredLight;
    visibleCloudOpacity = mainOpacity + lowOpacity;
    visibleDistanceSum = mainRepresentativeDistanceSum + lowRepresentativeDistanceSum + localDistanceSum;
    return vec4(scatteredLight, dot(transmittance, vec3(1.0 / 3.0)));
}

/// Integrate native atmosphere using fixed-altitude low-cloud illumination and retain cloud history metadata.
vec4 openmwseIntegrateClouds(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, OpenMWSEVolumeIllumination lowIllumination,
    OpenMWSEAerialPerspectiveParameters atmosphere,
    OpenMWSELocalMedia localMedia, out float visibleDistanceSum, out float visibleCloudOpacity)
{
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, lowIllumination, vec3(0.0), atmosphere, false, localMedia, visibleDistanceSum, visibleCloudOpacity);
}

/// Integrate deterministic reflected clouds with fixed-altitude illumination supplied by the sky vertices.
vec4 openmwseIntegrateCloudsStable(vec3 rayOrigin, vec3 worldDirection, float maximumDistance, int quality,
    vec3 solarRadiance, vec3 skyIrradiance, vec3 groundIrradiance, OpenMWSEVolumeIllumination lowIllumination, vec3 atmosphericTarget,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    float visibleDistanceSum;
    float visibleCloudOpacity;
    OpenMWSELocalMedia localMedia;
    localMedia.count = 0;
    localMedia.startDistance = 0.0;
    localMedia.endDistance = 0.0;
    return openmwseIntegrateCloudsInternal(rayOrigin, worldDirection, maximumDistance, quality, solarRadiance,
        skyIrradiance, groundIrradiance, lowIllumination, atmosphericTarget, atmosphere, true, localMedia, visibleDistanceSum, visibleCloudOpacity);
}


#endif
