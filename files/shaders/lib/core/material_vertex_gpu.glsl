#version 430 compatibility

#include "lib/core/material_vertex.h.glsl"
#include "lib/core/gpuscene_material.glsl"

uint instanceMaterial();

Material getMaterial()
{
    return gpuMaterialParams(int(instanceMaterial()));
}

vec4 mapCoord(int unit)
{
    if (unit == 0)
        return gl_MultiTexCoord0;
    if (unit == 1)
        return gl_MultiTexCoord1;
    if (unit == 2)
        return gl_MultiTexCoord2;
    if (unit == 3)
        return gl_MultiTexCoord3;
    if (unit == 4)
        return gl_MultiTexCoord4;
    if (unit == 5)
        return gl_MultiTexCoord5;
    if (unit == 6)
        return gl_MultiTexCoord6;
    if (unit == 7)
        return gl_MultiTexCoord7;
    return vec4(0.0, 0.0, 0.0, 1.0);
}

// Uber materials pick the coordinate set per map.
vec2 mapUV(int map, int unit, vec4 coord)
{
    int id = int(instanceMaterial());
#if @gpuUber
    unit = gpuMapUnit(id, map);
    coord = mapCoord(unit);
#endif
    return (gpuTexMat(id, unit) * coord).xy;
}
