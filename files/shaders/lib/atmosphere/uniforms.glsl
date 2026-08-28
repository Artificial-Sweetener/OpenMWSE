#ifndef OPENMWSE_ATMOSPHERE_UNIFORMS_GLSL
#define OPENMWSE_ATMOSPHERE_UNIFORMS_GLSL

struct Atmosphere
{
    bool enabled;
    vec4 skyColor;
    vec3 inScattering;
    vec3 outScattering;
    float weatherWeight;
    vec3 sunDirection;
    // coverage, density, base altitude in world units, thickness in world units
    vec4 cloudParameters;
    // density, base altitude in world units, thickness in world units, shaft intensity
    vec4 mistParameters;
    // scene-linear HDR tint and visibility
    vec4 sunDiscColor;
    // direct solar radiance, diffuse sky irradiance, and diffuse ground irradiance
    vec3 solarRadiance;
    vec3 skyIrradiance;
    vec3 groundIrradiance;
    // Integrated world-space offset shared by visible clouds, reflections, and surface shadows.
    vec2 cloudAdvectionOffsetWorldUnits;
    int quality;
};

uniform Atmosphere atmosphere;

#endif
