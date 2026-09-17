#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/fragment.h.glsl"

// Inspired by Blender GLSL Water by martinsh ( https://devlog-martinsh.blogspot.de/2012/07/waterundewater-shader-wip.html )
// OpenMWSE ports the MGE XE water at revision 2697fdfa7fd5af48a321a8b6cee3303a0a256690,
// Enhanced Water Shader 2.2 by Krokantor and credited contributors, and Pixel Shader Style Water 1.0
// by Hrnchamd with permission and attribution. The OpenMW path remains unchanged.

// tweakables -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

const float REFL_BUMP = 0.10;                      // reflection distortion amount

#if @sunlightScattering
const float SCATTER_AMOUNT = 0.3;                  // amount of sunlight scattering
const vec3 SCATTER_COLOUR = vec3(0.0,1.0,0.95);    // colour of sunlight scattering
const vec3 SUN_EXT = vec3(0.45, 0.55, 0.68);       // sunlight extinction
#endif

const float SUN_SPEC_FADING_THRESHOLD = 0.15;       // visibility at which sun specularity starts to fade
const float SPEC_HARDNESS = 256.0;                 // specular highlights hardness
const float SPEC_BUMPINESS = 5.0;                  // surface bumpiness boost for specular
const float SPEC_BRIGHTNESS = 1.5;                 // boosts the brightness of the specular highlights

const float VISIBILITY = 2500.0;
const float BUMP_SUPPRESS_DEPTH = 300.0;           // at what water depth bumpmap will be suppressed for reflections and refractions (prevents artifacts at shores)
const float REFR_FOG_DISTORT_DISTANCE = 3000.0;    // at what distance refraction fog will be calculated using real water depth instead of distorted depth (prevents splotchy shores)

#if @wobblyShores
const float WOBBLY_SHORE_FADE_DISTANCE = 6200.0;   // fade out wobbly shores to mask precision errors, the effect is almost impossible to see at a distance
#endif

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

uniform sampler2D rippleMap;
uniform sampler3D mgeWaterVolume;
uniform vec3 playerPos;
uniform int waterAppearance;
uniform int enhancedWaterColor;
uniform bool enhancedWaterFoam;
uniform bool enhancedWaterDistantWaves;
uniform float windSpeed;
uniform float geometricWaveHeight;
uniform vec3 nodePosition;

varying vec3 worldPos;

varying vec2 rippleMapUV;

varying vec4 position;
varying vec3 passViewPos;
varying float linearDepth;
#if @waterGeometricWaves
varying float enclosedSurfaceCoverage;
#endif

uniform float near;
uniform float far;

uniform float rainIntensity;

uniform vec2 screenRes;

#include "lib/water/fresnel.glsl"
#include "lib/water/rain_ripples.glsl"
#include "lib/view/depth.glsl"
#include "lib/light/struct.glsl"

#include "shadows_fragment.glsl"
#include "fog.glsl"
#include "lib/water/optical_field.glsl"

/// Return whether one normalized depth value represents untouched framebuffer clear depth.
bool isClearOpaqueDepth(float depth)
{
#if @reverseZ
    const float clearDepth = 0.0;
#else
    const float clearDepth = 1.0;
#endif
    return abs(depth - clearDepth) <= 0.000001;
}

/// Return whether a complete 3-by-3 footprint contains receiver geometry rather than clear sky.
bool hasOpaqueGeometry(vec2 coords)
{
    vec2 texel = vec2(1.0) / screenRes;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            if (isClearOpaqueDepth(sampleOpaqueDepthTex(coords + vec2(float(x), float(y)) * texel).r))
                return false;
    return true;
}

/// Convert opaque depth to positive view-axis water depth while preserving MGE XE's clear sentinel.
float waterViewDepth(vec2 coords, float surfaceDepth)
{
    float opaqueDepth = sampleOpaqueDepthTex(coords).r;
    if (isClearOpaqueDepth(opaqueDepth))
        return 1e38;
    return max(linearizeDepth(opaqueDepth, near, far) - surfaceDepth, 0.0);
}

