@link "lib/atmosphere/textured_clouds_vertex.glsl"
@link "lib/atmosphere/cloud_model_vertex.glsl"
@link "lib/core/vertex.glsl" if !@useOVR_multiview
@link "lib/core/vertex_multiview.glsl" if @useOVR_multiview
@link "lib/core/lighting_vertex.glsl" if @lightingMethodClustered
@link "lib/core/lighting_vertex_legacy.glsl" if @legacyLightList

#include "lib/core/material_vertex.h.glsl"

vec4 modelToClip(vec4 pos);
vec4 modelToView(vec4 pos);
vec4 viewToClip(vec4 pos);
vec2 clipToScreen(vec4 pos);
/// Evaluate canonical directional and cloud-shadow lighting at a view-space surface position.
void directionalLighting(vec3 viewPos, vec3 viewDir, vec3 viewNormal, float shininess, out vec3 diffuseLight,
    out vec3 ambientLight, out vec3 specularLight);
void pointLighting(vec2 screenCoord, vec3 viewDir, vec3 viewPos, vec3 viewNormal, float shininess, out vec3 diffuseLight, out vec3 ambientLight, out vec3 specularLight);
