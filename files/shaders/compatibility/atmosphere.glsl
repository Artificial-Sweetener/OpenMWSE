#ifndef OPENMW_COMPATIBILITY_ATMOSPHERE_GLSL
#define OPENMW_COMPATIBILITY_ATMOSPHERE_GLSL

struct Atmosphere {
    bool enabled;
    vec4 skyColor;
    vec3 inScattering;
    vec3 outScattering;
    float weatherWeight;
    vec3 sunDirection;
};

uniform Atmosphere atmosphere;

vec3 atmosphereWorldDirection(vec3 viewDirection)
{
    return normalize((osg_ViewMatrixInverse * vec4(viewDirection, 0.0)).xyz);
}

vec3 atmosphereScatter(vec3 viewDirection, float opticalDistance, float visibility, vec3 baseColor)
{
    vec3 worldDirection = atmosphereWorldDirection(viewDirection);
    vec3 sunDirection = normalize(atmosphere.sunDirection);
    float sunCosine = dot(worldDirection, sunDirection);
    float sunHeight = sunDirection.z;

    float altitude = pow(max(1.0 + sunHeight, 1e-4), 10.0);
    float altitudeBrightness = clamp(1.0 - exp2(-1.9 * altitude), 0.0, 1.0);
    float horizonSun = clamp(exp(-4.0 * sunHeight), 0.0, 1.0) * clamp(altitude, 0.0, 1.0);
    float forwardScatter = (1.58 / max(1.24 - sunCosine, 0.02)) * horizonSun;
    float diffuseScatter = 1.0 - 0.09 * forwardScatter;

    float atmosphereDepth = 1.33 * exp(-2.0 * clamp(worldDirection.z, 0.0, 1.0));
    vec3 directionalScatter
        = mix(atmosphere.inScattering, atmosphere.outScattering, 0.5 * (1.0 + sunCosine));
    float altitudeScale = 2.8 + 4.3 / max(altitude, 1e-4);
    vec3 attenuation = atmosphereDepth * directionalScatter * (altitudeScale + 0.7 * forwardScatter);
    vec3 integratedAttenuation
        = (vec3(1.0) - exp(-opticalDistance * attenuation)) / max(attenuation, vec3(1e-4));

    vec3 scatteredColor = vec3(0.125 * forwardScatter)
        + mix(atmosphere.skyColor.xyz, vec3(0.371, 0.637, 1.108), 0.44) * diffuseScatter;
    scatteredColor *= integratedAttenuation * (1.17 * atmosphereDepth + 0.89) * altitudeBrightness;

    return mix(baseColor * (1.0 - visibility), scatteredColor, atmosphere.weatherWeight);
}

vec3 atmosphereSkyColor(vec3 viewDirection)
{
    vec3 worldDirection = atmosphereWorldDirection(viewDirection);
    float skyAmount = 1.0 - pow(
        clamp(1.0 - 2.22 * clamp(worldDirection.z - 0.075, 0.0, 1.0), 0.0, 1.0), 1.15);
    vec3 directionalSky = mix(fog.color.xyz, atmosphere.skyColor.xyz, skyAmount);
    return atmosphereScatter(viewDirection, 1.0, 0.0, directionalSky);
}

#endif
