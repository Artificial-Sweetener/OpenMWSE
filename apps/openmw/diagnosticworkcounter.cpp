#include "diagnosticworkcounter.hpp"

#include "mwrender/renderworkcounter.hpp"

#include <components/sceneutil/deformationstats.hpp>

#include <array>

namespace OMW
{
    std::span<const DiagnosticWorkCounter> diagnosticWorkCounters() noexcept
    {
        static constexpr std::array counters{
            DiagnosticWorkCounter{ "static.visible-drawables", "Visible number of drawables",
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "static.visible-vertices", "Visible vertex count",
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "static.visible-triangles", "Visible number of GL_TRIANGLES",
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "static.state-graphs", "Number of StateGraphs",
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "static.ordered-leaves", "Number of ordered leaves",
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "static.object-chunks", "Object Chunk Count" },
            DiagnosticWorkCounter{ "resolve.color-copies", MWRender::colorCopyCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "resolve.depth-copies", MWRender::depthCopyCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "resolve.transferred-bytes", MWRender::resolveByteCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "transparent.classified-leaves", MWRender::transparentClassifiedLeafCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "transparent.replayed-drawables", MWRender::transparentReplayCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "first-person.replayed-drawables", MWRender::firstPersonReplayCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "actors.registered", "Mechanics Actors" },
            DiagnosticWorkCounter{ "actors.broad-phase-candidates", "Diagnostic Actor Broad Phase Candidates" },
            DiagnosticWorkCounter{ "actors.exact-distance-checks", "Diagnostic Actor Exact Distance Checks" },
            DiagnosticWorkCounter{ "lights.visible", "Visible number of lights",
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "lights.scanned", "Diagnostic Lights Scanned" },
            DiagnosticWorkCounter{ "lights.exact-intersections", "Diagnostic Light Exact Intersections" },
            DiagnosticWorkCounter{ "lights.selected", "Diagnostic Lights Selected" },
            DiagnosticWorkCounter{ "groundcover.chunks", "Groundcover Chunk Count" },
            DiagnosticWorkCounter{ "groundcover.instances-tested", MWRender::groundcoverInstanceTestCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "groundcover.instances-rendered", MWRender::groundcoverInstanceRenderCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "deformation.skinning-vertices", SceneUtil::skinningVertexCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "deformation.native-skinning-vertices", SceneUtil::nativeSkinningVertexCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "deformation.osga-skinning-vertices", SceneUtil::osgaSkinningVertexCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "deformation.morph-vertices", SceneUtil::morphVertexCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "deformation.cpu-milliseconds", SceneUtil::deformationCpuMillisecondCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "uploads.bytes", SceneUtil::deformationUploadByteCounter,
                DiagnosticStatisticOwner::PrimaryCamera },
            DiagnosticWorkCounter{ "cache.object-chunk-gets", "Object Chunk Get" },
            DiagnosticWorkCounter{ "cache.object-chunk-hits", "Object Chunk Hit" },
            DiagnosticWorkCounter{ "cache.groundcover-chunk-gets", "Groundcover Chunk Get" },
            DiagnosticWorkCounter{ "cache.groundcover-chunk-hits", "Groundcover Chunk Hit" },
            DiagnosticWorkCounter{ "fallback.vertex-order", "Diagnostic Vertex Order Fallbacks" },
            DiagnosticWorkCounter{ "fallback.attachment-copy", "Diagnostic Attachment Copy Fallbacks" },
            DiagnosticWorkCounter{ "fallback.actor-broad-phase", "Diagnostic Actor Broad Phase Fallbacks" },
            DiagnosticWorkCounter{ "fallback.light-broad-phase", "Diagnostic Light Broad Phase Fallbacks" },
            DiagnosticWorkCounter{ "fallback.static-packet", "Diagnostic Static Packet Fallbacks" },
            DiagnosticWorkCounter{ "fallback.transparent-depth", "Diagnostic Transparent Depth Fallbacks" },
            DiagnosticWorkCounter{ "fallback.first-person-depth", "Diagnostic First Person Depth Fallbacks" },
            DiagnosticWorkCounter{ "fallback.gpu-deformation", "Diagnostic GPU Deformation Fallbacks" },
        };
        return counters;
    }
}
