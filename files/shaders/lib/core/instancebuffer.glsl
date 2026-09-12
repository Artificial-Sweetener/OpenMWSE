#if @gpuInstanced
in uint instanceSlot;

#include "lib/core/gpuscene_types.glsl"

layout(std430, binding = 8) restrict readonly buffer InstanceBuffer
{
    Instance instances[];
};
#endif
