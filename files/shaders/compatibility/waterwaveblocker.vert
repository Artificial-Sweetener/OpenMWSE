#version 120

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

uniform float waterWaveLevel;
uniform mat4 osg_ViewMatrixInverse;
varying float waterRelativeHeight;

/// Project authored geometry and classify its height in the shared world-space water frame.
void main()
{
    vec4 vertex = instanceWorld() * gl_Vertex;
    vec4 viewVertex = modelToView(vertex);
    vec4 worldVertex = osg_ViewMatrixInverse * viewVertex;
    waterRelativeHeight = worldVertex.z - waterWaveLevel;
    gl_Position = viewToClip(viewVertex);
}
