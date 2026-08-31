#version 120

/// Preserve light-facing casters while leaving geometry behind the receiver volume to normal clipping.
vec4 clampShadowCasterLightFacingDepth(vec4 clipPosition)
{
    clipPosition.z = max(clipPosition.z, -clipPosition.w);
    return clipPosition;
}

#if @opaqueShadowCaster

void main(void)
{
    // Opaque casters need only clip-space position and user clip-plane distance.
    gl_Position = clampShadowCasterLightFacingDepth(gl_ModelViewProjectionMatrix * gl_Vertex);
    gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;
}

#else

#include "lib/core/vertex.h.glsl"
#include "lib/material/colormodes.glsl"

varying vec2 diffuseMapUV;

varying float alphaPassthrough;

uniform bool useTreeAnim;
uniform bool useDiffuseMapForShadowAlpha = true;
uniform bool alphaTestShadows = true;
uniform float alpha;
uniform float actorFade;
uniform mat4 texMat0;

void main(void)
{
    Material material = getMaterial();

    gl_Position = clampShadowCasterLightFacingDepth(gl_ModelViewProjectionMatrix * gl_Vertex);

    vec4 viewPos = (gl_ModelViewMatrix * gl_Vertex);
    gl_ClipVertex = viewPos;

    if (useDiffuseMapForShadowAlpha)
        diffuseMapUV = (texMat0 * gl_MultiTexCoord0).xy;
    else
        diffuseMapUV = vec2(0.0); // Avoid undefined behaviour if running on hardware predating the concept of dynamically uniform expressions
    if (material.vertexColorMode == ColorMode_AmbientAndDiffuse)
        alphaPassthrough = useTreeAnim ? 1.0 : gl_Color.a;
    else
        // This is uniform, so if it's too low, we might be able to put the position/clip vertex outside the view frustum and skip the fragment shader and rasteriser
        alphaPassthrough = material.diffuse.a;

    alphaPassthrough *= actorFade * alpha;
}

#endif
