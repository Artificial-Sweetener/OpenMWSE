#define SHADOWS @shadows_enabled

#if SHADOWS
    uniform float maximumShadowMapDistance;
    uniform float shadowFadeStart;
    uniform vec2 shadowMapTexelSize;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        uniform sampler2DShadow shadowTexture@shadow_texture_unit_index;
        varying vec4 shadowSpaceCoords@shadow_texture_unit_index;
#if @shadowFilteringQuality > 0
        uniform vec2 shadowCascadeRange@shadow_texture_unit_index;
        uniform vec2 shadowCascadeTransition@shadow_texture_unit_index;
        uniform vec2 shadowMapTexelSize@shadow_texture_unit_index;
        uniform float shadowFilterRadiusScale@shadow_texture_unit_index;
#endif

#if @perspectiveShadowMaps
        varying vec4 shadowRegionCoords@shadow_texture_unit_index;
#endif
    @endforeach
#endif // SHADOWS

#if SHADOWS && @shadowFilteringQuality > 0
/// Compare one projected receiver after shifting it by a stable shadow-map texel offset.
float sampleShadowOffset(
    sampler2DShadow shadowMap, vec4 coordinates, vec2 texelSize, vec2 texelOffset)
{
    coordinates.xy += texelOffset * texelSize * coordinates.w;
    return shadow2DProj(shadowMap, coordinates).r;
}

/// Reconstruct a deterministic tent-filtered visibility value using hardware bilinear comparisons.
float filteredShadow(sampler2DShadow shadowMap, vec4 coordinates, vec2 texelSize, float radiusScale)
{
#if @shadowFilteringQuality == 1
    return shadow2DProj(shadowMap, coordinates).r;
#elif @shadowFilteringQuality == 2
    return 0.25 * (
        sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(-0.65, -0.65) * radiusScale)
        + sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(0.65, -0.65) * radiusScale)
        + sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(-0.65, 0.65) * radiusScale)
        + sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(0.65, 0.65) * radiusScale));
#elif @shadowFilteringQuality == 3
    float radius = radiusScale;
#else
    float radius = 1.75 * radiusScale;
#endif
#if @shadowFilteringQuality >= 3
    float corner00 = sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(-radius, -radius));
    float corner10 = sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(radius, -radius));
    float corner01 = sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(-radius, radius));
    float corner11 = sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(radius, radius));
    float center = shadow2DProj(shadowMap, coordinates).r;
    float minimumProbe = min(center, min(min(corner00, corner10), min(corner01, corner11)));
    float maximumProbe = max(center, max(max(corner00, corner10), max(corner01, corner11)));
    if (minimumProbe >= 0.9999)
        return 1.0;
    if (maximumProbe <= 0.0001)
        return 0.0;
    float visibility = corner00 + corner10 + corner01 + corner11 + 4.0 * center;
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(0.0, -radius));
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(-radius, 0.0));
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(radius, 0.0));
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, texelSize, vec2(0.0, radius));
    return visibility * 0.0625;
#endif
}

#endif

