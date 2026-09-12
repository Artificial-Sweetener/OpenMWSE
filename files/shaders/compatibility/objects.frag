#version 120

#pragma import_defines(FORCE_OPAQUE, DISTORTION)

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#if @diffuseMap
varying vec2 diffuseMapUV;
#endif

#if @darkMap
varying vec2 darkMapUV;
#endif

#if @detailMap
varying vec2 detailMapUV;
#endif

#if @decalMap
varying vec2 decalMapUV;
#endif

#if @emissiveMap
varying vec2 emissiveMapUV;
#endif

#if @normalMap
varying vec2 normalMapUV;
#endif

#if @envMap
varying vec2 envMapUV;
#endif

#if @specularMap
varying vec2 specularMapUV;
#endif

#if @bumpMap
varying vec2 bumpMapUV;
#endif

#if @glossMap
varying vec2 glossMapUV;
#endif

uniform vec2 screenRes;
uniform float near;
uniform float far;
uniform float distortionStrength;
uniform float alpha;
uniform float actorFade;

centroid varying vec4 passColor;

#if !@forcePPL
centroid varying vec3 shadedLighting;
centroid varying vec3 shadedSpecular;
centroid varying vec3 passLighting;
centroid varying vec3 passSpecular;
#else
#include "lib/light/clamp.glsl"
#endif
varying vec3 passViewPos;
varying vec3 passNormal;
#if @normalMap || @diffuseParallax
varying vec4 passTangent;
#endif

#if @additiveBlending
#define ADDITIVE_BLENDING
#endif

#include "lib/core/fragment.h.glsl"
#include "lib/material/parallax.glsl"
#include "lib/material/alpha.glsl"
#include "lib/util/distortion.glsl"

#include "fog.glsl"
#include "lib/material/vertexcolors.glsl"
#include "shadows_fragment.glsl"
#include "compatibility/normals.glsl"

#if @softParticles
#include "lib/particle/soft.glsl"

uniform float particleSize;
uniform bool particleFade;
uniform float softFalloffDepth;
#endif

#if @particleOcclusion
#include "lib/particle/occlusion.glsl"
uniform sampler2D orthoDepthMap;
varying vec3 orthoDepthMapCoord;
#endif

