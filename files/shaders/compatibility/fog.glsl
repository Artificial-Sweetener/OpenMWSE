@link "lib/atmosphere/textured_clouds_fragment.glsl"
#include "fog.h.glsl"
#include "lib/atmosphere/diagnostics.glsl"

#if @skyBlending
#include "lib/core/fragment.h.glsl"

uniform float skyBlendingStart;
#endif

@link "lib/atmosphere/cloud_model_fragment.glsl"
#include "atmosphere.glsl"

const vec3 WATER_COLOR = vec3(0.090195, 0.115685, 0.12745);

/// Filter scene-linear ambient illumination through the water medium without scalar norm amplification.
vec3 waterInScatteringColor()
{
    return WATER_COLOR * max(sun.ambient.xyz, vec3(0.0));
}

/// Apply shared altitude-aware aerial perspective or the configured underwater/depth-fog transfer.
void computeFog(vec3 pos, float euclideanDist, float linearDist, float near, float far, out float colorScale, out vec3 colorOffset)
{
#if @radialFog
    float dist = euclideanDist;
#else
    float dist = abs(linearDist);
#endif

    bool cameraBelowWater = false;
    bool useWaterDepthFog = false;
    float underwaterFogFactor = 1.0;
    float waterDepth = 0.0;
    float airEuclideanDistance = euclideanDist;

    if (waterEnabled) {
        vec3 cameraPos = osg_ViewMatrixInverse[3].xyz;
        cameraBelowWater = cameraPos.z < waterHeight;
        if (!cameraBelowWater && !isReflection && !waterSurface) {
            vec3 worldPos = (osg_ViewMatrixInverse * vec4(pos, 1)).xyz;

            if (worldPos.z < waterHeight) {
                useWaterDepthFog = true;
                const float visibility = 2500.0;
                const float depthFade = 0.25;
                float underwaterSegmentFraction
                    = clamp((waterHeight - worldPos.z) / (cameraPos.z - worldPos.z), 0.0, 1.0);
                waterDepth = dist * underwaterSegmentFraction;
                airEuclideanDistance *= 1.0 - underwaterSegmentFraction;
                float depthCorrection = sqrt(1.0 + 4.0 * depthFade * depthFade);
                underwaterFogFactor = depthFade * depthFade
                    / (-0.5 * depthCorrection + 0.5 - waterDepth / visibility) + 0.5 * depthCorrection + 0.5;
                underwaterFogFactor = clamp(underwaterFogFactor, 0.0, 1.0);
            }
        }
    }

    vec4 fogColor = cameraBelowWater ? fog.underwaterColor : fog.color;
    float start = cameraBelowWater ? fog.underwaterStart : fog.start;
    float end = cameraBelowWater ? fog.underwaterEnd : fog.end;

    if (fog.depth >= 0.0) {
        start = near * fog.depth + far * (1.0 - fog.depth);
        end = far;
        useWaterDepthFog = false;
    }

    if (useWaterDepthFog)
        dist -= waterDepth;

    float opticalDistance;
#if @exponentialFog
    opticalDistance = openmwseExponentialFogOpticalDistance(dist, start, end);
#else
    opticalDistance = openmwseLinearFogOpticalDistance(dist, start, end);
#endif
    OpenMWSEAerialPerspectiveParameters atmosphereParameters = openmwseAerialPerspectiveParameters();
    if (atmosphere.enabled && atmosphere.scatteringEnabled && !cameraBelowWater && fog.depth < 0.0)
    {
        atmosphereParameters.fogStart = start;
        atmosphereParameters.fogEnd = end;
        vec3 worldDirection = atmosphereWorldDirection(normalize(pos));
        // Planar fog uses projected distance, but its physical segment still reaches the same world height.
        worldDirection.z *= airEuclideanDistance / max(dist, 1e-4);
        opticalDistance = openmwseFogOpticalDistance(dist, worldDirection, atmosphereParameters);
    }
    float fogVisibility = openmwseFogVisibility(opticalDistance, atmosphereParameters);
    float fogValue = 1.0 - fogVisibility;
    colorScale = fogVisibility;
#ifdef ADDITIVE_BLENDING
    colorOffset = vec3(0.0);
#else
    if (atmosphere.enabled && atmosphere.scatteringEnabled && !cameraBelowWater)
        colorOffset = atmosphereScatter(normalize(pos), clamp(0.224 * opticalDistance, 0.0, 1.0),
            fogVisibility, fogColor.xyz);
    else
        colorOffset = fogColor.xyz * fogValue;
#endif

    if (useWaterDepthFog) {
        colorScale *= 1.0 - underwaterFogFactor;
#ifndef ADDITIVE_BLENDING
        colorOffset += waterInScatteringColor() * underwaterFogFactor * (1.0 - fogValue);
#endif
    }

#if @skyBlending
    if (!cameraBelowWater && !isReflection) {
        float fadeValue = clamp((far - dist) / (far - skyBlendingStart), 0.0, 1.0);
        fadeValue *= fadeValue;
        colorScale *= fadeValue;
#ifndef ADDITIVE_BLENDING
        colorOffset = colorOffset * fadeValue + sampleSkyColor(gl_FragCoord.xy / screenRes) * (1.0 - fadeValue);
#endif
    }
#endif
}

vec4 applyFogAtDist(vec4 color, vec3 pos, float euclideanDist, float linearDist, float near, float far)
{
    float colorScale;
    vec3 colorOffset;
    computeFog(pos, euclideanDist, linearDist, near, far, colorScale, colorOffset);

    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticFogBypassed)
        return color;
    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticFogVisibility)
        return vec4(vec3(colorScale), color.a);
    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticFogInScattering)
        return vec4(colorOffset, color.a);

    color.xyz = color.xyz * colorScale + colorOffset;
    return color;
}

vec4 applyFogAtPos(vec4 color, vec3 pos, float near, float far)
{
    return applyFogAtDist(color, pos, length(pos), pos.z, near, far);
}
