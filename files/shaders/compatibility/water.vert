#version 120

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

varying vec4  position;
varying float linearDepth;

#include "shadows_vertex.glsl"
#include "lib/view/depth.glsl"

uniform vec3 nodePosition;
uniform vec3 playerPos;

varying vec3 worldPos;
varying vec2 rippleMapUV;
varying vec3 passViewPos;

void main(void)
{
    vec4 vertex = instanceWorld() * gl_Vertex;
    vec3 vertexNormal = instanceNormal() * gl_Normal.xyz;
    gl_Position = modelToClip(vertex);

    position = vertex;

    worldPos = vertex.xyz + instanceWorldOffset(nodePosition);
    rippleMapUV = (worldPos.xy - playerPos.xy + (@rippleMapSize * @rippleMapWorldScale / 2.0)) / @rippleMapSize / @rippleMapWorldScale;

    vec4 viewPos = modelToView(vertex);
    passViewPos = viewPos.xyz;
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    setupShadowCoords(viewPos, normalize(gl_NormalMatrix * vertexNormal));
}
