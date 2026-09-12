#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/material_fragment.h.glsl"

uniform Material material;

uniform sampler2D diffuseMap;
uniform sampler2D darkMap;
uniform sampler2D detailMap;
uniform sampler2D decalMap;
uniform sampler2D emissiveMap;
uniform sampler2D normalMap;
uniform sampler2D envMap;
uniform sampler2D specularMap;
uniform sampler2D bumpMap;
uniform sampler2D glossMap;
uniform sampler2D blendMap;

uniform float alphaRef;
uniform vec4 envMapColor;
uniform vec2 envMapLumaBias;
uniform mat2 bumpMapMatrix;

uniform mat4 texMat0;
uniform mat4 texMat1;
uniform mat4 texMat2;
uniform mat4 texMat3;
uniform mat4 texMat4;
uniform mat4 texMat5;
uniform mat4 texMat6;
uniform mat4 texMat7;

Material getMaterial()
{
    return material;
}

// Maps are compiled in or out per program.
bool hasMap(int map)
{
    if (map == @diffuseMapIndex)
        return @diffuseMap != 0;
    if (map == @darkMapIndex)
        return @darkMap != 0;
    if (map == @detailMapIndex)
        return @detailMap != 0;
    if (map == @decalMapIndex)
        return @decalMap != 0;
    if (map == @emissiveMapIndex)
        return @emissiveMap != 0;
    if (map == @normalMapIndex)
        return @normalMap != 0;
    if (map == @envMapIndex)
        return @envMap != 0;
    if (map == @specularMapIndex)
        return @specularMap != 0;
    if (map == @bumpMapIndex)
        return @bumpMap != 0;
    if (map == @glossMapIndex)
        return @glossMap != 0;
    return @blendMap != 0;
}

vec4 sampleDiffuseMap(vec2 uv)
{
    return texture2D(diffuseMap, uv);
}

vec4 sampleDarkMap(vec2 uv)
{
    return texture2D(darkMap, uv);
}

vec4 sampleDetailMap(vec2 uv)
{
    return texture2D(detailMap, uv);
}

vec4 sampleDecalMap(vec2 uv)
{
    return texture2D(decalMap, uv);
}

vec4 sampleEmissiveMap(vec2 uv)
{
    return texture2D(emissiveMap, uv);
}

vec4 sampleNormalMap(vec2 uv)
{
    return texture2D(normalMap, uv);
}

vec4 sampleEnvMap(vec2 uv)
{
    return texture2D(envMap, uv);
}

vec4 sampleSpecularMap(vec2 uv)
{
    return texture2D(specularMap, uv);
}

vec4 sampleBumpMap(vec2 uv)
{
    return texture2D(bumpMap, uv);
}

vec4 sampleGlossMap(vec2 uv)
{
    return texture2D(glossMap, uv);
}

vec4 sampleBlendMap(vec2 uv)
{
    return texture2D(blendMap, uv);
}

vec2 mapSize(int map)
{
#if @useGPUShader4
    if (map == @diffuseMapIndex)
        return vec2(textureSize2D(diffuseMap, 0));
    if (map == @darkMapIndex)
        return vec2(textureSize2D(darkMap, 0));
    if (map == @detailMapIndex)
        return vec2(textureSize2D(detailMap, 0));
    if (map == @decalMapIndex)
        return vec2(textureSize2D(decalMap, 0));
    if (map == @emissiveMapIndex)
        return vec2(textureSize2D(emissiveMap, 0));
    if (map == @normalMapIndex)
        return vec2(textureSize2D(normalMap, 0));
    if (map == @envMapIndex)
        return vec2(textureSize2D(envMap, 0));
    if (map == @specularMapIndex)
        return vec2(textureSize2D(specularMap, 0));
    if (map == @bumpMapIndex)
        return vec2(textureSize2D(bumpMap, 0));
    if (map == @glossMapIndex)
        return vec2(textureSize2D(glossMap, 0));
    return vec2(textureSize2D(blendMap, 0));
#else
    return vec2(256.0);
#endif
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
    return 1.0 + mapMipmapLevel(uv * mapSize(map)) * 0.25;
#else
    return 1.0;
#endif
}

mat4 texMat(int unit)
{
    if (unit == 0)
        return texMat0;
    if (unit == 1)
        return texMat1;
    if (unit == 2)
        return texMat2;
    if (unit == 3)
        return texMat3;
    if (unit == 4)
        return texMat4;
    if (unit == 5)
        return texMat5;
    if (unit == 6)
        return texMat6;
    return texMat7;
}

vec2 transformUV(int unit, vec2 uv)
{
    return (texMat(unit) * vec4(uv, 0.0, 1.0)).xy;
}

float getAlphaRef()
{
    return alphaRef;
}

vec4 getEnvMapColor()
{
    return envMapColor;
}

vec2 getEnvMapLumaBias()
{
    return envMapLumaBias;
}

mat2 getBumpMapMatrix()
{
    return bumpMapMatrix;
}
