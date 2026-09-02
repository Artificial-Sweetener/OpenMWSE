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
    // morphology from stratus to storm, feature scale, vertical development, anvil strength
    vec4 cloudShapeParameters;
    // deck continuity, convective frequency, convective maturity, storm organization
    vec4 cloudOrganizationParameters;
    // tower taper, base-height variation, reserved, reserved
    vec4 cloudStructureParameters;
    // coverage, density, base altitude in world units, thickness in world units
    vec4 lowCloudParameters;
    // morphology, feature scale, vertical development, environment influence
    vec4 lowCloudShapeParameters;
    // density, base altitude in world units, thickness in world units, shaft intensity
    vec4 mistParameters;
    // density, base altitude in world units, thickness in world units, scattering anisotropy
    vec4 fogVolumeParameters;
    // terrain height, cloud terrain-following weight, terrain clearance, valley depth
    vec4 mediumReferenceParameters;
    // sparse terrain sample center xy and local height gradient xy
    vec4 orographicParameters;
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
uniform sampler3D openmwseCloudNoiseTexture;
uniform sampler3D openmwseCloudDetailNoiseTexture;
uniform sampler2D openmwseCloudCurlNoiseTexture;
uniform sampler2D openmwseCloudWeatherTexture;

#endif
