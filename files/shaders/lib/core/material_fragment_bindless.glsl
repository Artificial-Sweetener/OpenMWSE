#version 430 compatibility

#extension GL_ARB_bindless_texture : require
#ifdef GL_NV_gpu_shader5
#extension GL_NV_gpu_shader5 : require
#define GPU_SAMPLER(handle) sampler2D(handle)
#else
#extension GL_EXT_nonuniform_qualifier : require
#define GPU_SAMPLER(handle) nonuniformEXT(sampler2D(handle))
#endif

#include "lib/core/material_fragment.h.glsl"
#include "lib/core/gpuscene_material.glsl"

flat in int gpuMaterialId;

Material getMaterial()
{
    return gpuMaterialParams(gpuMaterialId);
}

bool hasMap(int map)
{
    return gpuHasMap(gpuMaterialId, map);
}

uvec2 mapHandle(int map)
{
    return gpuMaterials[gpuMaterialId].maps[map];
}

// Absent maps sample as white.
vec4 sampleMap(int map, vec2 uv)
{
    if (!hasMap(map))
        return vec4(1.0);
    return texture(GPU_SAMPLER(mapHandle(map)), uv);
}

vec4 sampleDiffuseMap(vec2 uv)
{
    return sampleMap(@diffuseMapIndex, uv);
}

vec4 sampleDarkMap(vec2 uv)
{
    return sampleMap(@darkMapIndex, uv);
}

vec4 sampleDetailMap(vec2 uv)
{
    return sampleMap(@detailMapIndex, uv);
}

vec4 sampleDecalMap(vec2 uv)
{
    return sampleMap(@decalMapIndex, uv);
}

vec4 sampleEmissiveMap(vec2 uv)
{
    return sampleMap(@emissiveMapIndex, uv);
}

vec4 sampleNormalMap(vec2 uv)
{
    return sampleMap(@normalMapIndex, uv);
}

vec4 sampleEnvMap(vec2 uv)
{
    return sampleMap(@envMapIndex, uv);
}

vec4 sampleSpecularMap(vec2 uv)
{
    return sampleMap(@specularMapIndex, uv);
}

vec4 sampleBumpMap(vec2 uv)
{
    return sampleMap(@bumpMapIndex, uv);
}

vec4 sampleGlossMap(vec2 uv)
{
    return sampleMap(@glossMapIndex, uv);
}

vec4 sampleBlendMap(vec2 uv)
{
    return sampleMap(@blendMapIndex, uv);
}

float mapMipmapLevel(vec2 scaledUV)
{
    vec2 dUVdx = dFdx(scaledUV);
    vec2 dUVdy = dFdy(scaledUV);
    float maxDUVSquared = max(dot(dUVdx, dUVdx), dot(dUVdy, dUVdy));
    return max(0.0, 0.5 * log2(maxDUVSquared));
}

float mapCoverageScale(int map, vec2 uv)
{
#if @adjustCoverage
    if (!hasMap(map))
        return 1.0;
    vec2 size = vec2(textureSize(GPU_SAMPLER(mapHandle(map)), 0));
    return 1.0 + mapMipmapLevel(uv * size) * 0.25;
#else
    return 1.0;
#endif
}

vec2 transformUV(int unit, vec2 uv)
{
    return (gpuTexMat(gpuMaterialId, unit) * vec4(uv, 0.0, 1.0)).xy;
}

float getAlphaRef()
{
    return gpuMaterials[gpuMaterialId].params.w;
}

vec4 getEnvMapColor()
{
    return gpuMaterials[gpuMaterialId].envMapColor;
}

vec2 getEnvMapLumaBias()
{
    return gpuMaterials[gpuMaterialId].lumaBias.xy;
}

mat2 getBumpMapMatrix()
{
    return mat2(gpuMaterials[gpuMaterialId].bumpMatrix);
}
