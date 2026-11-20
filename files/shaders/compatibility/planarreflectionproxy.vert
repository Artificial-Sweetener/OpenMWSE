#version 120

#pragma import_defines(useGPUShader4)

#if @useGPUShader4
#extension GL_EXT_gpu_shader4: require
#extension GL_ARB_draw_instanced: require
#endif

uniform sampler2D sourceDepth;
uniform float proxyResolution;
uniform float sourceResolution;
uniform float sourceClearDepth;
uniform vec2 sourceDepthNdcScaleBias;
uniform mat4 sourceInverseProjection;
uniform mat4 sourceViewToWorld;
uniform mat4 sourceViewToTarget;
uniform mat4 targetProjection;
uniform vec2 targetNdcPadding;
uniform vec4 targetPlane;
uniform float sourceTargetCoincident;

varying vec2 sourceUV;
varying float targetPlaneDistance;

// Resolve one homogeneous source-camera sample without letting invalid depth create NaNs in later clip tests.
vec3 projectivePoint(vec4 point)
{
    float safeW = abs(point.w) > 0.000001 ? point.w : (point.w < 0.0 ? -0.000001 : 0.000001);
    return point.xyz / safeW;
}

// Reconstruct one source-view position from a native-depth sample and the source's final GPU projection.
vec3 sourceViewPosition(vec2 uv, float depth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    float ndcDepth = depth * sourceDepthNdcScaleBias.x + sourceDepthNdcScaleBias.y;
    return projectivePoint(sourceInverseProjection * vec4(ndc, ndcDepth, 1.0));
}

// Reconstruct a neighboring sample only when it belongs to a plausible continuation of the centre surface.
bool compatibleNeighbor(vec2 uv, vec3 centerViewPosition, out vec3 neighborViewPosition)
{
    float depth = texture2DLod(sourceDepth, uv, 0.0).r;
    if (abs(depth - sourceClearDepth) <= 0.000001)
        return false;
    neighborViewPosition = sourceViewPosition(uv, depth);
    float maximumSeparation = max(16.0, abs(centerViewPosition.z) * 0.02);
    return distance(neighborViewPosition, centerViewPosition) <= maximumSeparation;
}

// Estimate one geometric tangent without joining independent patches across a depth discontinuity.
vec3 sourceTangent(vec2 negativeUV, vec2 positiveUV, vec3 centerViewPosition, vec3 fallback)
{
    vec3 negativePosition;
    vec3 positivePosition;
    bool negativeValid = compatibleNeighbor(negativeUV, centerViewPosition, negativePosition);
    bool positiveValid = compatibleNeighbor(positiveUV, centerViewPosition, positivePosition);
    if (negativeValid && positiveValid)
        return positivePosition - negativePosition;
    if (positiveValid)
        return positivePosition - centerViewPosition;
    if (negativeValid)
        return centerViewPosition - negativePosition;
    return fallback;
}

void main()
{
#if @useGPUShader4
    float instance = float(gl_InstanceIDARB);
    vec2 pixel = vec2(mod(instance, proxyResolution), floor(instance / proxyResolution));
    vec2 centerUV = (pixel + vec2(0.5)) / proxyResolution;

    float sourceDepthValue = texture2DLod(sourceDepth, centerUV, 0.0).r;
    float patchValidity = step(0.000001, abs(sourceDepthValue - sourceClearDepth));

    vec2 cornerUV = (pixel + vec2(0.5) + gl_Vertex.xy) / proxyResolution;
    targetPlaneDistance = 0.0;
    if (sourceTargetCoincident > 0.5)
    {
        float ndcDepth = sourceDepthValue * sourceDepthNdcScaleBias.x + sourceDepthNdcScaleBias.y;
        gl_Position = vec4(cornerUV * 2.0 - 1.0, ndcDepth, 1.0);
    }
    else
    {
        vec3 centerViewPosition = sourceViewPosition(centerUV, sourceDepthValue);
        vec4 centerClipPosition = targetProjection * sourceViewToTarget * vec4(centerViewPosition, 1.0);

        vec2 texel = vec2(1.0 / sourceResolution);
        vec3 tangentX = sourceTangent(centerUV - vec2(texel.x, 0.0), centerUV + vec2(texel.x, 0.0),
            centerViewPosition, vec3(1.0, 0.0, 0.0));
        vec3 tangentY = sourceTangent(centerUV - vec2(0.0, texel.y), centerUV + vec2(0.0, texel.y),
            centerViewPosition, vec3(0.0, 1.0, 0.0));
        vec3 geometricNormal = cross(tangentX, tangentY);
        if (dot(geometricNormal, geometricNormal) <= 0.00000001)
            geometricNormal = vec3(0.0, 0.0, 1.0);
        else
            geometricNormal = normalize(geometricNormal);

        vec3 cornerRay = sourceViewPosition(cornerUV, sourceDepthValue);
        float cornerDenominator = dot(geometricNormal, cornerRay);
        float denominatorValidity = step(0.000001, abs(cornerDenominator));
        float safeDenominator = denominatorValidity > 0.5
            ? cornerDenominator
            : (cornerDenominator < 0.0 ? -0.000001 : 0.000001);
        float cornerScale = dot(geometricNormal, centerViewPosition) / safeDenominator;
        patchValidity *= denominatorValidity * step(0.000001, cornerScale);

        vec3 cornerViewPosition = cornerRay * cornerScale;
        vec4 worldPosition = sourceViewToWorld * vec4(cornerViewPosition, 1.0);
        targetPlaneDistance = dot(targetPlane, vec4(projectivePoint(worldPosition), 1.0));
        gl_Position = targetProjection * sourceViewToTarget * vec4(cornerViewPosition, 1.0);

        float cornerWValidity = step(0.000001, abs(gl_Position.w));
        float centerWValidity = step(0.000001, abs(centerClipPosition.w));
        patchValidity *= cornerWValidity * centerWValidity;
        float safeCornerW = cornerWValidity > 0.5
            ? gl_Position.w
            : (gl_Position.w < 0.0 ? -0.000001 : 0.000001);
        float safeCenterW = centerWValidity > 0.5
            ? centerClipPosition.w
            : (centerClipPosition.w < 0.0 ? -0.000001 : 0.000001);
        vec2 cornerNdc = gl_Position.xy / safeCornerW;
        vec2 centerNdc = centerClipPosition.xy / safeCenterW;
        gl_Position.xy = (cornerNdc + sign(cornerNdc - centerNdc) * targetNdcPadding) * gl_Position.w;
        if (patchValidity < 0.5)
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
    sourceUV = centerUV;
#else
    sourceUV = vec2(0.0);
    targetPlaneDistance = 0.0;
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
#endif
}
