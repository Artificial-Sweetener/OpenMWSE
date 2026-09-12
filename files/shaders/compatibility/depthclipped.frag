#version 120

#include "lib/core/material_fragment.h.glsl"

varying vec2 diffuseMapUV;
varying float alphaPassthrough;

void main()
{
    float alpha = sampleDiffuseMap(diffuseMapUV).a * alphaPassthrough;

    const float depthAlpha = 0.499;

    if (alpha < depthAlpha)
        discard;

    // DO NOT write to color!
    // This is in charge of only updating depth buffer.
}
