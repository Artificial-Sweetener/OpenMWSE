#ifndef OPENMWSE_ATMOSPHERE_CLOUD_FIELD_H_GLSL
#define OPENMWSE_ATMOSPHERE_CLOUD_FIELD_H_GLSL

#include "units.glsl"
#include "cloud_model.h.glsl"

// Morrowind's cloud presentation range is independent of observer altitude and shell thickness.
const float openmwseCloudPresentationFadeStartDistance = 9000.0 * openmwseAtmosphereWorldUnitsPerMeter;
const float openmwseCloudPresentationFadeEndDistance = 13000.0 * openmwseAtmosphereWorldUnitsPerMeter;
const float openmwseCloudMaximumRayDistance = openmwseCloudPresentationFadeEndDistance;
const float openmwseCloudMaximumMarchingDistance = 30000.0 * openmwseAtmosphereWorldUnitsPerMeter;

/// Translate an episode's stationary density realization without changing frequency.
vec2 openmwseCloudEpisodeOffset(float phase);
/// Recover the source wind clock from OpenMW's integrated weather displacement.
float openmwseCloudAnimationSeconds(vec2 integratedDisplacementWorldUnits);

#endif
