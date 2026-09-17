#version 120

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

uniform float waterWaveLevel;
varying float waterRelativeHeight;

/// Project authored terrain and static triangles into the water obstruction field.
void main()
{
    vec4 worldVertex = instanceWorld() * gl_Vertex;
    worldVertex.xyz += instanceWorldOffset(vec3(0.0));
    waterRelativeHeight = worldVertex.z - waterWaveLevel;
    gl_Position = modelToClip(worldVertex);
}
