#ifndef LIB_LIGHT_BINDINGS_POINT
#define LIB_LIGHT_BINDINGS_POINT

#include "lib/light/struct.glsl"

/* Layout:
--------------------------------------- -----------
|  pos_x  |  ambi_r  |  diff_r  |  spec_r         |
|  pos_y  |  ambi_g  |  diff_g  |  spec_g         |
|  pos_z  |  ambi_b  |  diff_b  |  spec_b         |
|  att_c  |  att_l   |  att_q   |  radius/spec_a  |
 --------------------------------------------------
*/
PointLight loadPointLight(int lightIndex)
{
    mat4 light = LightAt(lightIndex);
    return PointLight(vec4(light[0].xyz, 1.0), vec4(light[2].xyz, 0.0), vec4(light[1].xyz, 0.0), light[3],
        light[0].w, light[1].w, light[2].w, light[3].w);
}

uniform DirectionalLight sun;

#endif
