uniform bool waterCaustics;
uniform bool waterReflectedCaustics;
uniform float waterCausticsStrength;
uniform int waterOpticsAppearance;
uniform bool waterOpticsDistantWaves;
uniform float waterGeometricWaveHeight;
uniform float waterRainIntensity;
uniform float windSpeed;

#define WATER_OPTICAL_NORMAL_VOLUME
#include "lib/water/optical_field.glsl"
#undef WATER_OPTICAL_NORMAL_VOLUME

#include "lib/water/surface_coverage.glsl"

/// Return the signed area spanned by two screen derivatives.
float waterDifferentialArea(vec2 dx, vec2 dy)
{
    return dx.x * dy.y - dx.y * dy.x;
}

/// Recover the world-space slope of the shared geometric height field from screen derivatives.
vec2 waterGeometricSlope(vec3 surfacePosition, float height)
{
    vec2 positionDx = dFdx(surfacePosition.xy);
    vec2 positionDy = dFdy(surfacePosition.xy);
    float determinant = waterDifferentialArea(positionDx, positionDy);
    if (abs(determinant) < 0.0001)
        return vec2(0.0);
    float heightDx = dFdx(height);
    float heightDy = dFdy(height);
    return vec2(heightDx * positionDy.y - heightDy * positionDx.y,
        positionDx.x * heightDy - positionDy.x * heightDx) / determinant;
}

/// Combine the optical and geometric wave slopes into one world-space surface normal.
vec3 waterCombinedSurfaceNormal(vec3 opticalNormal, vec2 geometricSlope)
{
    vec2 opticalSlope = -opticalNormal.xy / max(opticalNormal.z, 0.05);
    return normalize(vec3(-(opticalSlope + geometricSlope), 1.0));
}

/// Refract one sunlight ray through the optical normal and shared geometric slope.
vec2 waterRefractedRaySlope(vec3 sunDirection, vec3 opticalNormal, vec2 geometricSlope)
{
    vec3 combinedNormal = waterCombinedSurfaceNormal(opticalNormal, geometricSlope);
    vec3 transmittedRay = refract(-sunDirection, combinedNormal, 1.0 / 1.333);
    return transmittedRay.xy / max(-transmittedRay.z, 0.05);
}

/// Reflect one directional-light ray from the optical normal and shared geometric slope.
vec3 waterReflectedRay(vec3 lightDirection, vec3 opticalNormal, vec2 geometricSlope)
{
    return reflect(-lightDirection, waterCombinedSurfaceNormal(opticalNormal, geometricSlope));
}

