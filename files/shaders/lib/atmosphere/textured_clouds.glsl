#ifndef OPENMWSE_TEXTURED_CLOUDS_GLSL
#define OPENMWSE_TEXTURED_CLOUDS_GLSL

#include "textured_clouds.h.glsl"

/// Project along sunlight onto a world-space UV datum; direction.z must exceed 0.01.
vec3 openmwseTexturedCloudShadowProjection(vec3 origin, vec3 direction)
{
    // Signed travel keeps terrain above the datum in the same continuous shadow field.
    float distance = (openmwseTexturedCloudProjectionHeight - origin.z) / direction.z;
    return origin + direction * distance;
}

/// Apply the weather layer's storm orientation and continuous UV advection to a world position.
vec2 openmwseTexturedCloudShadowUv(vec3 point, vec4 transform)
{
    vec2 position = point.xy / openmwseTexturedCloudTileSize;
    return vec2(dot(position, transform.xy), dot(position, vec2(transform.y, -transform.x))) + vec2(0.5) + transform.zw;
}

/// Convert authored cloud coverage into Beer transmission; empty texels remain exactly transparent.
float openmwseTexturedCloudTransmission(float alpha)
{
    float clearFraction = 1.0 - clamp(alpha, 0.0, 1.0);
    return clearFraction * clearFraction * clearFraction * clearFraction;
}

/// Project softly prefiltered weather opacity onto surfaces as bounded Beer transmission.
float openmwseTexturedCloudTransmittance(vec3 origin, vec3 direction, bool legacyCloudShadows)
{
    if (!openmwseTexturedCloudsEnabled)
        return 1.0;
    vec3 point = openmwseTexturedCloudShadowProjection(origin, direction);
    if (legacyCloudShadows)
    {
        float outgoing = texture2D(openmwseTexturedCloudOpacity,
            openmwseTexturedCloudShadowUv(point, openmwseTexturedCloudTransforms[0])).r;
        float incoming = texture2D(openmwseTexturedCloudOpacity,
            openmwseTexturedCloudShadowUv(point, openmwseTexturedCloudTransforms[1])).g;
        return openmwseTexturedCloudTransmission(outgoing * openmwseTexturedCloudWeights.x)
            * openmwseTexturedCloudTransmission(incoming * openmwseTexturedCloudWeights.y);
    }
    float outgoing = texture2D(openmwseTexturedCloudOpacity,
        openmwseTexturedCloudShadowUv(point, openmwseTexturedCloudTransforms[0])).b;
    float incoming = texture2D(openmwseTexturedCloudOpacity,
        openmwseTexturedCloudShadowUv(point, openmwseTexturedCloudTransforms[1])).a;
    const float maximumOpticalDepth = 1.05;
    float opticalDepth = maximumOpticalDepth * (outgoing * openmwseTexturedCloudWeights.x
        + incoming * openmwseTexturedCloudWeights.y);
    return exp(-opticalDepth);
}

/// Recover the loaded dome's authored UVs and vertex opacity in its unrotated local direction.
vec3 openmwseTexturedCloudDomeSample(vec3 direction, vec4 transform)
{
    if (!openmwseTexturedCloudDomeAvailable || direction.z < 0.0)
        return vec3(0.0);
    vec2 localDirection = vec2(dot(direction.xy, transform.xy),
        dot(direction.xy, vec2(-transform.y, transform.x)));
    vec2 lookup = 0.5 + 0.5 * localDirection / (1.0 + direction.z);
    vec3 mapping = texture2D(openmwseTexturedCloudDomeMapping, lookup).rgb;
    return vec3(mapping.xy / max(mapping.z, 0.00001) + transform.zw, mapping.z);
}

/// Attenuate celestial light through the authored camera-relative dome, independent of observer altitude.
float openmwseTexturedCloudViewTransmittance(vec3 direction)
{
    if (!openmwseTexturedCloudsEnabled)
        return 1.0;
    vec3 outgoing = openmwseTexturedCloudDomeSample(direction, openmwseTexturedCloudTransforms[0]);
    vec3 incoming = openmwseTexturedCloudDomeSample(direction, openmwseTexturedCloudTransforms[1]);
    float outgoingAlpha = texture2D(openmwseTexturedCloudOpacity, outgoing.xy).r
        * outgoing.z * openmwseTexturedCloudWeights.x;
    float incomingAlpha = texture2D(openmwseTexturedCloudOpacity, incoming.xy).g
        * incoming.z * openmwseTexturedCloudWeights.y;
    return openmwseTexturedCloudTransmission(outgoingAlpha) * openmwseTexturedCloudTransmission(incomingAlpha);
}

/// Illuminate optically thin cloud edges with a bounded forward-scattering solar lobe.
float openmwseTexturedCloudSolarSource(vec3 direction, vec3 sunDirection)
{
    float transmission = openmwseTexturedCloudViewTransmittance(direction);
    float forwardPhase = pow(max(dot(direction, sunDirection), 0.0), 64.0);
    return (1.0 - transmission) * sqrt(transmission) * forwardPhase;
}

/// Add local cloud illumination and a small angular diffusion halo, independent of pixel size or camera motion.
vec3 openmwseTexturedCloudBloom(vec3 direction, vec3 sunDirection, vec4 solarColor)
{
    if (dot(direction, sunDirection) < 0.9 || solarColor.a <= 0.001)
        return vec3(0.0);
    vec3 reference = abs(direction.z) > 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 horizontal = normalize(cross(direction, reference));
    vec3 vertical = cross(horizontal, direction);
    float center = openmwseTexturedCloudSolarSource(direction, sunDirection);
    float diffusion = 0.0;
    const float diffusionAngle = 0.008;
    diffusion += openmwseTexturedCloudSolarSource(normalize(direction + horizontal * diffusionAngle), sunDirection);
    diffusion += openmwseTexturedCloudSolarSource(normalize(direction - horizontal * diffusionAngle), sunDirection);
    diffusion += openmwseTexturedCloudSolarSource(normalize(direction + vertical * diffusionAngle), sunDirection);
    diffusion += openmwseTexturedCloudSolarSource(normalize(direction - vertical * diffusionAngle), sunDirection);
    return solarColor.rgb * solarColor.a * (1.25 * center + 0.08 * diffusion);
}

#endif
