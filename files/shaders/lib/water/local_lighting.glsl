#ifndef OPENMWSE_WATER_LOCAL_LIGHTING_GLSL
#define OPENMWSE_WATER_LOCAL_LIGHTING_GLSL

uniform bool waterInterior;
uniform int waterInteriorLocalLights;
uniform int waterExteriorLocalLights;

#if @lightingMethodClustered || @legacyLightList
#define WATER_LOCAL_LIGHTS_AVAILABLE 1
#else
#define WATER_LOCAL_LIGHTS_AVAILABLE 0
#endif

/// Return the bounded local-light budget selected for the current world-space lighting domain.
int waterLocalLightBudget()
{
#if WATER_LOCAL_LIGHTS_AVAILABLE
    int budget = waterInterior ? waterInteriorLocalLights : waterExteriorLocalLights;
    if (budget < 0)
        return 0;
    if (budget > 4)
        return 4;
    return budget;
#else
    return 0;
#endif
}

/// Transform one world-space water location into view space without requiring a second matrix uniform.
vec3 waterWorldPositionToView(vec3 worldPosition)
{
    vec3 relativePosition = worldPosition - osg_ViewMatrixInverse[3].xyz;
    return vec3(dot(osg_ViewMatrixInverse[0].xyz, relativePosition),
        dot(osg_ViewMatrixInverse[1].xyz, relativePosition),
        dot(osg_ViewMatrixInverse[2].xyz, relativePosition));
}

#if WATER_LOCAL_LIGHTS_AVAILABLE

/// Reserve the future shadow-system visibility boundary for one canonical local light.
float waterLocalLightVisibility(int index, vec3 surfaceViewPosition)
{
    return 1.0;
}

/// Rank one canonical light by the irradiance it can deliver to the nearby water surface.
void waterRankLocalLight(int index, vec3 surfaceViewPosition, inout ivec4 selected, inout vec4 scores)
{
    vec4 positionRadius = waterCanonicalLightPositionRadius(index);
    vec3 lightVector = positionRadius.xyz - surfaceViewPosition;
    float distanceToSurface = length(lightVector);
    if (distanceToSurface <= 0.001 || distanceToSurface >= positionRadius.w)
        return;

    vec3 sourceWorldPosition = (osg_ViewMatrixInverse * vec4(positionRadius.xyz, 1.0)).xyz;
    float sourceHeight = sourceWorldPosition.z - waterHeight;
    if (sourceHeight <= 1.0)
        return;

    float luminance = dot(waterCanonicalLightColor(index), vec3(0.2126, 0.7152, 0.0722));
    float incidence = clamp(sourceHeight / distanceToSurface, 0.0, 1.0);
    float score = luminance * waterCanonicalLightAttenuation(index, distanceToSurface)
        * mix(0.25, 1.0, incidence);
    if (score <= 0.0001)
        return;

    if (score > scores.x)
    {
        scores = vec4(score, scores.xyz);
        selected = ivec4(index, selected.xyz);
    }
    else if (score > scores.y)
    {
        scores = vec4(scores.x, score, scores.yz);
        selected = ivec4(selected.x, index, selected.yz);
    }
    else if (score > scores.z)
    {
        scores = vec4(scores.xy, score, scores.z);
        selected = ivec4(selected.xy, index, selected.z);
    }
    else if (score > scores.w)
    {
        scores.w = score;
        selected.w = index;
    }
}

/// Select at most four strongest canonical lights whose bounded influence reaches this water location.
ivec4 waterSelectLocalLights(vec2 screenCoordinate, vec3 receiverViewPosition, vec3 surfaceViewPosition)
{
    ivec4 selected = ivec4(-1);
    vec4 scores = vec4(0.0);
    if (waterLocalLightBudget() <= 0)
        return selected;

    ivec2 range = waterCanonicalLightRange(screenCoordinate, receiverViewPosition);
    for (int ordinal = 0; ordinal < range.y; ++ordinal)
        waterRankLocalLight(
            waterCanonicalLightIndex(range.x, ordinal), surfaceViewPosition, selected, scores);
    return selected;
}

/// Evaluate one selected local light's colored reflected and in-water surface response.
vec3 waterLocalLightSurfaceContribution(
    int index, vec3 surfaceViewPosition, vec3 viewNormal, vec3 viewDirection)
{
    if (index < 0)
        return vec3(0.0);
    vec4 positionRadius = waterCanonicalLightPositionRadius(index);
    vec3 lightVector = positionRadius.xyz - surfaceViewPosition;
    float distanceToSurface = length(lightVector);
    if (distanceToSurface <= 0.001 || distanceToSurface >= positionRadius.w)
        return vec3(0.0);
    vec3 lightDirection = lightVector / distanceToSurface;
    float facing = max(dot(viewNormal, lightDirection), 0.0);
    if (facing <= 0.0)
        return vec3(0.0);
    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float highlight = pow(max(dot(viewNormal, halfDirection), 0.0), 160.0);
    float attenuation = waterCanonicalLightAttenuation(index, distanceToSurface)
        * waterLocalLightVisibility(index, surfaceViewPosition);
    vec3 sourceColor = waterCanonicalLightColor(index);
    float surfaceScatter = 0.035 * mix(0.25, 1.0, facing);
    return sourceColor * (highlight + surfaceScatter) * attenuation;
}

/// Accumulate bounded local-light water radiance without evaluating unselected sources.
vec3 waterLocalLightSurfaceRadiance(vec2 screenCoordinate, vec3 surfaceViewPosition, vec3 viewNormal)
{
    int budget = waterLocalLightBudget();
    if (budget <= 0)
        return vec3(0.0);
    ivec4 selected = waterSelectLocalLights(
        screenCoordinate, surfaceViewPosition, surfaceViewPosition);
    vec3 viewDirection = normalize(-surfaceViewPosition);
    vec3 result = vec3(0.0);
    if (budget > 0)
        result += waterLocalLightSurfaceContribution(selected.x, surfaceViewPosition, viewNormal, viewDirection);
    if (budget > 1)
        result += waterLocalLightSurfaceContribution(selected.y, surfaceViewPosition, viewNormal, viewDirection);
    if (budget > 2)
        result += waterLocalLightSurfaceContribution(selected.z, surfaceViewPosition, viewNormal, viewDirection);
    if (budget > 3)
        result += waterLocalLightSurfaceContribution(selected.w, surfaceViewPosition, viewNormal, viewDirection);
    return result;
}

#else

/// Keep unlit shader permutations independent from point-light buffer declarations.
ivec4 waterSelectLocalLights(vec2 screenCoordinate, vec3 receiverViewPosition, vec3 surfaceViewPosition)
{
    return ivec4(-1);
}

/// Keep water surfaces unchanged when the active shader permutation has no canonical light list.
vec3 waterLocalLightSurfaceRadiance(vec2 screenCoordinate, vec3 surfaceViewPosition, vec3 viewNormal)
{
    return vec3(0.0);
}

#endif

#endif
