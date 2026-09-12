#version 120

varying vec2 diffuseMapUV;
varying float alphaPassthrough;

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"
#include "lib/material/vertexcolors.glsl"

void main()
{
    Material material = getMaterial();
    vec4 vertex = instanceWorld() * gl_Vertex;
    gl_Position = modelToClip(vertex);

    vec4 viewPos = modelToView(vertex);
    gl_ClipVertex = viewPos;

    if (material.vertexColorMode == ColorMode_AmbientAndDiffuse)
        alphaPassthrough = gl_Color.a;
    else
        alphaPassthrough = material.diffuse.a;

    diffuseMapUV = mapUV(@diffuseMapIndex, 0, gl_MultiTexCoord0);
}
