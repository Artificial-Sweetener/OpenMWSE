#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/fragment.h.glsl"


#if @diffuseMap
varying vec2 diffuseMapUV;
#endif

#if @normalMap
varying vec2 normalMapUV;
#endif

varying float euclideanDepth;
varying float linearDepth;
uniform vec2 screenRes;
uniform float near;
uniform float far;

// World-unit fade interval shared with CPU groundcover visibility.
uniform vec2 groundcoverFade;

#if @forcePPL
#include "lib/light/clamp.glsl"
#else
centroid varying vec3 shadedLighting;
centroid varying vec3 passLighting;
#endif

varying vec3 passNormal;
varying vec3 passViewPos;

#include "shadows_fragment.glsl"
#include "lib/material/alpha.glsl"
#include "fog.glsl"
#include "lib/water/caustics.glsl"
#include "compatibility/normals.glsl"

centroid varying vec4 passColor;

void main()
{
    Material material = getMaterial();

#if @diffuseMap
    gl_FragData[0] = sampleDiffuseMap(diffuseMapUV);
#else
    gl_FragData[0] = vec4(1.0);
#endif

    if (euclideanDepth >= groundcoverFade.y)
        discard;
    if (euclideanDepth > groundcoverFade.x)
        gl_FragData[0].a *= 1.0-smoothstep(groundcoverFade.x, groundcoverFade.y, euclideanDepth);

    gl_FragData[0].a = alphaTest(gl_FragData[0].a, getAlphaRef());

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
    }
#endif

    float shadowing = unshadowedLightRatio(linearDepth);

    vec3 lighting;
#if !@forcePPL
    lighting = mix(shadedLighting, passLighting, shadowing);
#else
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(gl_FragCoord.xy, passViewPos, viewNormal, material.shininess, shadowing, diffuseLight, ambientLight, specularLight);
    lighting = diffuseLight + ambientLight;
    clampLighting(lighting);
#endif

    gl_FragData[0].xyz *= lighting;
    gl_FragData[0].xyz = applyWaterCaustics(gl_FragData[0].xyz, passViewPos, viewNormal, shadowing);
    gl_FragData[0] = applyFogAtDist(gl_FragData[0], passViewPos, euclideanDepth, linearDepth, near, far);

#if !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

    applyShadowDebugOverlay();
}
