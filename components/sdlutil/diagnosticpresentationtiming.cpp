#include "diagnosticpresentationtiming.hpp"

#include <osg/Stats>

#include <atomic>
#include <mutex>
#include <string>

namespace SDLUtil
{
    namespace
    {
        std::atomic_bool sCollectionEnabled = false;
        std::mutex sStatisticsMutex;
        osg::ref_ptr<osg::Stats> sStatistics;
    }

    void setDiagnosticPresentationTimingStatistics(osg::Stats* stats)
    {
        std::lock_guard lock(sStatisticsMutex);
        sStatistics = stats;
        sCollectionEnabled.store(stats != nullptr, std::memory_order_release);
    }

    bool diagnosticPresentationTimingCollectionEnabled() noexcept
    {
        return sCollectionEnabled.load(std::memory_order_acquire);
    }

    void recordDiagnosticSwapBufferTime(unsigned frameNumber, double seconds)
    {
        std::lock_guard lock(sStatisticsMutex);
        if (sStatistics != nullptr)
            sStatistics->setAttribute(frameNumber, std::string(diagnosticSwapBufferTimeStatistic), seconds);
    }
}
