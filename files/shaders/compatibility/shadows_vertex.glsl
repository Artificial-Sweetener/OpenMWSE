#define SHADOWS @shadows_enabled

#if SHADOWS
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
        uniform mat4 shadowSpaceMatrix@shadow_texture_unit_index;
        varying vec4 shadowSpaceCoords@shadow_texture_unit_index;
#if @shadowFilteringQuality > 0
        uniform float shadowNormalOffsetScale@shadow_texture_unit_index;
        uniform vec3 shadowLightDirection@shadow_texture_unit_index;
#endif

#if @perspectiveShadowMaps
        uniform mat4 validRegionMatrix@shadow_texture_unit_index;
        varying vec4 shadowRegionCoords@shadow_texture_unit_index;
#endif
    @endforeach

    // Enabling this may reduce peter panning. Probably unnecessary.
    const bool onlyNormalOffsetUV = false;
#endif // SHADOWS

#if SHADOWS && @shadowFilteringQuality > 0
/// Increase normal-offset protection at grazing sunlight angles without quantizing the light direction.
float shadowSurfaceOffsetScale(vec3 viewNormal, vec3 lightDirection)
{
    float lightFacing = abs(dot(normalize(viewNormal), normalize(lightDirection)));
    return mix(2.5, 1.0, smoothstep(0.15, 0.65, lightFacing));
}
#endif

void setupShadowCoords(vec4 viewPos, vec3 viewNormal)
{
#if SHADOWS
    vec4 shadowOffset;
    @foreach shadow_texture_unit_index @shadow_texture_unit_list
#if @perspectiveShadowMaps
        shadowRegionCoords@shadow_texture_unit_index = validRegionMatrix@shadow_texture_unit_index * viewPos;
#endif

#if @disableNormalOffsetShadows
        shadowSpaceCoords@shadow_texture_unit_index = shadowSpaceMatrix@shadow_texture_unit_index * viewPos;
#else
#if @shadowFilteringQuality > 0
        float shadowOffsetScale@shadow_texture_unit_index = shadowNormalOffsetScale@shadow_texture_unit_index
            * shadowSurfaceOffsetScale(viewNormal, shadowLightDirection@shadow_texture_unit_index);
        shadowOffset = vec4(viewNormal * @shadowNormalOffset * shadowOffsetScale@shadow_texture_unit_index, 0.0);
#else
        shadowOffset = vec4(viewNormal * @shadowNormalOffset, 0.0);
#endif

        if (onlyNormalOffsetUV)
        {
            vec4 lightSpaceXY = viewPos + shadowOffset;
            lightSpaceXY = shadowSpaceMatrix@shadow_texture_unit_index * lightSpaceXY;

            shadowSpaceCoords@shadow_texture_unit_index.xy = lightSpaceXY.xy;
        }
        else
        {
            vec4 offsetViewPosition = viewPos + shadowOffset;
            shadowSpaceCoords@shadow_texture_unit_index = shadowSpaceMatrix@shadow_texture_unit_index * offsetViewPosition;
        }
#endif
    @endforeach
#endif // SHADOWS
}
