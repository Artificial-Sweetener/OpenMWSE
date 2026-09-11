#ifndef OPENMWSE_ATMOSPHERE_UNITS_GLSL
#define OPENMWSE_ATMOSPHERE_UNITS_GLSL

// Convert physical atmospheric lengths into OpenMW's uncurved world-space coordinates.
const float openmwseAtmosphereWorldUnitsPerMeter = 69.99125109;

// Use an Earth-scale datum for cloud-shell curvature while keeping the terrain coordinate frame uncurved.
const float openmwseAtmospherePlanetRadiusWorldUnits = 6360000.0 * openmwseAtmosphereWorldUnitsPerMeter;

#endif