/// Redistribute directional light reflected by the active water field onto one above-water receiver.
vec3 applyWaterReflectedCaustics(vec3 radiance, vec3 receiverPosition, vec3 receiverNormal,
    vec3 lightDirection, float shadowVisibility)
{
    float receiverHeight = receiverPosition.z - waterHeight;
    if (receiverHeight <= 4.0 || receiverHeight >= 1800.0)
        return radiance;

    vec3 flatReflectedRay = reflect(-lightDirection, vec3(0.0, 0.0, 1.0));
    if (flatReflectedRay.z <= 0.05)
        return radiance;

    float directionalLuminance = dot(max(sun.diffuse.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    float sourceVisibility = mix(0.35, 1.0, clamp(sun.specular.a, 0.0, 1.0));
    float sourceLight = clamp(directionalLuminance * lightDirection.z * sourceVisibility, 0.0, 1.0);
    if (sourceLight <= 0.0)
        return radiance;

    vec2 surfacePosition
        = receiverPosition.xy - receiverHeight * flatReflectedRay.xy / flatReflectedRay.z;
    float exposedSurface = 1.0 - sampleWaterEnclosedCoverage(surfacePosition);
    if (exposedSurface <= 0.0)
        return radiance;
    vec3 cameraPosition = osg_ViewMatrixInverse[3].xyz;
    float cameraDistance = distance(vec3(surfacePosition, waterHeight), cameraPosition);
    float geometricHeight = sampleWaterGeometricHeight(surfacePosition, osg_SimulationTime,
        waterOpticsAppearance, cameraDistance, waterGeometricWaveHeight, windSpeed, fog.start, fog.end);
    receiverHeight -= geometricHeight;
    if (receiverHeight <= 4.0)
        return radiance;
    surfacePosition
        = receiverPosition.xy - receiverHeight * flatReflectedRay.xy / flatReflectedRay.z;

    vec3 opticalNormal = sampleWaterCausticOpticalNormal(surfacePosition, osg_SimulationTime,
        waterOpticsAppearance, cameraPosition.xy, waterOpticsDistantWaves, waterRainIntensity);
    vec2 surfaceDx = dFdx(surfacePosition);
    vec2 surfaceDy = dFdy(surfacePosition);
    float pixelFootprint = 0.5 * (length(surfaceDx) + length(surfaceDy));
    float differentialStep = max(20.0 + 0.018 * receiverHeight, 2.0 * pixelFootprint);
    vec3 opticalNormalX = sampleWaterCausticOpticalNormal(surfacePosition + vec2(differentialStep, 0.0),
        osg_SimulationTime, waterOpticsAppearance, cameraPosition.xy, waterOpticsDistantWaves, waterRainIntensity);
    vec3 opticalNormalY = sampleWaterCausticOpticalNormal(surfacePosition + vec2(0.0, differentialStep),
        osg_SimulationTime, waterOpticsAppearance, cameraPosition.xy, waterOpticsDistantWaves, waterRainIntensity);
    vec2 geometricSlope = waterGeometricSlope(vec3(surfacePosition, waterHeight), geometricHeight);
    vec3 reflectedRay = waterReflectedRay(lightDirection, opticalNormal, geometricSlope);
    vec3 reflectedRayX = waterReflectedRay(lightDirection, opticalNormalX, geometricSlope);
    vec3 reflectedRayY = waterReflectedRay(lightDirection, opticalNormalY, geometricSlope);
    if (reflectedRay.z <= 0.05 || reflectedRayX.z <= 0.05 || reflectedRayY.z <= 0.05)
        return radiance;

    vec2 raySlope = reflectedRay.xy / reflectedRay.z;
    vec2 raySlopeX = reflectedRayX.xy / reflectedRayX.z;
    vec2 raySlopeY = reflectedRayY.xy / reflectedRayY.z;
    vec2 mappedX = vec2(1.0, 0.0) + receiverHeight * (raySlopeX - raySlope) / differentialStep;
    vec2 mappedY = vec2(0.0, 1.0) + receiverHeight * (raySlopeY - raySlope) / differentialStep;
    float areaScale = abs(waterDifferentialArea(mappedX, mappedY));
    float focusedIrradiance = clamp(1.0 / max(areaScale, 0.12), 0.35, 3.0);
    float focusedLight = max(focusedIrradiance - 1.0, 0.0);

    vec3 surfaceWorldPosition = vec3(surfacePosition, waterHeight + geometricHeight);
    vec3 directionToWater = normalize(surfaceWorldPosition - receiverPosition);
    float receiverFacing = max(dot(receiverNormal, directionToWater), 0.0);
    if (receiverFacing <= 0.0)
        return radiance;

    float heightTransmission = exp(-receiverHeight / 950.0)
        * smoothstep(4.0, 40.0, receiverHeight) * (1.0 - smoothstep(1100.0, 1800.0, receiverHeight));
    float shadedContrast = mix(0.28, 1.0, 1.0 - clamp(shadowVisibility, 0.0, 1.0));
    float caustic = 0.42 * focusedLight * heightTransmission * sourceLight * receiverFacing * exposedSurface
        * shadedContrast * waterCausticsStrength;
    vec3 lightColor = sun.diffuse.rgb / max(directionalLuminance, 0.25);
    vec3 materialResponse = max(radiance, vec3(0.12)) * lightColor;
    return max(radiance + materialResponse * caustic, vec3(0.0));
}

/// Redistribute direct and reflected directional light through the active water field.
vec3 applyWaterCaustics(vec3 radiance, vec3 viewPosition, vec3 viewNormal, float shadowVisibility)
{
    if ((!waterCaustics && !waterReflectedCaustics) || !waterEnabled || waterSurface || isReflection
        || waterCausticsStrength <= 0.0)
        return radiance;

    vec3 receiverPosition = (osg_ViewMatrixInverse * vec4(viewPosition, 1.0)).xyz;
    float receiverDepth = waterHeight - receiverPosition.z;
    vec3 lightDirection = normalize((osg_ViewMatrixInverse * vec4(sun.position.xyz, 0.0)).xyz);
    if (receiverDepth <= 0.0)
    {
        if (!waterReflectedCaustics)
            return radiance;
        vec3 receiverNormal = normalize(mat3(osg_ViewMatrixInverse) * viewNormal);
        return applyWaterReflectedCaustics(
            radiance, receiverPosition, receiverNormal, lightDirection, shadowVisibility);
    }

    if (!waterCaustics || receiverDepth >= 2600.0)
        return radiance;

    float directSun = clamp(sun.specular.a * lightDirection.z * shadowVisibility, 0.0, 1.0);
    if (directSun <= 0.0)
        return radiance;

    vec3 cameraPosition = osg_ViewMatrixInverse[3].xyz;
    vec3 transmittedRay = refract(-lightDirection, vec3(0.0, 0.0, 1.0), 1.0 / 1.333);
    vec2 surfacePosition = receiverPosition.xy
        - receiverDepth * transmittedRay.xy / max(-transmittedRay.z, 0.05);
    float cameraDistance = distance(vec3(surfacePosition, waterHeight), cameraPosition);
    float geometricHeight = sampleWaterGeometricHeight(surfacePosition, osg_SimulationTime, waterOpticsAppearance,
        cameraDistance, waterGeometricWaveHeight, windSpeed, fog.start, fog.end);
    receiverDepth += geometricHeight;
    surfacePosition = receiverPosition.xy
        - receiverDepth * transmittedRay.xy / max(-transmittedRay.z, 0.05);

    vec3 opticalNormal = sampleWaterCausticOpticalNormal(surfacePosition, osg_SimulationTime,
        waterOpticsAppearance, cameraPosition.xy, waterOpticsDistantWaves, waterRainIntensity);
    vec2 surfaceDx = dFdx(surfacePosition);
    vec2 surfaceDy = dFdy(surfacePosition);
    float pixelFootprint = 0.5 * (length(surfaceDx) + length(surfaceDy));
    float differentialStep = max(24.0 + 0.02 * receiverDepth, 2.0 * pixelFootprint);
    vec3 opticalNormalX = sampleWaterCausticOpticalNormal(surfacePosition + vec2(differentialStep, 0.0),
        osg_SimulationTime, waterOpticsAppearance, cameraPosition.xy, waterOpticsDistantWaves, waterRainIntensity);
    vec3 opticalNormalY = sampleWaterCausticOpticalNormal(surfacePosition + vec2(0.0, differentialStep),
        osg_SimulationTime, waterOpticsAppearance, cameraPosition.xy, waterOpticsDistantWaves, waterRainIntensity);
    vec2 geometricSlope = waterGeometricSlope(vec3(surfacePosition, waterHeight), geometricHeight);
    vec2 raySlope = waterRefractedRaySlope(lightDirection, opticalNormal, geometricSlope);
    vec2 raySlopeX = waterRefractedRaySlope(lightDirection, opticalNormalX, geometricSlope);
    vec2 raySlopeY = waterRefractedRaySlope(lightDirection, opticalNormalY, geometricSlope);
    vec2 mappedX = vec2(1.0, 0.0) + receiverDepth * (raySlopeX - raySlope) / differentialStep;
    vec2 mappedY = vec2(0.0, 1.0) + receiverDepth * (raySlopeY - raySlope) / differentialStep;
    float areaScale = abs(waterDifferentialArea(mappedX, mappedY));
    float focusedIrradiance = clamp(1.0 / max(areaScale, 0.16), 0.45, 2.4);
    float redistributedLight = focusedIrradiance - 1.0;
    float depthTransmission = exp(-receiverDepth / 900.0) * smoothstep(3.0, 36.0, receiverDepth);
    float caustic = 0.65 * redistributedLight * depthTransmission * directSun * waterCausticsStrength;
    vec3 illuminatedBase = max(radiance, vec3(0.045)) * sun.diffuse.rgb;
    return max(radiance + illuminatedBase * caustic, vec3(0.0));
}
