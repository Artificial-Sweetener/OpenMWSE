#version 430 compatibility

#include "lib/core/material_fragment.h.glsl"
#include "lib/core/gpuscene_material.glsl"

flat in int gpuMaterialId;

uniform sampler2DArray diffuseMap;
uniform sampler2DArray darkMap;
uniform sampler2DArray detailMap;
uniform sampler2DArray decalMap;
uniform sampler2DArray emissiveMap;
uniform sampler2DArray normalMap;
uniform sampler2DArray envMap;
uniform sampler2DArray specularMap;
uniform sampler2DArray bumpMap;
uniform sampler2DArray glossMap;
uniform sampler2DArray blendMap;

Material getMaterial()
{
    return gpuMaterialParams(gpuMaterialId);
}

bool hasMap(int map)
{
    return gpuHasMap(gpuMaterialId, map);
}

float mapLayer(int map)
{
    return float(gpuMaterials[gpuMaterialId].maps[map].x - 1u);
}

// Absent maps sample as white.
vec4 sampleLayer(sampler2DArray map, int index, vec2 uv)
{
    if (!hasMap(index))
        return vec4(1.0);
    return texture(map, vec3(uv, mapLayer(index)));
}

vec4 sampleDiffuseMap(vec2 uv)
{
    return sampleLayer(diffuseMap, @diffuseMapIndex, uv);
}

vec4 sampleDarkMap(vec2 uv)
{
    return sampleLayer(darkMap, @darkMapIndex, uv);
}

vec4 sampleDetailMap(vec2 uv)
{
    return sampleLayer(detailMap, @detailMapIndex, uv);
}

vec4 sampleDecalMap(vec2 uv)
{
    return sampleLayer(decalMap, @decalMapIndex, uv);
}

vec4 sampleEmissiveMap(vec2 uv)
{
    return sampleLayer(emissiveMap, @emissiveMapIndex, uv);
}

vec4 sampleNormalMap(vec2 uv)
{
    return sampleLayer(normalMap, @normalMapIndex, uv);
}

vec4 sampleEnvMap(vec2 uv)
{
    return sampleLayer(envMap, @envMapIndex, uv);
}

vec4 sampleSpecularMap(vec2 uv)
{
    return sampleLayer(specularMap, @specularMapIndex, uv);
}

vec4 sampleBumpMap(vec2 uv)
{
    return sampleLayer(bumpMap, @bumpMapIndex, uv);
}

vec4 sampleGlossMap(vec2 uv)
{
    return sampleLayer(glossMap, @glossMapIndex, uv);
}

vec4 sampleBlendMap(vec2 uv)
{
    return sampleLayer(blendMap, @blendMapIndex, uv);
}

vec2 mapSize(int map)
{
    if (map == @diffuseMapIndex)
        return vec2(textureSize(diffuseMap, 0).xy);
    if (map == @darkMapIndex)
        return vec2(textureSize(darkMap, 0).xy);
    if (map == @detailMapIndex)
        return vec2(textureSize(detailMap, 0).xy);
    if (map == @decalMapIndex)
        return vec2(textureSize(decalMap, 0).xy);
    if (map == @emissiveMapIndex)
        return vec2(textureSize(emissiveMap, 0).xy);
    if (map == @normalMapIndex)
        return vec2(textureSize(normalMap, 0).xy);
    if (map == @envMapIndex)
        return vec2(textureSize(envMap, 0).xy);
    if (map == @specularMapIndex)
        return vec2(textureSize(specularMap, 0).xy);
    if (map == @bumpMapIndex)
        return vec2(textureSize(bumpMap, 0).xy);
    if (map == @glossMapIndex)
        return vec2(textureSize(glossMap, 0).xy);
    return vec2(textureSize(blendMap, 0).xy);
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
    return 1.0 + mapMipmapLevel(uv * mapSize(map)) * 0.25;
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
