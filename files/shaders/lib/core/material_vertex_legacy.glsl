#version 120

#include "lib/core/material_vertex.h.glsl"

uniform Material material;

uniform mat4 texMat0;
uniform mat4 texMat1;
uniform mat4 texMat2;
uniform mat4 texMat3;
uniform mat4 texMat4;
uniform mat4 texMat5;
uniform mat4 texMat6;
uniform mat4 texMat7;

Material getMaterial()
{
    return material;
}

mat4 texMat(int unit)
{
    if (unit == 0)
        return texMat0;
    if (unit == 1)
        return texMat1;
    if (unit == 2)
        return texMat2;
    if (unit == 3)
        return texMat3;
    if (unit == 4)
        return texMat4;
    if (unit == 5)
        return texMat5;
    if (unit == 6)
        return texMat6;
    return texMat7;
}

vec2 mapUV(int map, int unit, vec4 coord)
{
    return (texMat(unit) * coord).xy;
}
