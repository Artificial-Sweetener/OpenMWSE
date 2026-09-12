#version 430 compatibility

#include "lib/core/instance.h.glsl"
#include "lib/core/gpuscene_types.glsl"

in uint instanceSlot;
in uint vertexMaterial;

layout(std430, binding = 8) restrict readonly buffer InstanceBuffer
{
    Instance instances[];
};

#if @gpuMaterial
flat out int gpuMaterialId;
#endif

#if @gpuLightList
#include "lib/core/gpuscene_lights.glsl"

flat out uvec2 gpuLightRangeData;

uvec2 gpuLightRangeOf(uint slot)
{
    // The buffer ends after lit slots.
    if (slot >= uint(lightRanges.length()))
        return uvec2(0u, 0u);
    LightRange range = lightRanges[slot];
    return uvec2(range.first, range.count);
}
#endif

uint instanceMaterial()
{
    uint material = instances[instanceSlot].material.x;
    return material == 0xffffffffu ? vertexMaterial : material;
}

uint instanceLight()
{
    return instances[instanceSlot].material.y;
}

mat4 instanceMatrix(Instance inst)
{
    return transpose(mat4(inst.row0, inst.row1, inst.row2, vec4(0.0, 0.0, 0.0, 1.0)));
}

// Also publishes the per-instance ids.
mat4 instanceWorld()
{
    Instance inst = instances[instanceSlot];
#if @gpuMaterial
    gpuMaterialId = int(instanceMaterial());
#endif
#if @gpuLightList
    gpuLightRangeData = gpuLightRangeOf(inst.material.y);
#endif
    return instanceMatrix(inst);
}

mat3 instanceNormal()
{
    Instance inst = instances[instanceSlot];
    return mat3(instanceMatrix(inst));
}

vec4 instanceTangent(vec4 tangent)
{
    return vec4(instanceNormal() * tangent.xyz, tangent.w);
}

vec3 instanceWorldOffset(vec3 nodePosition)
{
    return vec3(0.0);
}
