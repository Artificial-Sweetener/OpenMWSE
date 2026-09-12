#version 120

#if @gpuInstanced
#include "lib/core/instancebuffer.glsl"

uint gpuLightChunk()
{
    return instances[instanceSlot].meta.y;
}
#endif

#include "lib/core/lighting_vertex_impl.glsl"