float unshadowedLightRatio(float distance)
{
    float shadowing = 1.0;
#if SHADOWS
#if @limitShadowMapDistance
#if @shadowFilteringQuality == 0
    float fade = clamp((distance - shadowFadeStart) / (maximumShadowMapDistance - shadowFadeStart), 0.0, 1.0);
#else
    float fadeRange = max(maximumShadowMapDistance - shadowFadeStart, 0.0001);
    float fade = smoothstep(0.0, 1.0, clamp((distance - shadowFadeStart) / fadeRange, 0.0, 1.0));
#endif
    if (fade == 1.0)
        return shadowing;
#endif
    bool doneShadows = false;
#if @shadowFilteringQuality > 0
    bool haveShadow = false;
    float preferredCascadeWeight = 0.0;
#endif
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        if (!doneShadows)
        {
            vec3 shadowXYZ = shadowSpaceCoords@shadow_texture_unit_index.xyz / shadowSpaceCoords@shadow_texture_unit_index.w;
#if @perspectiveShadowMaps
            vec3 shadowRegionXYZ = shadowRegionCoords@shadow_texture_unit_index.xyz / shadowRegionCoords@shadow_texture_unit_index.w;
#endif
            bool shadowCoordinatesValid = all(lessThan(shadowXYZ.xy, vec2(1.0, 1.0)))
                && all(greaterThan(shadowXYZ.xy, vec2(0.0, 0.0)));
#if @shadowFilteringQuality > 0
            shadowCoordinatesValid = shadowCoordinatesValid
                && shadowXYZ.z >= 0.0
                && shadowXYZ.z <= 1.0
                && distance >= shadowCascadeRange@shadow_texture_unit_index.x
                && distance <= shadowCascadeRange@shadow_texture_unit_index.y;
#endif
            if (shadowCoordinatesValid)
            {
#if @shadowFilteringQuality == 0
                shadowing = min(shadow2DProj(shadowTexture@shadow_texture_unit_index, shadowSpaceCoords@shadow_texture_unit_index).r, shadowing);
                doneShadows = all(lessThan(shadowXYZ, vec3(0.95, 0.95, 1.0))) && all(greaterThan(shadowXYZ, vec3(0.05, 0.05, 0.0)));
#if @perspectiveShadowMaps
                doneShadows = doneShadows && all(lessThan(shadowRegionXYZ, vec3(1.0, 1.0, 1.0))) && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
#else
                float cascadeShadow = filteredShadow(
                    shadowTexture@shadow_texture_unit_index, shadowSpaceCoords@shadow_texture_unit_index,
                    shadowMapTexelSize@shadow_texture_unit_index,
                    shadowFilterRadiusScale@shadow_texture_unit_index);
                if (!haveShadow)
                {
                    shadowing = cascadeShadow;
                    vec2 transition = shadowCascadeTransition@shadow_texture_unit_index;
                    preferredCascadeWeight = transition.y > transition.x
                        ? 1.0 - smoothstep(transition.x, transition.y, distance)
                        : 1.0;
                    haveShadow = true;
                    bool cascadeComplete = distance < transition.x || transition.y <= transition.x;
#if @perspectiveShadowMaps
                    cascadeComplete = cascadeComplete && all(lessThan(shadowRegionXYZ, vec3(1.0, 1.0, 1.0)))
                        && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
                    if (cascadeComplete)
                    {
#if @limitShadowMapDistance
                        return mix(shadowing, 1.0, fade);
#else
                        return shadowing;
#endif
                    }
                }
                else
                {
                    shadowing = mix(cascadeShadow, shadowing, preferredCascadeWeight);
#if @limitShadowMapDistance
                    return mix(shadowing, 1.0, fade);
#else
                    return shadowing;
#endif
                }
#endif
            }
        }
    @endforeach
#if @limitShadowMapDistance
    shadowing = mix(shadowing, 1.0, fade);
#endif
#endif // SHADOWS
    return shadowing;
}

void applyShadowDebugOverlay()
{
#if SHADOWS && @useShadowDebugOverlay
    bool doneOverlay = false;
    float colourIndex = 0.0;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        if (!doneOverlay)
        {
            vec3 shadowXYZ = shadowSpaceCoords@shadow_texture_unit_index.xyz / shadowSpaceCoords@shadow_texture_unit_index.w;
#if @perspectiveShadowMaps
            vec3 shadowRegionXYZ = shadowRegionCoords@shadow_texture_unit_index.xyz / shadowRegionCoords@shadow_texture_unit_index.w;
#endif
            if (all(lessThan(shadowXYZ.xy, vec2(1.0, 1.0))) && all(greaterThan(shadowXYZ.xy, vec2(0.0, 0.0))))
            {
                colourIndex = mod(@shadow_texture_unit_index.0, 3.0);
                if (colourIndex < 1.0)
                    gl_FragData[0].x += 0.1;
                else if (colourIndex < 2.0)
                    gl_FragData[0].y += 0.1;
                else
                    gl_FragData[0].z += 0.1;

                doneOverlay = all(lessThan(shadowXYZ, vec3(0.95, 0.95, 1.0))) && all(greaterThan(shadowXYZ, vec3(0.05, 0.05, 0.0)));
#if @perspectiveShadowMaps
                doneOverlay = doneOverlay && all(lessThan(shadowRegionXYZ.xyz, vec3(1.0, 1.0, 1.0))) && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
            }
        }
    @endforeach
#endif // SHADOWS
}
