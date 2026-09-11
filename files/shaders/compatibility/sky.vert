#version 120

#include "lib/sky/cloud_lighting.h.glsl"

#include "lib/core/vertex.h.glsl"

#include "lib/sky/passes.glsl"
#include "fog.h.glsl"
#include "atmosphere.glsl"

uniform int pass;
uniform mat4 texMat0;

varying vec4 passColor;
varying vec2 diffuseMapUV;
varying vec3 passViewDirection;

/// Cover reflected solar diffusion and publish fixed-altitude illumination for reflected atmosphere and sunlight.
void main()
{
    skyLowCloudSolarIrradiance = vec3(0.0);
    skyLowCloudDiffuseRadiance = vec3(0.0);
    if ((pass == PASS_ATMOSPHERE || pass == PASS_SUN) && isReflection && atmosphere.enabled)
    {
        OpenMWSEVolumeIllumination lowLighting = openmwseLocalVolumeIllumination(
            atmosphere.solarRadiance, atmosphere.groundIrradiance,
            waterHeight + atmosphere.lowCloudParameters.z + atmosphere.lowCloudParameters.w * 0.5,
            openmwseAerialPerspectiveParameters());
        skyLowCloudSolarIrradiance = lowLighting.solarIrradiance;
        skyLowCloudDiffuseRadiance = lowLighting.diffuseRadiance;
    }

    vec4 position = gl_Vertex;
    if (pass == PASS_SUN && atmosphere.sunlightEnabled)
        position.xy *= 3.0;
    vec4 viewPosition = modelToView(position);

    gl_Position = viewToClip(viewPosition);
    passColor = gl_Color;
    passViewDirection = viewPosition.xyz;

    if (pass == PASS_CLOUDS)
        diffuseMapUV = (texMat0 * gl_MultiTexCoord0).xy;
    else
        diffuseMapUV = gl_MultiTexCoord0.xy;
}
