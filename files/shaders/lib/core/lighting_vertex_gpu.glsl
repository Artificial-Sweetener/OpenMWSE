#version 430 compatibility

#include "lib/core/lighting_vertex_impl.glsl"

flat out uvec2 gpuLightRangeData;

uvec2 gpuLightRange()
{
    return gpuLightRangeData;
}
