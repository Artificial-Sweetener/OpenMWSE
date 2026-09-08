/// Share scene fog and water-view uniforms between vertex and fragment programs.
#ifndef OPENMW_COMPATIBILITY_FOG_H_GLSL
#define OPENMW_COMPATIBILITY_FOG_H_GLSL

#include "lib/light/struct.glsl"

/// Describe surface and underwater fog colors, distances, and depth.
struct Fog {
    vec4 color;
    vec4 underwaterColor;
    float start;
    float underwaterStart;
    float end;
    float underwaterEnd;
    float depth;
};

uniform Fog fog;
uniform float waterHeight;
uniform bool waterEnabled;
uniform bool waterSurface;
uniform bool isReflection;
uniform mat4 osg_ViewMatrixInverse;
uniform DirectionalLight sun;

#endif
