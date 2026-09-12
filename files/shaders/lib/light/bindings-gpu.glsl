#include "lib/light/struct.glsl"
#include "lib/core/gpuscene_lights.glsl"

uvec2 gpuLightRange();
#define LightAt(index) gpuLights[gpuLightRange().x + uint(index)]
#define PointLightCount int(gpuLightRange().y)

#include "lib/light/bindings-point.glsl"
