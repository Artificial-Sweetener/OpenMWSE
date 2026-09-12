#ifndef OPENMWSE_ATMOSPHERE_UNIFORMS_GLSL
#define OPENMWSE_ATMOSPHERE_UNIFORMS_GLSL

struct Atmosphere
{
    // Fourteen vec4 parameters per outgoing, incoming, and environmental cloud layer.
    vec4 cloudLayers[42];
    // Four scalar terrain heights per slot on a fixed 15 by 15 world lattice.
    vec4 terrainHeights[57];
    vec4 terrainOriginSpacing;
    vec2 terrainHeightBounds;
    bool enabled;
    bool scatteringEnabled;
    int appearance;
    bool sunlightEnabled;
    bool cloudSunOcclusion;
    bool cloudsEnabled;
    bool cloudShadowsEnabled;
    bool legacyCloudShadows;
    float cloudShadowIntensity;
    bool cloudReflectionsEnabled;
    vec4 skyColor;
    vec3 inScattering;
    vec3 outScattering;
    float weatherWeight;
    vec3 sunDirection;
    // coverage, density, base altitude in world units, thickness in world units
    vec4 cloudParameters;
    // Incoming episode parameters retain a distinct procedural domain during transition.
    vec4 incomingCloudParameters;
    // outgoing phase, incoming phase, incoming blend weight, incoming field enabled
    vec4 cloudScenarioParameters;
    // coverage, density, base altitude in world units, thickness in world units
    vec4 lowCloudParameters;
    // Coupling of the environmental source layer to local terrain.
    float lowCloudEnvironmentInfluence;
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
