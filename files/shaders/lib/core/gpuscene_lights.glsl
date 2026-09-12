#ifndef LIB_CORE_GPUSCENE_LIGHTS
#define LIB_CORE_GPUSCENE_LIGHTS

struct LightRange
{
    uint first;
    uint count;
};

layout(std430, binding = 15) restrict readonly buffer LightRangeBuffer
{
    LightRange lightRanges[];
};

layout(std430, binding = 16) restrict readonly buffer LightBufferStorage
{
    mat4 gpuLights[];
};

#endif
