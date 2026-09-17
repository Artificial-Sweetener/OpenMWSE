#version 120

#include "lib/core/vertex.h.glsl"
#include "lib/core/instance.h.glsl"

varying vec4  position;
varying float linearDepth;

#include "shadows_vertex.glsl"
#include "fog.h.glsl"
#include "lib/view/depth.glsl"

uniform vec3 nodePosition;
uniform vec3 playerPos;
uniform int waterAppearance;
uniform float windSpeed;
uniform float geometricWaveHeight;
#if @waterGeometricWaves
uniform float osg_SimulationTime;
#define WATER_SURFACE_COVERAGE_VERTEX_STAGE
#include "lib/water/surface_coverage.glsl"
#undef WATER_SURFACE_COVERAGE_VERTEX_STAGE
#endif

#define WATER_OPTICAL_VERTEX_STAGE
#include "lib/water/optical_field.glsl"
#undef WATER_OPTICAL_VERTEX_STAGE

varying vec3 worldPos;
varying vec2 rippleMapUV;
varying vec3 passViewPos;
#if @waterGeometricWaves
varying float enclosedSurfaceCoverage;
#endif

#if @waterGeometricWaves
/// Evaluate the selected MGE XE displacement profile and classify cached near-surface coverage.
float visibleGeometricDisplacement(vec2 samplePosition, float cameraDistance, out float flatCoverage)
{
    float displacement = sampleWaterGeometricHeight(samplePosition, osg_SimulationTime, waterAppearance,
        cameraDistance, geometricWaveHeight, windSpeed, fog.start, fog.end);
    // Fine coverage constrains crests near shores and hulls. A fully covered 512-unit neighborhood identifies
    // enclosed surface mats while leaving bridges connected to exposed water geometrically active.
    float obstruction = step(0.000001, sampleWaterSurfaceCoverage(samplePosition));
    float constrained = min(displacement, 0.0) + max(displacement, 0.0) * (1.0 - obstruction);
    flatCoverage = sampleWaterEnclosedCoverage(samplePosition);
    return mix(constrained, 0.0, flatCoverage);
}
#endif

void main(void)
{
    vec4 localVertex = gl_Vertex;
#if @waterGeometricWaves
    localVertex.xy += playerPos.xy - nodePosition.xy;
#endif
    vec4 vertex = instanceWorld() * localVertex;
    vec3 vertexNormal = instanceNormal() * gl_Normal.xyz;
    worldPos = vertex.xyz + instanceWorldOffset(nodePosition);

#if @waterGeometricWaves
    vec3 cameraPosition = (gl_ModelViewMatrixInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float displacement = visibleGeometricDisplacement(
        worldPos.xy, distance(vertex.xyz, cameraPosition), enclosedSurfaceCoverage);
    if (waterAppearance == 2 && cameraPosition.z - vertex.z <= -1.0)
        displacement += 20.0;
    vertex.z += displacement;
    worldPos.z = vertex.z + instanceWorldOffset(nodePosition).z;
#endif

    gl_Position = modelToClip(vertex);
    position = vertex;
    rippleMapUV = (worldPos.xy - playerPos.xy + (@rippleMapSize * @rippleMapWorldScale / 2.0)) / @rippleMapSize / @rippleMapWorldScale;

    vec4 viewPos = modelToView(vertex);
    passViewPos = viewPos.xyz;
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    setupShadowCoords(viewPos, normalize(gl_NormalMatrix * vertexNormal));
}
