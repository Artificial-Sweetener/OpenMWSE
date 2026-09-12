#include "lib/light/struct.glsl"

uniform mat4 LightBuffer[@maxLights];
uniform int PointLightCount;
#define LightAt(index) LightBuffer[index]

#include "lib/light/bindings-point.glsl"
