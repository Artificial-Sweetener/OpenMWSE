uniform bool waterCaustics;
uniform float waterCausticsStrength;

#define WATER_OPTICAL_NORMAL_VOLUME
#include "lib/water/optical_field.glsl"
#undef WATER_OPTICAL_NORMAL_VOLUME

#include "lib/water/surface_coverage.glsl"
#include "lib/water/local_lighting.glsl"

/// Sample the animated MGE XE caustic field at its authored world scale and speed.
float waterMgeXeCausticSample(vec2 surfacePosition)
{
    return sampleWaterOpticalVolume(
        vec3(surfacePosition / 1104.0, 0.4 * osg_SimulationTime)).b;
}

/// Reproduce MGE XE's screen-footprint attenuation for its projected caustic volume.
float waterMgeXeCausticFootprint(vec2 surfacePosition, float viewDepth)
{
    float projectedFootprint = min(fwidth(surfacePosition.x), fwidth(surfacePosition.y));
    return clamp(125.0 * projectedFootprint / max(viewDepth, 1.0), 0.0, 1.0);
}

/// Reproduce MGE XE's near-water depth envelope while retaining OpenMW's configured fog range.
float waterMgeXeCausticEnvelope(float receiverDepth, float viewDepth)
{
    float depthFade = clamp(exp(-receiverDepth / 400.0), 0.0, 1.0)
        * clamp(receiverDepth / 30.0, 0.0, 1.0);
    bool cameraUnderwater = osg_ViewMatrixInverse[3].z < waterHeight;
    float fogStart = cameraUnderwater ? fog.underwaterStart : fog.start;
    float fogEnd = cameraUnderwater ? fog.underwaterEnd : fog.end;
    float fogVisibility = clamp((fogEnd - viewDepth) / max(fogEnd - fogStart, 0.0001), 0.0, 1.0);
    return depthFade * fogVisibility;
}

/// Convert OpenMWSE's normalized strength control to MGE XE's default intensity of 50.
float waterMgeXeCausticStrength(float illumination)
{
    return 2.5 * waterCausticsStrength * clamp(illumination, 0.0, 1.0);
}

/// Apply MGE XE's multiplicative caustic contrast at one projected water-surface point.
vec3 applyWaterMgeXeCaustic(vec3 radiance, vec3 receiverPosition, vec3 surfacePosition,
    float viewDepth, float illumination, vec3 lightColor)
{
    float receiverDepth = waterHeight - receiverPosition.z;
    float exposedSurface = sampleWaterCausticPathExposure(surfacePosition, receiverPosition);
    if (exposedSurface <= 0.0)
        return radiance;

    float caustic = waterMgeXeCausticStrength(illumination)
        * waterMgeXeCausticSample(surfacePosition.xy)
        * waterMgeXeCausticFootprint(surfacePosition.xy, viewDepth);
    float contrast = (caustic - 0.3) * waterMgeXeCausticEnvelope(receiverDepth, viewDepth)
        * exposedSurface;
    return max(radiance * (vec3(1.0) + lightColor * contrast), vec3(0.0));
}

#if WATER_LOCAL_LIGHTS_AVAILABLE
/// Project one bounded local light through the water plane using MGE XE's caustic field.
vec3 applyWaterPointTransmittedCaustic(
    vec3 radiance, int lightIndex, vec3 receiverPosition, float viewDepth)
{
    if (lightIndex < 0)
        return radiance;

    vec4 positionRadius = waterCanonicalLightPositionRadius(lightIndex);
    vec3 sourcePosition = (osg_ViewMatrixInverse * vec4(positionRadius.xyz, 1.0)).xyz;
    float sourceHeight = sourcePosition.z - waterHeight;
    float receiverDepth = waterHeight - receiverPosition.z;
    if (sourceHeight <= 1.0 || receiverDepth <= 0.0)
        return radiance;

    float sourceDistance = distance(sourcePosition, receiverPosition);
    if (sourceDistance >= positionRadius.w)
        return radiance;

    float surfaceFraction = receiverDepth / max(sourceHeight + receiverDepth, 0.001);
    vec3 surfacePosition = mix(receiverPosition, sourcePosition, surfaceFraction);
    surfacePosition.z = waterHeight;
    float attenuation = waterCanonicalLightAttenuation(lightIndex, sourceDistance)
        * waterLocalLightVisibility(lightIndex, waterWorldPositionToView(surfacePosition));
    if (attenuation <= 0.0)
        return radiance;

    vec3 sourceColor = max(waterCanonicalLightColor(lightIndex), vec3(0.0));
    float sourceLuminance = max(dot(sourceColor, vec3(0.2126, 0.7152, 0.0722)), 0.0001);
    return applyWaterMgeXeCaustic(radiance, receiverPosition, surfacePosition, viewDepth,
        attenuation * sourceLuminance, sourceColor / sourceLuminance);
}
#endif

/// Apply configured local-light caustics through a statically bounded evaluation path.
vec3 applyWaterLocalTransmittedCaustics(
    vec3 radiance, ivec4 selected, vec3 receiverPosition, float viewDepth)
{
#if WATER_LOCAL_LIGHTS_AVAILABLE
    int budget = waterLocalLightBudget();
    if (budget > 0)
        radiance = applyWaterPointTransmittedCaustic(radiance, selected.x, receiverPosition, viewDepth);
    if (budget > 1)
        radiance = applyWaterPointTransmittedCaustic(radiance, selected.y, receiverPosition, viewDepth);
    if (budget > 2)
        radiance = applyWaterPointTransmittedCaustic(radiance, selected.z, receiverPosition, viewDepth);
    if (budget > 3)
        radiance = applyWaterPointTransmittedCaustic(radiance, selected.w, receiverPosition, viewDepth);
#endif
    return radiance;
}

/// Apply MGE XE-style directional and local-light caustics to submerged receivers.
vec3 applyWaterCaustics(vec3 radiance, vec3 viewPosition, vec3 viewNormal, float shadowVisibility)
{
    if (!waterCaustics || !waterEnabled || waterSurface || isReflection || waterCausticsStrength <= 0.0)
        return radiance;

    vec3 receiverPosition = (osg_ViewMatrixInverse * vec4(viewPosition, 1.0)).xyz;
    float receiverDepth = waterHeight - receiverPosition.z;
    if (receiverDepth <= 0.0 || receiverDepth >= 2600.0)
        return radiance;

    float viewDepth = abs(viewPosition.z);
    vec3 lightDirection = normalize((osg_ViewMatrixInverse * vec4(sun.position.xyz, 0.0)).xyz);
    vec3 approximateSurfacePosition = vec3(receiverPosition.xy, waterHeight);
    ivec4 selected = waterSelectLocalLights(gl_FragCoord.xy, viewPosition,
        waterWorldPositionToView(approximateSurfacePosition));

    if (!waterInterior && lightDirection.z > 0.001)
    {
        vec3 surfacePosition = vec3(receiverPosition.xy
            + lightDirection.xy * receiverDepth / lightDirection.z, waterHeight);
        float sunVisibility = clamp(sun.specular.a, 0.0, 1.0);
        float sunlightFactor = 1.0 - pow(1.0 - sunVisibility, 2.0);
        float directVisibility = clamp(0.75 * sunlightFactor
            + 0.35 * length(fog.color.rgb), 0.0, 1.0) * shadowVisibility;
        radiance = applyWaterMgeXeCaustic(radiance, receiverPosition, surfacePosition,
            viewDepth, directVisibility, vec3(1.0));
    }
    return applyWaterLocalTransmittedCaustics(radiance, selected, receiverPosition, viewDepth);
}
