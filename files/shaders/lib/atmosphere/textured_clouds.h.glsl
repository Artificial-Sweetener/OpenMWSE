#ifndef OPENMWSE_TEXTURED_CLOUDS_H_GLSL
#define OPENMWSE_TEXTURED_CLOUDS_H_GLSL

uniform sampler2D openmwseTexturedCloudOpacity;
uniform sampler2D openmwseTexturedCloudDomeMapping;
uniform bool openmwseTexturedCloudDomeAvailable;
uniform vec4 openmwseTexturedCloudTransforms[2];
uniform vec2 openmwseTexturedCloudWeights;
uniform bool openmwseTexturedCloudsEnabled;

// World-unit UV projection distances do not position the visible dome or limit receiver altitude.
const float openmwseTexturedCloudProjectionHeight = 12000.0;
const float openmwseTexturedCloudTileSize = 16000.0;

/// Project along sunlight onto a world-space UV datum; direction.z must exceed 0.01.
vec3 openmwseTexturedCloudShadowProjection(vec3 origin, vec3 direction);

/// Apply the weather layer's storm orientation and continuous UV advection to a world position.
vec2 openmwseTexturedCloudShadowUv(vec3 point, vec4 transform);

/// Convert authored cloud coverage into Beer transmission; empty texels remain exactly transparent.
float openmwseTexturedCloudTransmission(float alpha);

/// Project the weather opacity onto surfaces without changing the camera-relative visible dome.
float openmwseTexturedCloudTransmittance(vec3 origin, vec3 direction);

/// Recover the loaded dome's authored UVs and vertex opacity in its unrotated local direction.
vec3 openmwseTexturedCloudDomeSample(vec3 direction, vec4 transform);

/// Attenuate celestial light through the authored camera-relative dome, independent of observer altitude.
float openmwseTexturedCloudViewTransmittance(vec3 direction);

/// Illuminate optically thin cloud edges with a bounded forward-scattering solar lobe.
float openmwseTexturedCloudSolarSource(vec3 direction, vec3 sunDirection);

/// Add local cloud illumination and a small angular diffusion halo, independent of pixel size or camera motion.
vec3 openmwseTexturedCloudBloom(vec3 direction, vec3 sunDirection, vec4 solarColor);

#endif
