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
#if @waterMeshSurface
uniform vec3 waterPlaneNormal;
uniform vec3 waterPlaneTangent;
uniform vec3 waterPlaneBitangent;
uniform float waterPlaneDistance;
#endif
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
#if @waterMeshSurface
    flatCoverage = 0.0;
    return displacement;
#else
    // Fine coverage constrains crests near shores and hulls. A fully covered 512-unit neighborhood identifies
    // enclosed surface mats while leaving bridges connected to exposed water geometrically active.
    float obstruction = step(0.000001, sampleWaterSurfaceCoverage(samplePosition));
    float constrained = min(displacement, 0.0) + max(displacement, 0.0) * (1.0 - obstruction);
    flatCoverage = sampleWaterEnclosedCoverage(samplePosition);
    return mix(constrained, 0.0, flatCoverage);
#endif
}
#endif

void main(void)
{
    vec4 localVertex = gl_Vertex;
#if @waterGeometricWaves
#if !@waterMeshSurface
    localVertex.xy += playerPos.xy - nodePosition.xy;
#endif
#endif
    vec4 vertex = instanceWorld() * localVertex;
    vec3 vertexNormal = instanceNormal() * gl_Normal.xyz;
#if @waterMeshSurface
    vec4 viewPos = modelToView(vertex);
    // CPU scene transforms may be relative to a paging parent while fitted planes use authored world coordinates.
    // GPU instances already carry their complete placement, so instanceWorldOffset resolves this correction to zero.
    vec3 renderedNodePosition
        = (osg_ViewMatrixInverse * modelToView(vec4(0.0, 0.0, 0.0, 1.0))).xyz;
    vec3 meshWorldOffset = instanceWorldOffset(nodePosition - renderedNodePosition);
    worldPos = (osg_ViewMatrixInverse * viewPos).xyz + meshWorldOffset;
#else
    worldPos = vertex.xyz + instanceWorldOffset(nodePosition);
#endif

#if @waterGeometricWaves
#if @waterMeshSurface
    vec3 cameraPosition = osg_ViewMatrixInverse[3].xyz;
    float cameraDistance = distance(worldPos, cameraPosition);
#else
    vec3 cameraPosition = (gl_ModelViewMatrixInverse * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float cameraDistance = distance(vertex.xyz, cameraPosition);
#endif
    vec2 geometricSamplePosition = worldPos.xy;
#if @waterMeshSurface
    geometricSamplePosition = vec2(dot(worldPos, waterPlaneTangent), dot(worldPos, waterPlaneBitangent));
#endif
    float displacement = visibleGeometricDisplacement(
        geometricSamplePosition, cameraDistance, enclosedSurfaceCoverage);
#if @waterMeshSurface
    if (waterAppearance == 2 && dot(cameraPosition, waterPlaneNormal) - waterPlaneDistance <= -1.0)
#else
    if (waterAppearance == 2 && cameraPosition.z - vertex.z <= -1.0)
#endif
        displacement += 20.0;
#if @waterMeshSurface
    vec3 displacementNormal = normalize(vertexNormal);
    if (dot(displacementNormal, waterPlaneNormal) < 0.0)
        displacementNormal = -displacementNormal;
    vertex.xyz += displacementNormal * displacement;
    viewPos = modelToView(vertex);
    worldPos = (osg_ViewMatrixInverse * viewPos).xyz + meshWorldOffset;
#else
    vertex.z += displacement;
    worldPos.z = vertex.z + instanceWorldOffset(nodePosition).z;
#endif
#endif

    gl_Position = modelToClip(vertex);
    position = vertex;
    rippleMapUV = (worldPos.xy - playerPos.xy + (@rippleMapSize * @rippleMapWorldScale / 2.0)) / @rippleMapSize / @rippleMapWorldScale;

#if !@waterMeshSurface
    vec4 viewPos = modelToView(vertex);
#endif
    passViewPos = viewPos.xyz;
    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    setupShadowCoords(viewPos, normalize(gl_NormalMatrix * vertexNormal));
}
