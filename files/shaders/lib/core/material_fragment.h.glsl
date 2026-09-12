#ifndef LIB_CORE_MATERIAL_FRAGMENT_H
#define LIB_CORE_MATERIAL_FRAGMENT_H

@link "lib/core/material_fragment_legacy.glsl" if !@gpuMaterial
@link "lib/core/material_fragment_bindless.glsl" if @gpuBindlessBindings
@link "lib/core/material_fragment_array.glsl" if @gpuArrayBindings

#include "lib/material/struct.glsl"

Material getMaterial();
bool hasMap(int map);
vec4 sampleDiffuseMap(vec2 uv);
vec4 sampleDarkMap(vec2 uv);
vec4 sampleDetailMap(vec2 uv);
vec4 sampleDecalMap(vec2 uv);
vec4 sampleEmissiveMap(vec2 uv);
vec4 sampleNormalMap(vec2 uv);
vec4 sampleEnvMap(vec2 uv);
vec4 sampleSpecularMap(vec2 uv);
vec4 sampleBumpMap(vec2 uv);
vec4 sampleGlossMap(vec2 uv);
vec4 sampleBlendMap(vec2 uv);
float mapCoverageScale(int map, vec2 uv);
vec2 transformUV(int unit, vec2 uv);
float getAlphaRef();
vec4 getEnvMapColor();
vec2 getEnvMapLumaBias();
mat2 getBumpMapMatrix();

#endif
