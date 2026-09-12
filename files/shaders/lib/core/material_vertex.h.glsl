#ifndef LIB_CORE_MATERIAL_VERTEX_H
#define LIB_CORE_MATERIAL_VERTEX_H

@link "lib/core/material_vertex_legacy.glsl" if !@gpuMaterial
@link "lib/core/material_vertex_gpu.glsl" if @gpuMaterial

#include "lib/material/struct.glsl"

Material getMaterial();
vec2 mapUV(int map, int unit, vec4 coord);

#endif
