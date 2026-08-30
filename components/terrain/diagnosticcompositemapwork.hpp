#ifndef OPENMW_COMPONENTS_TERRAIN_DIAGNOSTICCOMPOSITEMAPWORK_H
#define OPENMW_COMPONENTS_TERRAIN_DIAGNOSTICCOMPOSITEMAPWORK_H

#include <cstddef>
#include <string_view>

namespace osg
{
    class RenderInfo;
}

namespace Terrain
{
    /// Names the recorded scheduler budget in milliseconds.
    inline constexpr std::string_view diagnosticCompositeMapBudgetStatistic
        = "OpenMW diagnostic terrain composite budget milliseconds";
    /// Names the queued non-immediate map count observed before compilation.
    inline constexpr std::string_view diagnosticCompositeMapQueuedBeforeStatistic
        = "OpenMW diagnostic terrain composite queued maps before";
    /// Names the immediate map count observed before compilation.
    inline constexpr std::string_view diagnosticCompositeMapImmediateBeforeStatistic
        = "OpenMW diagnostic terrain composite immediate maps before";
    /// Names the map count attempted during the camera pass.
    inline constexpr std::string_view diagnosticCompositeMapProcessedStatistic
        = "OpenMW diagnostic terrain composite processed maps";
    /// Names the drawable count submitted while compiling composite maps.
    inline constexpr std::string_view diagnosticCompositeMapSubmittedDrawablesStatistic
        = "OpenMW diagnostic terrain composite submitted drawables";
    /// Names the map count completed during the camera pass.
    inline constexpr std::string_view diagnosticCompositeMapCompletedStatistic
        = "OpenMW diagnostic terrain composite completed maps";
    /// Names the queued non-immediate map count remaining after compilation.
    inline constexpr std::string_view diagnosticCompositeMapQueuedAfterStatistic
        = "OpenMW diagnostic terrain composite queued maps after";

    /// Describes the bounded composite-map work attempted during one draw traversal.
    struct DiagnosticCompositeMapWork
    {
        double mBudgetMilliseconds = 0.0;
        std::size_t mQueuedBefore = 0;
        std::size_t mImmediateBefore = 0;
        std::size_t mProcessed = 0;
        std::size_t mSubmittedDrawables = 0;
        std::size_t mCompleted = 0;
        std::size_t mQueuedAfter = 0;
    };

    /// Record one frame of composite-map scheduling work when bounded diagnostics are active.
    void recordDiagnosticCompositeMapWork(osg::RenderInfo& renderInfo, const DiagnosticCompositeMapWork& work);
}

#endif
