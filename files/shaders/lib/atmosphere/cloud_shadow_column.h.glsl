#ifndef OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_COLUMN_H_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_SHADOW_COLUMN_H_GLSL

#include "cloud_field.h.glsl"

/// Estimate surface-shadow optical depth only through the typed weather column above a Z-up receiver.
float openmwseCloudShadowColumnOpticalDepth(vec3 receiver, vec3 sunDirection,
    vec4 parameters, float waterDatum, vec2 episodeOffset, CloudModelLayer model,
    sampler2D weatherTexture);

#endif
