#include "diagnosticrenderingtiming.hpp"

#include "mwrender/renderbin.hpp"

#include <array>
#include <mutex>
#include <string>

#include <osgUtil/RenderBin>

#include <components/sceneutil/diagnosticgputiming.hpp>

namespace OMW
{
    namespace
    {
        constexpr std::array passes{
            DiagnosticRenderBinPass{ "early-sky", "RenderBin", MWRender::RenderBin_Sky,
                "OpenMW diagnostic GPU render bin early sky", "OpenMW diagnostic CPU render bin early sky",
                std::nullopt },
            DiagnosticRenderBinPass{ "scene-root", "RenderBin", MWRender::RenderBin_Default,
                "OpenMW diagnostic GPU render bin scene root", "OpenMW diagnostic CPU render bin scene root",
                std::nullopt },
            DiagnosticRenderBinPass{ "opaque-attachment-resolve", "OpaqueResolve", MWRender::RenderBin_OpaqueResolve,
                "OpenMW diagnostic GPU render bin opaque attachment resolve",
                "OpenMW diagnostic CPU render bin opaque attachment resolve", std::nullopt },
            DiagnosticRenderBinPass{ "transparency", "DepthSortedBin", MWRender::RenderBin_DepthSorted,
                "OpenMW diagnostic GPU render bin transparency", "OpenMW diagnostic CPU render bin transparency",
                std::nullopt },
            DiagnosticRenderBinPass{ "occlusion-query", "RenderBin", MWRender::RenderBin_OcclusionQuery,
                "OpenMW diagnostic GPU render bin occlusion query", "OpenMW diagnostic CPU render bin occlusion query",
                std::nullopt },
            DiagnosticRenderBinPass{ "first-person", "DepthClear", MWRender::RenderBin_FirstPerson,
                "OpenMW diagnostic GPU render bin first person", "OpenMW diagnostic CPU render bin first person",
                std::nullopt },
            DiagnosticRenderBinPass{ "sun-glare", "RenderBin", MWRender::RenderBin_SunGlare,
                "OpenMW diagnostic GPU render bin sun glare", "OpenMW diagnostic CPU render bin sun glare",
                std::nullopt },
            DiagnosticRenderBinPass{ "distortion", "Distortion", MWRender::RenderBin_Distortion,
                "OpenMW diagnostic GPU render bin distortion", "OpenMW diagnostic CPU render bin distortion",
                std::nullopt },
            DiagnosticRenderBinPass{ "reflected-sky", "RenderBin", MWRender::RenderBin_ReflectionSky,
                "OpenMW diagnostic GPU render bin reflected sky", "OpenMW diagnostic CPU render bin reflected sky",
                SceneUtil::reflectionCameraName },
        };
    }

    std::span<const DiagnosticRenderBinPass> diagnosticRenderBinPasses()
    {
        return passes;
    }

    void enableDiagnosticRenderBinTimings()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            for (const DiagnosticRenderBinPass& pass : passes)
            {
                if (osgUtil::RenderBin* prototype
                    = osgUtil::RenderBin::getRenderBinPrototype(std::string(pass.mPrototype)))
                {
                    SceneUtil::enableDiagnosticGpuRenderBinStatistics(
                        *prototype, pass.mBinNumber, pass.mGpuStatistic, pass.mCpuStatistic);
                }
            }
        });
    }
}
