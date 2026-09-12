#version 120

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

void main(void)
{
    vec4 vertex = instanceWorld() * gl_Vertex;
    gl_Position = modelToClip(vertex);
    gl_ClipVertex = modelToView(vertex);
}
