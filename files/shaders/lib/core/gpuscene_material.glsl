#ifndef LIB_CORE_GPUSCENE_MATERIAL
#define LIB_CORE_GPUSCENE_MATERIAL

#include "lib/material/struct.glsl"

struct GpuMaterial
{
    uvec2 maps[12];
    vec4 diffuse;
    vec4 ambient;
    vec4 specular;
    vec4 emission;
    vec4 envMapColor;
    vec4 params;
    vec4 lumaBias;
    vec4 bumpMatrix;
    ivec4 modes;
    vec4 texMatRows[16];
};

layout(std430, binding = 14) restrict readonly buffer MaterialBuffer
{
    GpuMaterial gpuMaterials[];
};

Material gpuMaterialParams(int id)
{
    Material result;
    result.diffuse = gpuMaterials[id].diffuse;
    result.ambient = gpuMaterials[id].ambient;
    result.specular = gpuMaterials[id].specular;
    result.emission = gpuMaterials[id].emission;
    result.shininess = gpuMaterials[id].params.x;
    result.emissiveMult = gpuMaterials[id].params.y;
    result.specStrength = gpuMaterials[id].params.z;
    result.vertexColorMode = gpuMaterials[id].modes.x;
    return result;
}

mat4 gpuTexMat(int id, int unit)
{
    vec4 r0 = gpuMaterials[id].texMatRows[unit * 2];
    vec4 r1 = gpuMaterials[id].texMatRows[unit * 2 + 1];
    return mat4(vec4(r0.x, r1.x, 0.0, 0.0), vec4(r0.y, r1.y, 0.0, 0.0), vec4(r0.z, r1.z, 1.0, 0.0), vec4(r0.w, r1.w, 0.0, 1.0));
}

bool gpuHasMap(int id, int map)
{
#if @gpuMaterialArray
    return gpuMaterials[id].maps[map].x != 0u;
#else
    return any(notEqual(gpuMaterials[id].maps[map], uvec2(0u)));
#endif
}

// Texture coordinate set of an uber material map.
int gpuMapUnit(int id, int map)
{
    int bits = map < 8 ? gpuMaterials[id].modes.y : gpuMaterials[id].modes.z;
    return (bits >> ((map < 8 ? map : map - 8) * 4)) & 15;
}

#endif
