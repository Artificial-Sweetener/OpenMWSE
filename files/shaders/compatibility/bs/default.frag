#version 120
#pragma import_defines(FORCE_OPAQUE, DISTORTION)

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#if @diffuseMap
varying vec2 diffuseMapUV;
#endif

#if @emissiveMap
varying vec2 emissiveMapUV;
#endif

#if @normalMap
varying vec2 normalMapUV;
#endif

varying float euclideanDepth;
varying float linearDepth;

varying vec3 passViewPos;
varying vec3 passNormal;

uniform vec2 screenRes;
uniform float near;
uniform float far;
uniform bool useTreeAnim;
uniform float distortionStrength;

#include "lib/core/fragment.h.glsl"
#include "lib/material/alpha.glsl"
#include "lib/util/distortion.glsl"
#include "lib/light/clamp.glsl"
#include "lib/material/vertexcolors.glsl"

#include "compatibility/shadows_fragment.glsl"
#include "compatibility/fog.glsl"
#include "compatibility/normals.glsl"

centroid varying vec4 passColor;

void main()
{
    Material material = getMaterial();

#if @diffuseMap
    if (hasMap(@diffuseMapIndex))
    {
        gl_FragData[0] = sampleDiffuseMap(diffuseMapUV);

#if defined(DISTORTION) && DISTORTION
        vec2 screenCoords = gl_FragCoord.xy / (screenRes * @distorionRTRatio);
        gl_FragData[0].a *= getDiffuseColor(material, passColor).a;
        gl_FragData[0] = applyDistortion(gl_FragData[0], distortionStrength, gl_FragCoord.z, sampleOpaqueDepthTex(screenCoords).x);

        return;
#endif

        gl_FragData[0].a *= coveragePreservingAlphaScale(@diffuseMapIndex, diffuseMapUV);
    }
    else
        gl_FragData[0] = vec4(1.0);
#else
    gl_FragData[0] = vec4(1.0);
#endif

    vec4 diffuseColor = getDiffuseColor(material, passColor);
    if (!useTreeAnim)
        gl_FragData[0].a *= diffuseColor.a;
    gl_FragData[0].a = alphaTest(gl_FragData[0].a, getAlphaRef());

    vec3 specularColor = getSpecularColor(material, passColor).xyz;
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#if @normalMap
    if (hasMap(@normalMapIndex))
    {
        vec4 normalTex = sampleNormalMap(normalMapUV);
        vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
        normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
        viewNormal = normalToView(normal);
        specularColor *= normalTex.a;
    }
#endif

    float shadowing = unshadowedLightRatio(linearDepth);
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(gl_FragCoord.xy, passViewPos, viewNormal, material.shininess, shadowing, diffuseLight, ambientLight, specularLight);
    vec3 diffuse = diffuseColor.xyz * diffuseLight;
    vec3 ambient = getAmbientColor(material, passColor).xyz * ambientLight;
    vec3 emission = getEmissionColor(material, passColor).xyz * material.emissiveMult;
#if @emissiveMap
    if (hasMap(@emissiveMapIndex))
        emission *= sampleEmissiveMap(emissiveMapUV).xyz;
#endif
    vec3 lighting = diffuse + ambient + emission;
    vec3 specular = specularColor * specularLight * material.specStrength;

    clampLighting(lighting);

    gl_FragData[0].xyz = gl_FragData[0].xyz * lighting + specular;

    gl_FragData[0] = applyFogAtDist(gl_FragData[0], passViewPos, euclideanDepth, linearDepth, near, far);

#if defined(FORCE_OPAQUE) && FORCE_OPAQUE
    // having testing & blending isn't enough - we need to write an opaque pixel to be opaque
    gl_FragData[0].a = 1.0;
#endif

#if !defined(FORCE_OPAQUE) && !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

    applyShadowDebugOverlay();
}
