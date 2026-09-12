#version 430 compatibility

#include "lib/core/lighting_fragment_impl.glsl"

flat in uvec2 gpuLightRangeData;

uvec2 gpuLightRange()
{
    return gpuLightRangeData;
}
