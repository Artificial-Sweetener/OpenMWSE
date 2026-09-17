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

/// Expose the existing per-receiver light list to water optics without transferring ownership.
ivec2 waterCanonicalLightRange(vec2 screenCoord, vec3 viewPos)
{
#if @lightingMethodClustered
    LightGrid grid = clusterLights(getClusterTileIndex(screenRes, gridSize, near, screenCoord, viewPos.z));
    return ivec2(int(grid.offset), int(grid.count));
#else
    return ivec2(0, PointLightCount);
#endif
}

/// Resolve a light-list entry through the active canonical lighting backend.
int waterCanonicalLightIndex(int offset, int ordinal)
{
#if @lightingMethodClustered
    return int(lightIndexList[offset + ordinal]);
#else
    return ordinal;
#endif
}

/// Return canonical point-light geometry in the view space consumed by material shaders.
vec4 waterCanonicalLightPositionRadius(int index)
{
#if @lightingMethodClustered
    PointLight light = pointLight[index];
#else
    PointLight light = loadPointLight(index);
#endif
    return vec4(light.position.xyz, light.radius);
}

/// Return canonical point-light color without imposing a separate water-light palette.
vec3 waterCanonicalLightColor(int index)
{
#if @lightingMethodClustered
    PointLight light = pointLight[index];
#else
    PointLight light = loadPointLight(index);
#endif
    return max(light.diffuse.rgb, light.specular.rgb);
}

/// Reuse the active lighting backend's attenuation policy for water illumination and focusing.
float waterCanonicalLightAttenuation(int index, float distanceToLight)
{
#if @lightingMethodClustered
    PointLight light = pointLight[index];
#else
    PointLight light = loadPointLight(index);
#endif
    return calcAttenuation(light, distanceToLight);
}

#if @clusteredClassic
#if @gpuInstanced
flat in uint gpuLightCluster;
uint receiverCluster() { return gpuLightCluster; }
#else
uniform uint lightCluster;
uint receiverCluster() { return lightCluster; }
#endif
#endif

void doLighting(vec2 screenCoord, vec3 viewPos, vec3 viewNormal, float shininess, float shadowing, out vec3 diffuseLight, out vec3 ambientLight, out vec3 specularLight) {
    vec3 viewDir = normalize(viewPos);
    shininess = max(shininess, 1e-4);

    diffuseLight = vec3(0.0);
    ambientLight = vec3(0.0);
    specularLight = vec3(0.0);

    calcDirectionalLighting(sun, viewDir, viewNormal, shininess, diffuseLight, ambientLight, specularLight);

    vec3 worldPosition = (osg_ViewMatrixInverse * vec4(viewPos, 1.0)).xyz;
    openmwseApplyCloudLighting(worldPosition, diffuseLight, ambientLight, specularLight);

    diffuseLight *= shadowing;
    specularLight *= shadowing;

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

#if @lightingMethodClustered && !@clusteredClassic
vec3 doSpecularLighting(vec2 screenCoord, vec3 viewPos, vec3 viewNormal) {
    vec3 specular = vec3(0.0);
    vec3 viewDir = normalize(viewPos);
    float shininess = 50.0;

    LightGrid grid = clusterLights(getClusterTileIndex(screenRes, gridSize, near, screenCoord, viewPos.z));
    for (uint i = 0u; i < grid.count; ++i) {
        PointLight light = pointLight[lightIndexList[grid.offset + i]];

        vec3 lightPos = light.position.xyz - viewPos;
        float lightDistance = length(lightPos);
        vec3 lightDir = lightPos / lightDistance;
        float attenuation = calcAttenuation(light, lightDistance) * clusterFade(viewPos, light.radius);
        specular += light.specular.xyz * specularIntensity(viewNormal, viewDir, shininess, lightDir) * attenuation;
    }

    return specular;
}
#else
vec3 doSpecularLighting(vec2 screenCoord, vec3 viewPos, vec3 viewNormal) { return vec3(0.0); }
#endif
