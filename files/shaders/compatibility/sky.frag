#version 120

#pragma import_defines(OPENMWSE_CLOUD_STEADY, OPENMWSE_REFLECTED_CLOUD_MODE)

#ifndef OPENMWSE_REFLECTED_CLOUD_MODE
#define OPENMWSE_REFLECTED_CLOUD_MODE 0
#endif

#include "lib/sky/cloud_lighting.h.glsl"
#include "lib/core/fragment.h.glsl"

#include "lib/sky/passes.glsl"

uniform int pass;
uniform sampler2D diffuseMap;
uniform sampler2D maskMap;      // PASS_MOON
uniform float opacity;          // PASS_CLOUDS, PASS_ATMOSPHERE_NIGHT
uniform vec2 screenRes;
uniform vec4 moonBlend;         // PASS_MOON
uniform vec4 atmosphereFade;    // PASS_MOON
uniform vec4 diffuseColor;

#include "lib/util/dither.glsl"
#include "fog.glsl"
#include "lib/atmosphere/textured_clouds.h.glsl"

varying vec2 diffuseMapUV;
varying vec4 passColor;
varying vec3 passViewDirection;

/// Render independent cloud transport or composite it with the full-resolution directional sky.
void paintAtmosphere(inout vec4 color)
{
    color = diffuseColor;
    vec3 viewDirection = normalize(passViewDirection);
    vec3 worldDirection = atmosphereWorldDirection(viewDirection);
    vec3 skyRadiance = atmosphereSkyColor(viewDirection);
#if OPENMWSE_REFLECTED_CLOUD_MODE == 1
    color = atmosphereCloudScattering(worldDirection, skyRadiance,
        OpenMWSEVolumeIllumination(skyLowCloudSolarIrradiance, skyLowCloudDiffuseRadiance));
#else
#if OPENMWSE_REFLECTED_CLOUD_MODE == 2
    vec4 clouds = sampleReflectedCloudTexture(gl_FragCoord.xy / screenRes);
#else
    vec4 clouds
        = isReflection ? atmosphereCloudScattering(worldDirection, skyRadiance,
            OpenMWSEVolumeIllumination(skyLowCloudSolarIrradiance, skyLowCloudDiffuseRadiance)) : vec4(0.0, 0.0, 0.0, 1.0);
#endif
    color.xyz = skyRadiance * clouds.a + clouds.rgb;
    color.xyz += openmwseTriangularDither(gl_FragCoord.xy) * (0.5 / 255.0);
    color.a = 1.0;
#endif
}

void paintAtmosphereNight(inout vec4 color)
{
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= passColor.a * opacity;
}

/// Preserve the authored dome's UVs and vertex fade while blending weather-colored clouds over the scattered sky.
void paintClouds(inout vec4 color)
{
    if (atmosphere.cloudsEnabled || (isReflection && !atmosphere.cloudReflectionsEnabled))
    {
        color = vec4(0.0);
        return;
    }
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= passColor.a * opacity;
    color.xyz = clamp(color.xyz * diffuseColor.xyz, 0.0, 1.0);

    // Alpha blending exposes the already-scattered sky, as in MGE XE's separate cloud pass.
}

void paintMoon(inout vec4 color)
{
    vec4 phase = texture2D(diffuseMap, diffuseMapUV);
    vec4 mask = texture2D(maskMap, diffuseMapUV);

    // Morrowind does this in two passes

    // First pass: moon shadow, normal blending (src alpha, 1 - src alpha)
    // dst.rgb = mask.rgb * mask.a + dst.rgb * (1 - mask.a)
    // Second pass: moon phase, additive blending (src alpha, 1)
    // dst.rgb += phase.rgb * phase.a

    // The same is doable in a single pass through premultiplied alpha blending
    // color.rgb = mask.rgb * mask.a + phase.rgb * phase.a
    // color.a = mask.a
    // dst.rgb = color.rgb + dst.rgb * (1 - color.a)

    vec3 maskTinted = mask.rgb * atmosphereFade.rgb;
    float maskAlpha = mask.a * atmosphereFade.a;
    vec3 phaseTinted = phase.rgb * moonBlend.rgb;
    float phaseAlpha = phase.a * atmosphereFade.a;

    color.rgb = maskTinted * maskAlpha + phaseTinted * phaseAlpha;
    color.a = maskAlpha;
}

/// Add occluded reflected sunlight after the cloud layers; the primary view receives sunlight in its native composite.
void paintSun(inout vec4 color)
{
    if (atmosphere.sunlightEnabled)
    {
        color = vec4(0.0);
        if (!isReflection)
            return;
        vec3 worldDirection = atmosphereWorldDirection(normalize(passViewDirection));
        vec3 solar = atmosphereSunDisc(worldDirection);
        vec3 bloom = vec3(0.0);
        if (atmosphere.cloudSunOcclusion && atmosphere.cloudReflectionsEnabled && openmwseTexturedCloudsEnabled)
            bloom = openmwseTexturedCloudBloom(worldDirection,
                normalize(atmosphere.sunDirection), atmosphere.sunDiscColor);
        if (dot(solar + bloom, vec3(1.0)) <= 0.000001)
            return;
        float transmission = 1.0;
        if (atmosphere.cloudSunOcclusion && atmosphere.cloudReflectionsEnabled)
        {
            transmission = openmwseTexturedCloudViewTransmittance(worldDirection);
#if OPENMWSE_REFLECTED_CLOUD_MODE == 2
            transmission *= sampleReflectedCloudTexture(gl_FragCoord.xy / screenRes).a;
#else
            if (atmosphere.cloudsEnabled)
                transmission *= atmosphereCloudScattering(worldDirection, vec3(0.0),
                    OpenMWSEVolumeIllumination(skyLowCloudSolarIrradiance, skyLowCloudDiffuseRadiance)).a;
#endif
        }
        color.rgb = solar * transmission + bloom;
        return;
    }
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= opacity;
    color.rgb *= color.a;
    color.a = 0.0;
}

void paintSunglare(inout vec4 color)
{
    color = atmosphere.enabled ? vec4(0.0) : diffuseColor;
}

void processSunflashQuery()
{
    const float threshold = 0.8;

    if (atmosphere.enabled || texture2D(diffuseMap, diffuseMapUV).a <= threshold)
        discard;
}

void main()
{
    vec4 color = vec4(0.0);

#if OPENMWSE_REFLECTED_CLOUD_MODE == 2
    if (pass == PASS_SUN)
        paintSun(color);
    else
        paintAtmosphere(color);
#elif OPENMWSE_REFLECTED_CLOUD_MODE == 1
    paintAtmosphere(color);
#else
    if (pass == PASS_ATMOSPHERE)
        paintAtmosphere(color);
    else if (pass == PASS_ATMOSPHERE_NIGHT)
        paintAtmosphereNight(color);
    else if (pass == PASS_CLOUDS)
        paintClouds(color);
    else if (pass == PASS_MOON)
        paintMoon(color);
    else if (pass == PASS_SUN)
        paintSun(color);
    else if (pass == PASS_SUNGLARE)
        paintSunglare(color);
    else if (pass == PASS_SUNFLASH_QUERY)
    {
        processSunflashQuery();
        return;
    }
#endif

    gl_FragData[0] = color;
}
