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

#if @clusteredClassic
layout(std430, binding = 15) readonly buffer ReceiverSlots { uint lightClusters[]; };
flat out uint gpuLightCluster;
#endif

uint instanceMaterial()
{
    uint material = instances[instanceSlot].material.x;
    return material == 0xffffffffu ? vertexMaterial : material;
}

mat4 instanceMatrix(Instance inst)
{
    return transpose(mat4(inst.row0, inst.row1, inst.row2, vec4(0.0, 0.0, 0.0, 1.0)));
}

// Also publishes the per-instance ids.
mat4 instanceWorld()
{
    Instance inst = instances[instanceSlot];
#if @clusteredClassic
    gpuLightCluster = inst.material.y < uint(lightClusters.length()) ? lightClusters[inst.material.y] : 0u;
#endif
#if @gpuMaterial
    gpuMaterialId = int(instanceMaterial());
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
