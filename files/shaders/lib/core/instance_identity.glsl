#version 120

#include "lib/core/instance.h.glsl"

mat4 instanceWorld()
{
    return mat4(1.0);
}

mat3 instanceNormal()
{
    return mat3(1.0);
}

vec4 instanceTangent(vec4 tangent)
{
    return tangent;
}

vec3 instanceWorldOffset(vec3 nodePosition)
{
    return nodePosition;
}
