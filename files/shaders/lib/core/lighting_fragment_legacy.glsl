#version 120

#if @gpuInstanced
flat in uint gpuChunkId;

uint gpuLightChunk()
{
    return gpuChunkId;
}
#endif

#include "lib/core/lighting_fragment_impl.glsl"
