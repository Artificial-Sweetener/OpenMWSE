#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#if @lightingMethodClustered
    #include "lib/light/bindings.glsl"
#else
    #include "lib/light/bindings-legacy.glsl"
#endif

#include "lib/light/util.glsl"
#include "lib/atmosphere/cloud_shadow.glsl"

uniform mat4 osg_ViewMatrixInverse;

uniform float near;
uniform vec2 screenRes;
uniform vec3 gridSize;

#if @clusteredClassic
#if @gpuInstanced
flat out uint gpuLightCluster;
uint receiverCluster() { return gpuLightCluster; }
#else
uniform uint lightCluster;
uint receiverCluster() { return lightCluster; }
#endif
#endif

/// Evaluate canonical directional and cloud-shadow lighting at a view-space surface position.
void directionalLighting(vec3 viewPos, vec3 viewDir, vec3 viewNormal, float shininess, out vec3 diffuseLight,
    out vec3 ambientLight, out vec3 specularLight)
{
    diffuseLight = vec3(0.0);
    ambientLight = vec3(0.0);
    specularLight = vec3(0.0);

    calcDirectionalLighting(sun, viewDir, viewNormal, shininess, diffuseLight, ambientLight, specularLight);

    vec3 worldPosition = (osg_ViewMatrixInverse * vec4(viewPos, 1.0)).xyz;
    openmwseApplyCloudLighting(worldPosition, diffuseLight, ambientLight, specularLight);
}

void pointLighting(vec2 screenCoord, vec3 viewDir, vec3 viewPos, vec3 viewNormal, float shininess, out vec3 diffuseLight, out vec3 ambientLight, out vec3 specularLight)
{
    diffuseLight = vec3(0.0);
    ambientLight = vec3(0.0);
    specularLight = vec3(0.0);

#if @simpleLighting || (@particle && !@particlePointLighting)
    return;
#endif

#if @lightingMethodClustered
    LightGrid grid = clusterLights(getClusterTileIndex(screenRes, gridSize, near, screenCoord, viewPos.z));
    for (uint i = 0u; i < grid.count; ++i) {
        PointLight light = pointLight[lightIndexList[grid.offset + i]];
#else
    for (int i = 0; i < PointLightCount; ++i) {
        PointLight light = loadPointLight(i);
#endif

        calcPointLighting(light, viewDir, viewPos, viewNormal, shininess, diffuseLight, ambientLight, specularLight);
    }
}
