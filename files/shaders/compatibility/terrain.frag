#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

varying vec2 uv;

varying float euclideanDepth;
varying float linearDepth;

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

uniform vec2 screenRes;
uniform float near;
uniform float far;

#include "lib/core/fragment.h.glsl"

#include "lib/material/vertexcolors.glsl"
#include "shadows_fragment.glsl"

#include "lib/material/parallax.glsl"
#include "fog.glsl"
#include "compatibility/normals.glsl"

centroid varying vec4 passColor;

void main()
{
    Material material = getMaterial();

    vec2 adjustedUV = transformUV(0, uv);

#if @parallax
    float height = sampleNormalMap(adjustedUV).a;
    adjustedUV += getParallaxOffset(transpose(normalToViewMatrix) * normalize(-passViewPos), height);
#endif
    vec4 diffuseTex = sampleDiffuseMap(adjustedUV);
    gl_FragData[0] = vec4(diffuseTex.xyz, 1.0);

    vec4 diffuseColor = getDiffuseColor(material, passColor);
    gl_FragData[0].a *= diffuseColor.a;

#if @blendMap
    vec2 blendMapUV = transformUV(1, uv);
    gl_FragData[0].a *= sampleBlendMap(blendMapUV).a;
#endif

#if @normalMap
    vec4 normalTex = sampleNormalMap(adjustedUV);
    vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
    normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
    vec3 viewNormal = normalToView(normal);
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

    float shadowing = unshadowedLightRatio(linearDepth);
    vec3 lighting, specular;
#if !@forcePPL
    lighting = mix(shadedLighting, passLighting, shadowing);
    specular = mix(shadedSpecular, passSpecular, shadowing);
#else
#if @specularMap
    float shininess = 128.0; // TODO: make configurable
    vec3 specularColor = vec3(diffuseTex.a);
#else
    float shininess = material.shininess;
    vec3 specularColor = getSpecularColor(material, passColor).xyz;
#endif
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(gl_FragCoord.xy, passViewPos, viewNormal, shininess, shadowing, diffuseLight, ambientLight, specularLight);
    lighting = diffuseColor.xyz * diffuseLight + getAmbientColor(material, passColor).xyz * ambientLight + getEmissionColor(material, passColor).xyz;
    specular = specularColor * specularLight;
    clampLighting(lighting);
#endif

    gl_FragData[0].xyz = gl_FragData[0].xyz * lighting + specular;

    gl_FragData[0] = applyFogAtDist(gl_FragData[0], passViewPos, euclideanDepth, linearDepth, near, far);

#if !@disableNormals
    // Terrain normals are now additively blended the same way as terrain color is, thus using color's alpha.
    gl_FragData[1] = vec4(viewNormal * 0.5 + 0.5, gl_FragData[0].a);
#endif

    applyShadowDebugOverlay();
}
