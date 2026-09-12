#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

#if @diffuseMap
varying vec2 diffuseMapUV;
#endif

#if @emissiveMap
varying vec2 emissiveMapUV;
#endif

#if @normalMap
varying vec2 normalMapUV;
varying vec4 passTangent;
#endif

varying float euclideanDepth;
varying float linearDepth;

varying vec3 passViewPos;
varying vec3 passNormal;

#include "lib/view/depth.glsl"
#include "lib/material/vertexcolors.glsl"

#include "compatibility/shadows_vertex.glsl"
#include "compatibility/normals.glsl"

centroid varying vec4 passColor;

void main(void)
{
    vec4 vertex = instanceWorld() * gl_Vertex;
    gl_Position = modelToClip(vertex);

    vec4 viewPos = modelToView(vertex);
    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);
    passColor = gl_Color;
    passViewPos = viewPos.xyz;
    passNormal = instanceNormal() * gl_Normal.xyz;
    normalToViewMatrix = gl_NormalMatrix;

#if @normalMap
    passTangent = instanceTangent(gl_MultiTexCoord7);
    normalToViewMatrix *= generateTangentSpace(passTangent, passNormal);
#endif

#if @diffuseMap
    diffuseMapUV = mapUV(@diffuseMapIndex, @diffuseMapUV, gl_MultiTexCoord@diffuseMapUV);
#endif

#if @emissiveMap
    emissiveMapUV = mapUV(@emissiveMapIndex, @emissiveMapUV, gl_MultiTexCoord@emissiveMapUV);
#endif

#if @normalMap
    normalMapUV = mapUV(@normalMapIndex, @normalMapUV, gl_MultiTexCoord@normalMapUV);
#endif


#if @shadows_enabled
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
    setupShadowCoords(viewPos, viewNormal);
#endif
}
