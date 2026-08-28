#version 120

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

varying vec2 diffuseMapUV;
varying vec4 passColor;
varying vec3 passViewDirection;

void paintAtmosphere(inout vec4 color)
{
    color = diffuseColor;
    vec3 viewDirection = normalize(passViewDirection);
    vec3 worldDirection = atmosphereWorldDirection(viewDirection);
    vec3 skyRadiance = atmosphereSkyColor(viewDirection);
    vec4 clouds
        = isReflection ? atmosphereCloudScattering(worldDirection, skyRadiance) : vec4(0.0, 0.0, 0.0, 1.0);
    color.xyz = skyRadiance * clouds.a + clouds.rgb;
    color.xyz += atmosphereSunDisc(worldDirection) * clouds.a;
    color.xyz += openmwseTriangularDither(gl_FragCoord.xy) * (0.5 / 255.0);
    color.a = 1.0;
}

void paintAtmosphereNight(inout vec4 color)
{
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= passColor.a * opacity;
}

void paintClouds(inout vec4 color)
{
    if (atmosphere.enabled)
    {
        color = vec4(0.0);
        return;
    }
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= passColor.a * opacity;
    color.xyz = clamp(color.xyz * diffuseColor.xyz, 0.0, 1.0);

    // ease transition between clear color and atmosphere/clouds
    color = mix(vec4(fog.color.xyz, color.a), color, passColor.a);
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

void paintSun(inout vec4 color)
{
    if (atmosphere.enabled)
    {
        color = vec4(0.0);
        return;
    }
    color = texture2D(diffuseMap, diffuseMapUV);
    color.a *= opacity;
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

    gl_FragData[0] = color;
}
