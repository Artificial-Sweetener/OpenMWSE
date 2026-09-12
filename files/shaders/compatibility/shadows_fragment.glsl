#define SHADOWS @shadows_enabled

#if SHADOWS
    uniform float maximumShadowMapDistance;
    uniform float shadowFadeStart;
    uniform vec2 shadowMapTexelSize;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        uniform sampler2DShadow shadowTexture@shadow_texture_unit_index;
        varying vec4 shadowSpaceCoords@shadow_texture_unit_index;

#if @perspectiveShadowMaps
        varying vec4 shadowRegionCoords@shadow_texture_unit_index;
#endif
    @endforeach
#endif // SHADOWS

#if SHADOWS && @shadowFilteringQuality > 0
/// Compare one projected receiver after shifting it by a stable shadow-map texel offset.
float sampleShadowOffset(sampler2DShadow shadowMap, vec4 coordinates, vec2 texelOffset)
{
    coordinates.xy += texelOffset * shadowMapTexelSize * coordinates.w;
    return shadow2DProj(shadowMap, coordinates).r;
}

/// Reconstruct a deterministic tent-filtered visibility value using hardware bilinear comparisons.
float filteredShadow(sampler2DShadow shadowMap, vec4 coordinates)
{
#if @shadowFilteringQuality == 1
    return shadow2DProj(shadowMap, coordinates).r;
#elif @shadowFilteringQuality == 2
    return 0.25 * (
        sampleShadowOffset(shadowMap, coordinates, vec2(-0.65, -0.65))
        + sampleShadowOffset(shadowMap, coordinates, vec2(0.65, -0.65))
        + sampleShadowOffset(shadowMap, coordinates, vec2(-0.65, 0.65))
        + sampleShadowOffset(shadowMap, coordinates, vec2(0.65, 0.65)));
#elif @shadowFilteringQuality == 3
    float visibility = 0.0;
    visibility += sampleShadowOffset(shadowMap, coordinates, vec2(-1.0, -1.0));
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, vec2(0.0, -1.0));
    visibility += sampleShadowOffset(shadowMap, coordinates, vec2(1.0, -1.0));
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, vec2(-1.0, 0.0));
    visibility += 4.0 * shadow2DProj(shadowMap, coordinates).r;
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, vec2(1.0, 0.0));
    visibility += sampleShadowOffset(shadowMap, coordinates, vec2(-1.0, 1.0));
    visibility += 2.0 * sampleShadowOffset(shadowMap, coordinates, vec2(0.0, 1.0));
    visibility += sampleShadowOffset(shadowMap, coordinates, vec2(1.0, 1.0));
    return visibility * 0.0625;
#else
    const vec4 weights = vec4(1.0, 3.0, 3.0, 1.0);
    const vec4 offsets = vec4(-1.5, -0.5, 0.5, 1.5);
    float visibility = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            visibility += weights[x] * weights[y]
                * sampleShadowOffset(shadowMap, coordinates, vec2(offsets[x], offsets[y]));
    return visibility * (1.0 / 64.0);
#endif
}

/// Weight the preferred cascade continuously across the overlap retained by the legacy selector.
float shadowCascadeInteriorWeight(vec2 coordinates)
{
    float edgeDistance = min(min(coordinates.x, 1.0 - coordinates.x),
        min(coordinates.y, 1.0 - coordinates.y));
    return smoothstep(0.02, 0.06, edgeDistance);
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
            if (all(lessThan(shadowXYZ.xy, vec2(1.0, 1.0))) && all(greaterThan(shadowXYZ.xy, vec2(0.0, 0.0))))
            {
#if @shadowFilteringQuality == 0
                shadowing = min(shadow2DProj(shadowTexture@shadow_texture_unit_index, shadowSpaceCoords@shadow_texture_unit_index).r, shadowing);
                doneShadows = all(lessThan(shadowXYZ, vec3(0.95, 0.95, 1.0))) && all(greaterThan(shadowXYZ, vec3(0.05, 0.05, 0.0)));
#if @perspectiveShadowMaps
                doneShadows = doneShadows && all(lessThan(shadowRegionXYZ, vec3(1.0, 1.0, 1.0))) && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
#else
                float cascadeShadow = filteredShadow(
                    shadowTexture@shadow_texture_unit_index, shadowSpaceCoords@shadow_texture_unit_index);
                if (!haveShadow)
                {
                    shadowing = cascadeShadow;
                    preferredCascadeWeight = shadowCascadeInteriorWeight(shadowXYZ.xy);
                    haveShadow = true;
                    doneShadows = preferredCascadeWeight >= 0.999;
#if @perspectiveShadowMaps
                    doneShadows = doneShadows && all(lessThan(shadowRegionXYZ, vec3(1.0, 1.0, 1.0)))
                        && all(greaterThan(shadowRegionXYZ.xy, vec2(-1.0, -1.0)));
#endif
                }
                else
                {
                    shadowing = mix(cascadeShadow, shadowing, preferredCascadeWeight);
                    doneShadows = true;
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