void main()
{
#if @particleOcclusion
    applyOcclusionDiscard(orthoDepthMapCoord, texture2D(orthoDepthMap, orthoDepthMapCoord.xy * 0.5 + 0.5).r);
#endif

    Material material = getMaterial();

    // only offset diffuse and normal maps for now, other textures are more likely to be using a completely different UV set
    vec2 offset = vec2(0.0);

#if @parallax || @diffuseParallax
#if @parallax
    float height = sampleNormalMap(normalMapUV).a;
#else
    float height = sampleDiffuseMap(diffuseMapUV).a;
#endif
    offset = getParallaxOffset(transpose(normalToViewMatrix) * normalize(-passViewPos), height);
#endif

vec2 screenCoords = gl_FragCoord.xy / screenRes;

#if @diffuseMap
    if (hasMap(@diffuseMapIndex))
    {
        gl_FragData[0] = sampleDiffuseMap(diffuseMapUV + offset);

#if defined(DISTORTION) && DISTORTION
        gl_FragData[0].a *= getDiffuseColor(material, passColor).a;
        gl_FragData[0] = applyDistortion(gl_FragData[0], distortionStrength, gl_FragCoord.z, sampleOpaqueDepthTex(screenCoords / @distorionRTRatio).x);
        return;
#endif

#if @diffuseParallax
        gl_FragData[0].a = 1.0;
#else
        gl_FragData[0].a *= mapCoverageScale(@diffuseMapIndex, diffuseMapUV + offset);
#endif
    }
    else
        gl_FragData[0] = vec4(1.0);
#else
    gl_FragData[0] = vec4(1.0);
#endif

    vec4 diffuseColor = getDiffuseColor(material, passColor);
    gl_FragData[0].a *= diffuseColor.a * alpha * actorFade;

#if @darkMap
    if (hasMap(@darkMapIndex))
    {
        gl_FragData[0] *= sampleDarkMap(darkMapUV);
        gl_FragData[0].a *= mapCoverageScale(@darkMapIndex, darkMapUV);
    }
#endif

    gl_FragData[0].a = alphaTest(gl_FragData[0].a, getAlphaRef());

    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#if @normalMap
    if (hasMap(@normalMapIndex))
    {
        vec4 normalTex = sampleNormalMap(normalMapUV + offset);
        vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
        normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
        viewNormal = normalToView(normal);
    }
#endif

    vec3 viewVec = normalize(passViewPos);

#if @detailMap
    if (hasMap(@detailMapIndex))
        gl_FragData[0].xyz *= sampleDetailMap(detailMapUV).xyz * 2.0;
#endif

#if @decalMap
    if (hasMap(@decalMapIndex))
    {
        vec4 decalTex = sampleDecalMap(decalMapUV);
        gl_FragData[0].xyz = mix(gl_FragData[0].xyz, decalTex.xyz, decalTex.a * diffuseColor.a);
    }
#endif

#if @envMap
    vec3 envEffect = vec3(0.0);
    if (hasMap(@envMapIndex))
    {
        vec2 envTexCoordGen = envMapUV;
        float envLuma = 1.0;

#if @normalMap
        // if using normal map + env map, take advantage of per-pixel normals for envTexCoordGen
        if (hasMap(@normalMapIndex))
        {
            vec3 r = reflect( viewVec, viewNormal );
            float m = 2.0 * sqrt( r.x*r.x + r.y*r.y + (r.z+1.0)*(r.z+1.0) );
            envTexCoordGen = vec2(r.x/m + 0.5, r.y/m + 0.5);
        }
#endif

#if @bumpMap
        if (hasMap(@bumpMapIndex))
        {
            vec4 bumpTex = sampleBumpMap(bumpMapUV);
            envTexCoordGen += bumpTex.rg * getBumpMapMatrix();
            envLuma = clamp(bumpTex.b * getEnvMapLumaBias().x + getEnvMapLumaBias().y, 0.0, 1.0);
        }
#endif

        envEffect = sampleEnvMap(envTexCoordGen).xyz * getEnvMapColor().xyz * envLuma;

#if @glossMap
        if (hasMap(@glossMapIndex))
            envEffect *= sampleGlossMap(glossMapUV).xyz;
#endif

#if @preLightEnv
        gl_FragData[0].xyz += envEffect;
#endif
    }
#endif

    vec3 diagnosticSampledSurfaceColor = gl_FragData[0].rgb;
    float shadowing = unshadowedLightRatio(-passViewPos.z);
    vec3 lighting, specular;
#if !@forcePPL
    lighting = mix(shadedLighting, passLighting, shadowing);
    specular = mix(shadedSpecular, passSpecular, shadowing);
#else
    float shininess = material.shininess;
    vec3 specularColor = getSpecularColor(material, passColor).xyz;
#if @specularMap
    if (hasMap(@specularMapIndex))
    {
        vec4 specTex = sampleSpecularMap(specularMapUV);
        shininess = specTex.a * 255.0;
        specularColor = specTex.xyz;
    }
#endif
    vec3 diffuseLight, ambientLight, specularLight;

    doLighting(gl_FragCoord.xy, passViewPos, viewNormal, shininess, shadowing, diffuseLight, ambientLight, specularLight);

    lighting = diffuseColor.xyz * diffuseLight + getAmbientColor(material, passColor).xyz * ambientLight + getEmissionColor(material, passColor).xyz * material.emissiveMult;
    specular = specularColor * specularLight * material.specStrength;
    clampLighting(lighting);
#endif

    gl_FragData[0].xyz = gl_FragData[0].xyz * lighting + specular;

    if (openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticPreFogRadiance
        || openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticSampledSurfaceColor)
    {
        vec3 diagnosticColor = openmwseAtmosphereDiagnosticMode == openmwseAtmosphereDiagnosticSampledSurfaceColor
            ? diagnosticSampledSurfaceColor
            : gl_FragData[0].rgb;
        gl_FragData[0] = vec4(diagnosticColor, 1.0);
#if !defined(FORCE_OPAQUE) && !@disableNormals
        gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif
        return;
    }

#if @envMap && !@preLightEnv
    gl_FragData[0].xyz += envEffect;
#endif

#if @emissiveMap
    if (hasMap(@emissiveMapIndex))
        gl_FragData[0].xyz += sampleEmissiveMap(emissiveMapUV).xyz;
#endif

    gl_FragData[0] = applyFogAtPos(gl_FragData[0], passViewPos, near, far);

#if !defined(FORCE_OPAQUE) && @softParticles
    gl_FragData[0].a *= calcSoftParticleFade(
        viewVec,
        passViewPos,
        viewNormal,
        near,
        far,
        sampleOpaqueDepthTex(screenCoords).x,
        particleSize,
        particleFade,
        softFalloffDepth,
        waterEnabled,
        isReflection,
        waterHeight,
        osg_ViewMatrixInverse
    );
#endif

#if defined(FORCE_OPAQUE) && FORCE_OPAQUE
    // having testing & blending isn't enough - we need to write an opaque pixel to be opaque
    gl_FragData[0].a = 1.0;
#endif

#if !defined(FORCE_OPAQUE) && !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

    applyShadowDebugOverlay();
}
