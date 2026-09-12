#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

#if @diffuseMap
varying vec2 diffuseMapUV;
#endif

varying vec3 passNormal;
varying vec3 passViewPos;
varying float euclideanDepth;
varying float linearDepth;
varying float passFalloff;

centroid varying vec4 passColor;

uniform bool useFalloff;
uniform vec4 falloffParams;

#include "lib/view/depth.glsl"
#include "lib/material/vertexcolors.glsl"

#include "compatibility/shadows_vertex.glsl"

void main(void)
{
    vec4 vertex = instanceWorld() * gl_Vertex;
    gl_Position = modelToClip(vertex);

    vec4 viewPos = modelToView(vertex);
    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

#if @diffuseMap
    diffuseMapUV = mapUV(@diffuseMapIndex, @diffuseMapUV, gl_MultiTexCoord@diffuseMapUV);
#endif

    passColor = gl_Color;
    passViewPos = viewPos.xyz;
    passNormal = instanceNormal() * gl_Normal.xyz;

    if (useFalloff)
    {
        vec3 viewNormal = gl_NormalMatrix * normalize(passNormal);
        vec3 viewDir = normalize(viewPos.xyz);
        float viewAngle = abs(dot(viewNormal, viewDir));
        passFalloff = smoothstep(falloffParams.x, falloffParams.y, viewAngle);

        float startOpacity = min(falloffParams.z, 1.0);
        float stopOpacity = max(falloffParams.w, 0.0);

        passFalloff = mix(startOpacity, stopOpacity, passFalloff);
    }
    else
    {
        passFalloff = 1.0;
    }

#if @shadows_enabled
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
    setupShadowCoords(viewPos, viewNormal);
#endif
}
