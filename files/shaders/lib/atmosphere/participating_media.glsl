#ifndef OPENMWSE_PARTICIPATING_MEDIA_GLSL
#define OPENMWSE_PARTICIPATING_MEDIA_GLSL

/// Store piecewise-constant local radiance and extinction per world unit along one scene-clipped ray.
struct OpenMWSELocalMedia
{
    int count;
    float startDistance;
    float stepLength;
    vec4 segments[15];
};

/// Advance local media to a cloud sample while preserving front-to-back transmittance and visible moments.
void openmwseAdvanceLocalMedia(OpenMWSELocalMedia media, float endDistance, inout float cursor,
    inout vec3 transmittance, inout vec3 scatteredLight, inout float opacity, inout float distanceSum)
{
    if (media.count == 0 || endDistance <= cursor)
        return;
    float start = max(cursor, media.startDistance);
    float end = min(endDistance, media.startDistance + float(media.count) * media.stepLength);
    for (int index = 0; index < 15; ++index)
    {
        if (index >= media.count || start >= end)
            break;
        float segmentStart = media.startDistance + float(index) * media.stepLength;
        float begin = max(start, segmentStart);
        float finish = min(end, segmentStart + media.stepLength);
        if (finish <= begin)
            continue;
        float segmentTransmittance = exp(-media.segments[index].a * (finish - begin));
        vec3 contribution = transmittance * (1.0 - segmentTransmittance);
        float visibleOpacity = dot(contribution, vec3(1.0 / 3.0));
        scatteredLight += contribution * media.segments[index].rgb;
        opacity += visibleOpacity;
        distanceSum += visibleOpacity * (begin + finish) * 0.5;
        transmittance *= segmentTransmittance;
    }
    cursor = endDistance;
}

/// Apply the native aerial response to the visible local-medium integral.
vec3 openmwseLocalMediaRadiance(vec3 light, float opacity, float distanceSum, vec3 direction,
    OpenMWSEAerialPerspectiveParameters atmosphere)
{
    if (opacity <= 1e-5)
        return vec3(0.0);
    return openmwseApplyAerialPerspective(light / opacity, direction, distanceSum / opacity, atmosphere) * opacity;
}

/// Resolve local media when the ray has no contributing cloud interval.
vec4 openmwseIntegrateLocalMedia(OpenMWSELocalMedia media, float maximumDistance, vec3 direction,
    OpenMWSEAerialPerspectiveParameters atmosphere, out float distanceSum)
{
    float cursor = 0.0;
    vec3 transmittance = vec3(1.0);
    vec3 light = vec3(0.0);
    float opacity = 0.0;
    distanceSum = 0.0;
    openmwseAdvanceLocalMedia(media, maximumDistance, cursor, transmittance, light, opacity, distanceSum);
    return vec4(openmwseLocalMediaRadiance(light, opacity, distanceSum, direction, atmosphere), transmittance.x);
}

#endif
