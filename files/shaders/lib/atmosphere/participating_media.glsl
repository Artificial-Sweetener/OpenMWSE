#ifndef OPENMWSE_PARTICIPATING_MEDIA_GLSL
#define OPENMWSE_PARTICIPATING_MEDIA_GLSL

/// Store ordered piecewise-constant local radiance and extinction over explicit world-unit ray intervals.
struct OpenMWSELocalMedia
{
    int count;
    float startDistance;
    float endDistance;
    vec2 intervals[15];
    vec4 segments[15];
};

/// Advance local media to a cloud sample while preserving front-to-back transmittance and visible moments.
void openmwseAdvanceLocalMedia(OpenMWSELocalMedia media, float endDistance, inout float cursor,
    inout vec3 transmittance, inout vec3 scatteredLight, inout float opacity, inout float distanceSum)
{
    if (media.count == 0 || endDistance <= cursor)
        return;
    float start = max(cursor, media.startDistance);
    float end = min(endDistance, media.endDistance);
    for (int index = 0; index < 15; ++index)
    {
        if (index >= media.count || start >= end)
            break;
        float begin = max(start, media.intervals[index].x);
        float finish = min(end, media.intervals[index].y);
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

/// Resolve local media when the ray has no contributing cloud interval.
vec4 openmwseIntegrateLocalMedia(
    OpenMWSELocalMedia media, float maximumDistance, out float distanceSum)
{
    float cursor = 0.0;
    vec3 transmittance = vec3(1.0);
    vec3 light = vec3(0.0);
    float opacity = 0.0;
    distanceSum = 0.0;
    openmwseAdvanceLocalMedia(media, maximumDistance, cursor, transmittance, light, opacity, distanceSum);
    return vec4(light, transmittance.x);
}

#endif
