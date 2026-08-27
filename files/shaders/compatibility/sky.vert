#version 120

#include "lib/core/vertex.h.glsl"

#include "lib/sky/passes.glsl"

uniform int pass;
uniform mat4 texMat0;

varying vec4 passColor;
varying vec2 diffuseMapUV;
varying vec3 passViewDirection;

void main()
{
    vec4 viewPosition = modelToView(gl_Vertex);

    gl_Position = viewToClip(viewPosition);
    passColor = gl_Color;
    passViewDirection = viewPosition.xyz;

    if (pass == PASS_CLOUDS)
        diffuseMapUV = (texMat0 * gl_MultiTexCoord0).xy;
    else
        diffuseMapUV = gl_MultiTexCoord0.xy;
}