/// Convert an MGE XE homogeneous screen-space offset into OpenGL texture coordinates.
vec2 mgeProjectedOffset(vec2 d3dOffset, float clipW)
{
    return vec2(d3dOffset.x, -d3dOffset.y) / max(clipW, 1.0);
}

void main(void)
{
    float shadow = unshadowedLightRatio(linearDepth);

    vec2 screenCoords = gl_FragCoord.xy / screenRes;
    // The reflection target is screen-space. Deriving coordinates from displaced vertices stretches the target across
    // the sparse horizon ring and creates a camera-centered near/far discontinuity.
    vec2 authoredReflectionCoords = screenCoords;

    #define waterTimer osg_SimulationTime

    vec4 rainRipple;

    if (rainIntensity > 0.01)
        rainRipple = rainCombined(position.xy/1000.0, waterTimer) * clamp(rainIntensity, 0.0, 1.0);
    else
        rainRipple = vec4(0.0);

    vec3 rippleAdd = rainRipple.xyz * 10.0;

    float distToCenter = length(rippleMapUV - vec2(0.5));
    float blendClose = smoothstep(0.001, 0.02, distToCenter);
    float blendFar = 1.0 - smoothstep(0.3, 0.4, distToCenter);
    float distortionLevel = 2.0;
    rippleAdd += distortionLevel * vec3(texture2D(rippleMap, rippleMapUV).ba * blendFar * blendClose, 0.0);

    WaterOpticalSample opticalField = sampleWaterOpticalField(worldPos.xy, waterTimer, waterAppearance, playerPos.xy,
        enhancedWaterDistantWaves, rippleAdd, rainIntensity);
    vec3 normal = opticalField.normal;

    vec3 sunWorldDir = normalize((gl_ModelViewMatrixInverse * sun.position).xyz);
    vec3 cameraPos = (gl_ModelViewMatrixInverse * vec4(0,0,0,1)).xyz;
    vec3 viewDir = normalize(position.xyz - cameraPos.xyz);

    float sunFade = length(sun.ambient.xyz);

#if @radialFog
    float radialDepth = distance(position.xyz, cameraPos);
#else
    float radialDepth = 0.0;
#endif

    float fogScale;
    vec3 fogOffset;
    computeFog(passViewPos, radialDepth, linearDepth, near, far, fogScale, fogOffset);

    // fresnel
    float ior = cameraPos.z >= waterHeight ? 1.333 : 1.0 / 1.333;
    float fresnel = clamp(fresnel_dielectric(viewDir, normal, ior), 0.0, 1.0);

    vec2 reflectionCoordsOffset = normal.xy * REFL_BUMP;
    vec2 refractionCoordsOffset = vec2(0.0);
    float surfaceDepth = linearizeDepth(gl_FragCoord.z, near, far);
    float realWaterDepth = waterViewDepth(screenCoords, surfaceDepth);

#if @waterRefraction
    refractionCoordsOffset = reflectionCoordsOffset;
    float depthSampleDistorted = waterViewDepth(screenCoords - refractionCoordsOffset, surfaceDepth);
    // Don't sample refraction from above water
    if (depthSampleDistorted <= 0.0)
        refractionCoordsOffset = vec2(0.0);
    refractionCoordsOffset *= clamp(realWaterDepth / BUMP_SUPPRESS_DEPTH, 0.0, 1.0);
#endif
    // reflection
    vec3 reflection = sampleReflectionMap(screenCoords + reflectionCoordsOffset).rgb * fogScale + fogOffset;

    vec3 waterColor = waterInScatteringColor();

    vec4 sunSpec = sun.specular;
    // alpha component is sun visibility; we want to start fading lighting effects when visibility is low
    sunSpec.a = min(1.0, sunSpec.a / SUN_SPEC_FADING_THRESHOLD);

    // specular
    const float SPEC_MAGIC = 1.55; // from the original blender shader, changing it makes the spec vanish or become too bright

    vec3 specNormal = normalize(vec3(normal.x * SPEC_BUMPINESS, normal.y * SPEC_BUMPINESS, normal.z));
    vec3 viewReflectDir = reflect(viewDir, specNormal);
    float phongTerm = max(dot(viewReflectDir, sunWorldDir), 0.0);
    float specular = pow(atan(phongTerm * SPEC_MAGIC), SPEC_HARDNESS) * SPEC_BRIGHTNESS;
    specular = clamp(specular, 0.0, 1.0) * shadow * sunSpec.a;

    // artificial specularity to make rain ripples more noticeable
    vec3 rainSpecular = abs(rainRipple.w) * mix(max(reflection, vec3(0.0)), vec3(1.0), 0.05) * 0.5;
    float waterTransparency = clamp(fresnel * 6.0 + specular, 0.0, 1.0);

    vec3 refraction = waterColor;
    vec3 rawRefraction = refraction;

    vec2 refractionCoords = screenCoords - refractionCoordsOffset;
#if @waterRefraction
    refraction = sampleOpaqueColorTex(refractionCoords).rgb;
#endif
    rawRefraction = refraction;

    // brighten up the refraction underwater
    if (cameraPos.z < waterHeight)
        refraction = clamp(refraction * 1.5, 0.0, 1.0);

    if (cameraPos.z >= waterHeight)
    {
        vec3 deepWaterColor = waterColor * fogScale + fogOffset;
        if (!hasOpaqueGeometry(refractionCoords))
            refraction = deepWaterColor;
        else
            refraction = mix(refraction, deepWaterColor, clamp(realWaterDepth / VISIBILITY - 1.0, 0.0, 1.0));
    }

#if @sunlightScattering
    vec3 scatterNormal = opticalField.scatterNormal;
    float sunHeight = sunWorldDir.z;
    vec3 scatterColour = mix(SCATTER_COLOUR * vec3(1.0, 0.4, 0.0), SCATTER_COLOUR, max(1.0 - exp(-sunHeight * SUN_EXT), 0.0));
    float scatterLambert = max(dot(sunWorldDir, scatterNormal) * 0.7 + 0.3, 0.0);
    float scatterReflectAngle = max(dot(reflect(sunWorldDir, scatterNormal), viewDir) * 2.0 - 1.2, 0.0);
    float lightScatter = scatterLambert * scatterReflectAngle * SCATTER_AMOUNT * sunFade * sunSpec.a * max(1.0 - exp(-sunHeight), 0.0);
    refraction = mix(refraction, scatterColour * fogScale + fogOffset, lightScatter);
#endif

    gl_FragData[0].rgb = mix(refraction, reflection, fresnel);
    gl_FragData[0].a = 1.0;
    // no alpha here, so make sure raindrop ripple specularity gets properly subdued
    rainSpecular *= waterTransparency;

    vec3 pointSpecular = doSpecularLighting(gl_FragCoord.xy, (gl_ModelViewMatrix * vec4(position.xyz, 1.0)).xyz, normalize(gl_NormalMatrix * specNormal));
    pointSpecular *= SPEC_BRIGHTNESS;

    gl_FragData[0].rgb += (specular * sunSpec.rgb + rainSpecular + pointSpecular) * fogScale;

    if (waterAppearance == 1)
    {
        float distanceToEye = length(position.xyz - cameraPos);
        float sunVisibility = clamp(sun.specular.a, 0.0, 1.0);
        float sunlightFactor = 1.0 - pow(1.0 - sunVisibility, 2.0);
        vec3 sunColorAdjusted = sun.diffuse.rgb * sunlightFactor;
        vec3 depthBaseColor = sunColorAdjusted * vec3(0.03, 0.04, 0.05)
            + (2.0 * atmosphere.skyColor.rgb + fog.color.rgb) * vec3(0.075, 0.08, 0.085);
        vec3 mgeDepthColor = depthBaseColor * fogScale + fogOffset;
        float mgeWind = (abs(windSpeed) + 1.5) / 140.0;
        vec2 refFactor = (mgeWind * distanceToEye + 0.1) * normal.xy;
        vec2 firstOffset = mgeProjectedOffset(refFactor.yx, surfaceDepth);
        float viewDepth = waterViewDepth(screenCoords + firstOffset, surfaceDepth);
        vec3 mgeRefraction = mgeDepthColor;
        float shoreFactor = 0.0;
        if (viewDepth < 4000.0)
        {
            vec2 refractionOffset = mgeProjectedOffset(
                clamp(viewDepth / 100.0, 0.0, 1.0) * refFactor.yx, surfaceDepth);
            vec2 refractionCoord = screenCoords + refractionOffset;
#if @waterRefraction
            mgeRefraction = sampleOpaqueColorTex(refractionCoord).rgb;
#endif
            viewDepth = waterViewDepth(refractionCoord, surfaceDepth);
            vec3 cameraForward = normalize((gl_ModelViewMatrixInverse * vec4(0.0, 0.0, -1.0, 0.0)).xyz);
            float waterDepth = viewDepth / max(dot(viewDir, cameraForward), 0.0001);
            waterDepth += 300.0 * (0.95 - normal.z);
            float depthScale = clamp(exp(-waterDepth / 800.0), 0.0, 1.0);
            shoreFactor = pow(depthScale, 90.0);
            mgeRefraction = mix(mgeDepthColor, mgeRefraction, 0.8 * depthScale + 0.2 * shoreFactor);
        }
        vec2 reflectionOffset = mgeProjectedOffset(
            vec2(-2.1 * refFactor.x, abs(refFactor.y)), surfaceDepth);
        vec3 mgeReflection = sampleReflectionMap(authoredReflectionCoords + reflectionOffset).rgb;
        mgeReflection = mix(fogOffset, mgeReflection, fogScale);
        vec3 adjustedNormal = mix(vec3(0.0, 0.0, 0.1), normal, pow(clamp(1.05 * fogScale, 0.0, 1.0), 2.0));
        adjustedNormal = mix(adjustedNormal, vec3(0.0, 0.0, 1.0),
            (1.0 + viewDir.z) * (1.0 - clamp(1.0 / (distanceToEye / 1000.0 + 1.0), 0.0, 1.0)));
        float mgeFresnel = dot(-viewDir, adjustedNormal);
        mgeFresnel = 0.02 + pow(clamp(0.9988 - 0.28 * mgeFresnel, 0.0, 1.0), 16.0);
        float mgeSpecularAngle = clamp(1.0025 * dot(-viewDir, reflect(-sunWorldDir, normal)), 0.0, 1.0);
        vec3 mgeSpecular = sunColorAdjusted
            * (pow(mgeSpecularAngle, 170.0) + 0.07 * pow(mgeSpecularAngle, 4.0));
        vec3 mgeResult = mix(mgeRefraction, mgeReflection, mgeFresnel) + mgeSpecular * fogScale;
        gl_FragData[0].rgb = mix(mgeResult, mgeRefraction, shoreFactor * fogScale);
    }
    else if (waterAppearance == 2)
    {
        vec3 enhancedGreen = enhancedWaterColor == 1 ? vec3(0.08, 0.16, 0.21)
            : (enhancedWaterColor == 2 ? vec3(0.11, 0.19, 0.12) : vec3(0.09, 0.22, 0.17));
        vec3 enhancedBlue = enhancedWaterColor == 1 ? vec3(0.04, 0.13, 0.29)
            : (enhancedWaterColor == 2 ? vec3(0.07, 0.19, 0.20) : vec3(0.07, 0.13, 0.29));
        vec3 enhancedShallow = enhancedWaterColor == 1 ? vec3(0.14, 0.42, 0.44)
            : (enhancedWaterColor == 2 ? vec3(0.12, 0.73, 0.10) : vec3(0.15, 0.70, 0.10));
        vec3 enhancedDeep = enhancedWaterColor == 1 ? vec3(0.10, 0.12, 0.30)
            : (enhancedWaterColor == 2 ? vec3(0.10, 0.19, 0.26) : vec3(0.10, 0.15, 0.30));
        float sunVisibility = clamp(sun.specular.a, 0.0, 1.0);
        float sunlightFactor = 1.0 - pow(1.0 - sunVisibility, 2.0);
        vec3 sunColorAdjusted = sun.diffuse.rgb * sunlightFactor;
        float distanceToEye = length(position.xyz - cameraPos);
        float enhancedWind = (abs(windSpeed) + 1.5) / 120.0;
        vec2 refFactor = 0.5 * (enhancedWind * distanceToEye + 0.1) * normal.xy;
        vec2 firstOffset = mgeProjectedOffset(refFactor.yx, surfaceDepth);
        float viewDepth = waterViewDepth(screenCoords + firstOffset, surfaceDepth);
        vec2 refractionOffset = mgeProjectedOffset(
            clamp(viewDepth / 150.0, 0.0, 1.0) * refFactor.yx, surfaceDepth);
        vec2 refractionCoord = screenCoords + refractionOffset;
        float depthFix = waterViewDepth(refractionCoord, surfaceDepth);
        float depthFixScale = clamp(1.0 - depthFix / 2500.0, 0.0, 1.0);
        vec3 enhancedWater = sunVisibility * mix(enhancedBlue, enhancedGreen, sun.ambient.rgb);
        enhancedWater += 0.13 * sunVisibility
            * mix(enhancedDeep, enhancedShallow, depthFixScale);
        enhancedWater = enhancedWater * fogScale + fogOffset;
        vec3 enhancedRefraction = enhancedWater;
        float depthScale = 0.0;
        float shoreFactor = 0.0;
        if (viewDepth < 10000.0)
        {
            vec2 aberration = reflect(viewDir, normal).xy;
#if @waterRefraction
            enhancedRefraction.r = sampleOpaqueColorTex(refractionCoord).r;
            enhancedRefraction.g = sampleOpaqueColorTex(
                refractionCoord + mgeProjectedOffset(-0.05 * aberration, surfaceDepth)).g;
            enhancedRefraction.b = sampleOpaqueColorTex(
                refractionCoord + mgeProjectedOffset(-0.1 * aberration, surfaceDepth)).b;
#endif
            float distortedViewDepth = waterViewDepth(refractionCoord, surfaceDepth);
            vec3 cameraForward = normalize((gl_ModelViewMatrixInverse * vec4(0.0, 0.0, -1.0, 0.0)).xyz);
            float waterDepth = distortedViewDepth / max(dot(viewDir, cameraForward), 0.0001);
            waterDepth += 50.0 * (0.99 - normal.z);
            depthScale = clamp(exp(-waterDepth / 800.0), 0.0, 1.0);
            shoreFactor = pow(depthScale, 90.0);
            enhancedRefraction = mix(
                enhancedWater, enhancedRefraction, 0.8 * depthScale + 0.2 * shoreFactor);
        }
        vec2 reflectionOffset = mgeProjectedOffset(
            vec2(-1.5 * refFactor.x, abs(refFactor.y)), surfaceDepth);
        vec3 enhancedReflection = sampleReflectionMap(authoredReflectionCoords + reflectionOffset).rgb;
        enhancedReflection = mix(fogOffset, enhancedReflection, fogScale);
        vec3 adjustedNormal = mix(vec3(0.0, 0.0, 0.1), normal, pow(clamp(1.05 * fogScale, 0.0, 1.0), 2.0));
        adjustedNormal = mix(adjustedNormal, vec3(0.0, 0.0, 1.0),
            (1.0 + viewDir.z) * (1.0 - clamp(1.0 / (distanceToEye / 1300.0 + 6.0), 0.0, 1.0)));
        float enhancedFresnel = dot(-viewDir, adjustedNormal);
        enhancedFresnel = 0.015 + pow(clamp(0.9988 - 0.28 * enhancedFresnel, 0.0, 1.0), 20.0);
        float enhancedSpecularAngle = clamp(1.0025 * dot(-viewDir, reflect(-sunWorldDir, normal)), 0.0, 1.0);
        vec3 enhancedResult = mix(enhancedRefraction, enhancedReflection, enhancedFresnel);
        vec3 enhancedSpecular = sunColorAdjusted
            * (pow(enhancedSpecularAngle, 200.0) + 0.07 * pow(enhancedSpecularAngle, 30.0));
        enhancedSpecular += 0.25 * pow(normal.z, 100.0) * sun.ambient.rgb / 12.0;
        enhancedSpecular = enhancedSpecular * fogScale + fogOffset;
        enhancedResult += enhancedSpecular * fogScale;
        if (enhancedWaterFoam && viewDepth < 10000.0)
        {
#if @waterGeometricWaves
            // Reuse the vertex-stage enclosure classification that already flattens fully covered surface mats.
            float foamVisibility = 1.0 - enclosedSurfaceCoverage;
#else
            float foamVisibility = 1.0;
#endif
            vec3 edgeNoiseColor = texture3D(mgeWaterVolume, vec3(worldPos.xy / 60.0, waterTimer * 0.2)).rgb;
            edgeNoiseColor = mix(vec3(-1.0), edgeNoiseColor * 1.29, fogScale * 0.9);
            float edgeNoise = dot(edgeNoiseColor, vec3(0.3, 0.3, 0.4));
            float edgeFoamFactor = pow(depthScale, 25.0);
            float edgeFoam = clamp(-5.0 * pow(edgeFoamFactor - 0.5, 2.0) + 1.0, 0.0, 1.0) * 0.5;
            edgeFoam *= edgeNoise > 1.0 - edgeFoam ? edgeNoise : edgeNoise / 3.0;
            vec3 waveNoiseColor = texture3D(mgeWaterVolume, vec3(worldPos.xy / 45.0, waterTimer)).rgb;
            waveNoiseColor = mix(vec3(-1.0), waveNoiseColor * 1.25, fogScale * 1.1);
            float waveNoise = dot(waveNoiseColor, vec3(0.25, 0.25, 0.5));
            float waveFoam = geometricWaveHeight > 0.0
                ? clamp((worldPos.z + 10.0 - waterHeight + 0.1 * geometricWaveHeight)
                    / max(0.6 * geometricWaveHeight, 0.001), 0.0, 1.0)
                : 0.0;
            waveFoam *= waveNoise > 1.0 - waveFoam ? waveNoise : waveNoise / 3.0;
            float randomFactor = 0.1 + fract(cos(dot(vec2(3.141592653589793),
                vec2(23.14069263277926, 2.665144142690225))) * 12345.6789);
            if (sunVisibility <= 0.0)
                enhancedResult += vec3(clamp(edgeFoam * 0.3
                    + waveFoam * (enhancedWind > 0.0235 ? 0.795 : 0.0), 0.0, 1.0) * foamVisibility);
            else
                enhancedResult += vec3(clamp(edgeFoam * clamp(atmosphere.skyColor.b, 0.0, 1.0)
                    * (randomFactor + 0.3), 0.0, 1.0) * foamVisibility);
        }
        enhancedResult = mix(enhancedResult, enhancedRefraction, shoreFactor * fogScale);
        float distanceFade = (1.0 + viewDir.z)
            * (1.0 - clamp(1.0 / max(distanceToEye / 3000.0, 0.0001), 0.0, 1.0));
        float fogFade = pow(clamp(mix(1.0, fogScale, distanceFade), 0.0, 1.0), 1.1);
        enhancedResult = mix(fogOffset, enhancedResult, fogFade);
        float waterCut = smoothstep(0.09, 0.1, distanceToEye / 1200.0);
        waterCut = (1.0 - clamp(abs(waterCut * 5.0), 0.0, 1.0))
            * step(0.5, 1.0 - clamp(cameraPos.z - waterHeight + 1.0, 0.0, 1.0));
        waterCut = 1.0 - clamp(waterCut, 0.0, 1.0);
        float waterCutDark = smoothstep(0.0889, 0.101, distanceToEye / 1200.0)
            * step(0.5, 1.0 - clamp(cameraPos.z - waterHeight + 1.0, 0.0, 1.0));
        waterCutDark = clamp(3.0 * waterCutDark * (1.0 - waterCutDark), 0.0, 1.0);
        enhancedResult = mix(enhancedRefraction, enhancedResult, waterCut);
        enhancedResult = mix(enhancedResult, enhancedResult * 0.1, waterCutDark);
        gl_FragData[0].rgb = enhancedResult;
    }
    else if (waterAppearance == 3)
    {
        float distanceToEye = length(position.xyz - cameraPos);
        float sunVisibility = clamp(sun.specular.a, 0.0, 1.0);
        float sunlightFactor = 1.0 - pow(1.0 - sunVisibility, 2.0);
        vec3 sunColorAdjusted = 0.4 * sun.diffuse.rgb * sunlightFactor;
        vec3 depthBaseColor = sunColorAdjusted * vec3(0.03, 0.04, 0.05)
            + (2.0 * atmosphere.skyColor.rgb + fog.color.rgb) * vec3(0.075, 0.08, 0.085);
        vec3 vanillaDepth = depthBaseColor * fogScale + fogOffset;
        float vanillaWind = (abs(windSpeed) + 1.5) / 140.0;
        vec2 refFactor = (vanillaWind * distanceToEye + 5.0) * normal.xy;
        vec2 refractionOffset = mgeProjectedOffset(0.2 * refFactor.yx, surfaceDepth);
        vec3 vanillaScene = vanillaDepth;
#if @waterRefraction
        vanillaScene = sampleOpaqueColorTex(screenCoords + refractionOffset).rgb;
#endif
        float shoreFactor = clamp(exp(-max(realWaterDepth, 0.0) / 4.0), 0.0, 1.0);
        float depthAttenuation = clamp(1.0 - max(realWaterDepth, 0.0) / 2700.0, 0.0, 1.0);
        vec3 vanillaRefraction = mix(vanillaDepth, vanillaScene,
            max(shoreFactor, clamp(depthAttenuation - 0.4, 0.0, 1.0)));
        vec2 reflectionOffset = mgeProjectedOffset(
            vec2(-12.0 * refFactor.x, 12.0 - 4.0 * refFactor.y), surfaceDepth);
        vec3 vanillaReflection = sampleReflectionMap(authoredReflectionCoords + reflectionOffset).rgb;
        vanillaReflection = mix(fogOffset, vanillaReflection, fogScale);
        float vanillaFresnel = 0.02
            + pow(clamp(0.9988 - 0.28 * (-viewDir.z), 0.0, 1.0), 6.0);
        float vanillaSpecularAngle = max(dot(-viewDir,
            vec3(sunWorldDir.x, sunWorldDir.y, -sunWorldDir.z)), 0.0);
        vec3 vanillaResult = mix(vanillaRefraction, vanillaReflection, vanillaFresnel);
        vanillaResult += sunColorAdjusted * normal.z * pow(vanillaSpecularAngle, 300.0) * fogScale;
        gl_FragData[0].rgb = mix(vanillaResult, vanillaRefraction, shoreFactor * fogScale);
    }

    if (waterAppearance != 0 && cameraPos.z < waterHeight)
    {
        vec3 underwaterNormal = -normal;
        float distanceToEye = length(position.xyz - cameraPos);
        float windDivisor = waterAppearance == 2 ? 120.0 : 140.0;
        float distortionBase = waterAppearance == 3 ? 5.0 : 0.1;
        float underwaterWind = (abs(windSpeed) + 1.5) / windDivisor;
        vec2 refFactor = 2.0 * (underwaterWind * distanceToEye + distortionBase) * underwaterNormal.xy;
        vec2 refractionOffset = mgeProjectedOffset(-2.0 * refFactor.xy, surfaceDepth);
        vec3 underwaterRefraction = fog.underwaterColor.rgb;
#if @waterRefraction
        underwaterRefraction = sampleOpaqueColorTex(screenCoords + refractionOffset).rgb;
#endif
        underwaterRefraction = mix(fog.color.rgb, underwaterRefraction, exp(-distanceToEye / 500.0));
        vec2 d3dReflectionOffset = waterAppearance == 3
            ? vec2(-4.5 * refFactor.x, -refFactor.y)
            : vec2(-2.1 * refFactor.x, abs(refFactor.y));
        vec3 underwaterReflection
            = sampleReflectionMap(authoredReflectionCoords + mgeProjectedOffset(d3dReflectionOffset, surfaceDepth)).rgb;
        float underwaterFresnel = pow(clamp(
            1.12 - 0.65 * dot(-viewDir, underwaterNormal), 0.0, 1.0), 8.0);
        vec3 underwaterResult = mix(underwaterRefraction, underwaterReflection, underwaterFresnel);
        float sunVisibility = clamp(sun.specular.a, 0.0, 1.0);
        float sunlightFactor = 1.0 - pow(1.0 - sunVisibility, 2.0);
        vec3 sunColorAdjusted = sun.diffuse.rgb * sunlightFactor * (waterAppearance == 3 ? 0.4 : 1.0);
        float refractedSun = max(dot(-viewDir, normalize(-sunWorldDir + underwaterNormal)), 0.0);
        vec3 underwaterSpecular
            = sunColorAdjusted * pow(refractedSun, 6.0) * clamp(exp(-distanceToEye / 4096.0), 0.0, 1.0);
        gl_FragData[0].rgb = underwaterResult + underwaterSpecular;
    }

#if @wobblyShores
    if (waterAppearance == 0)
    {
    // wobbly water: hard-fade into refraction texture at extremely low depth, with a wobble based on normal mapping
    float shoreRippleRain = sampleWaterOpenMWShoreNoise(
        worldPos.xy, waterTimer, opticalField.shoreLargeNormal, opticalField.shoreSmallNormal);
    float viewFactor = mix(abs(viewDir.z), 1.0, 0.2);
    float verticalWaterDepth = realWaterDepth * viewFactor; // an estimate
    float shoreOffset = verticalWaterDepth
        - (opticalField.shoreBaseNormal.r + mix(0.0, shoreRippleRain, rainIntensity) + 0.15) * 8.0;
    float fuzzFactor = min(1.0, 1000.0 / surfaceDepth) * viewFactor;
    shoreOffset *= fuzzFactor;
    shoreOffset = clamp(mix(shoreOffset, 1.0, clamp(linearDepth / WOBBLY_SHORE_FADE_DISTANCE, 0.0, 1.0)), 0.0, 1.0);
    gl_FragData[0].rgb = mix(rawRefraction, gl_FragData[0].rgb, shoreOffset);
    }
#endif

#if !@disableNormals
    gl_FragData[1].rgb = normalize(gl_NormalMatrix * normal) * 0.5 + 0.5;
#endif

    applyShadowDebugOverlay();
}
