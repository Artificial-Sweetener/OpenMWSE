#include "diagnosticperformance.hpp"

#include "diagnosticrenderingtiming.hpp"
#include "diagnosticworkcounter.hpp"
#include "mwrender/renderworkcounter.hpp"
#include "profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <osg/Camera>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <components/sceneutil/diagnosticcamerawork.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/diagnosticshadowwork.hpp>
#include <components/sdlutil/diagnosticpresentationtiming.hpp>
#include <components/terrain/diagnosticcompositemapwork.hpp>

namespace OMW
{
    namespace
    {
        /// Return a linearly interpolated quantile from sorted values.
        double quantile(std::span<const double> sorted, double probability)
        {
            const double index = probability * static_cast<double>(sorted.size() - 1);
            const std::size_t lower = static_cast<std::size_t>(std::floor(index));
            const std::size_t upper = static_cast<std::size_t>(std::ceil(index));
            const double fraction = index - static_cast<double>(lower);
            return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
        }

        /// Describe one stable statistic and the OSG owner that records it.
        struct Metric
        {
            enum class Owner
            {
                Viewer,
                PrimaryCamera,
            };

            std::string mName;
            Owner mOwner = Owner::Viewer;
        };

        /// Maps one stable diagnostic field to the OSG statistic that supplies it.
        struct CameraMetric
        {
            std::string_view mName;
            std::string_view mStatistic;
        };

        constexpr std::array cameraDetailMetrics{
            CameraMetric{ "render-leaves", SceneUtil::diagnosticShadowRenderLeafCountStatistic },
            CameraMetric{ "state-graphs", SceneUtil::diagnosticShadowStateGraphCountStatistic },
        };

        constexpr std::array terrainCompositeMetrics{
            CameraMetric{ "budget-milliseconds", Terrain::diagnosticCompositeMapBudgetStatistic },
            CameraMetric{ "queued-maps-before", Terrain::diagnosticCompositeMapQueuedBeforeStatistic },
            CameraMetric{ "immediate-maps-before", Terrain::diagnosticCompositeMapImmediateBeforeStatistic },
            CameraMetric{ "processed-maps", Terrain::diagnosticCompositeMapProcessedStatistic },
            CameraMetric{ "submitted-drawables", Terrain::diagnosticCompositeMapSubmittedDrawablesStatistic },
            CameraMetric{ "completed-maps", Terrain::diagnosticCompositeMapCompletedStatistic },
            CameraMetric{ "queued-maps-after", Terrain::diagnosticCompositeMapQueuedAfterStatistic },
            CameraMetric{ "drawable-cpu-milliseconds", Terrain::diagnosticCompositeMapDrawableCpuStatistic },
            CameraMetric{ "camera-setup-time", SceneUtil::diagnosticCpuPassSetupTimeStatistic },
            CameraMetric{ "camera-setup-thread-cycles", SceneUtil::diagnosticCpuPassSetupThreadCycleStatistic },
            CameraMetric{ "camera-draw-time", SceneUtil::diagnosticCpuPassDrawTimeStatistic },
            CameraMetric{ "camera-draw-thread-cycles", SceneUtil::diagnosticCpuPassDrawThreadCycleStatistic },
            CameraMetric{ "camera-completion-time", SceneUtil::diagnosticCpuPassCompletionTimeStatistic },
            CameraMetric{
                "camera-completion-thread-cycles", SceneUtil::diagnosticCpuPassCompletionThreadCycleStatistic },
        };

        /// Identifies one stable gap between adjacent draw-attribution boundaries.
        struct CameraTransition
        {
            std::string mFrom;
            std::string mTo;

            /// Order transition identities for deterministic reporting.
            bool operator<(const CameraTransition& other) const
            {
                return std::tie(mFrom, mTo) < std::tie(other.mFrom, other.mTo);
            }
        };

        /// Retains draw-stage timing distributions and their named transition gaps.
        struct DrawAttribution
        {
            std::vector<double> mCameraCovered;
            std::vector<double> mBeforeFirstCamera;
            std::vector<double> mBetweenCameras;
            std::vector<double> mAfterLastCamera;
            std::vector<double> mOutsideCameras;
            std::map<CameraTransition, std::vector<double>> mTransitionGaps;
        };

        /// Retains elapsed GPU-clock intervals across instrumented camera boundaries.
        struct GpuAttribution
        {
            std::vector<double> mCameraSpan;
            std::vector<double> mCameraCovered;
            std::vector<double> mBetweenCameras;
            std::map<CameraTransition, std::vector<double>> mTransitionGaps;
        };

        /// Return the stable diagnostic name for the viewer's effective threading model.
        std::string_view threadingModelName(osgViewer::ViewerBase::ThreadingModel model)
        {
            switch (model)
            {
                case osgViewer::ViewerBase::SingleThreaded:
                    return "single-threaded";
                case osgViewer::ViewerBase::CullDrawThreadPerContext:
                    return "cull-draw-thread-per-context";
                case osgViewer::ViewerBase::DrawThreadPerContext:
                    return "draw-thread-per-context";
                case osgViewer::ViewerBase::CullThreadPerCameraDrawThreadPerContext:
                    return "cull-thread-per-camera-draw-thread-per-context";
                case osgViewer::ViewerBase::AutomaticSelection:
                    return "automatic-selection";
            }
            throw std::logic_error("Unsupported OSG viewer threading model");
        }

        /// Represents one clipped camera-owned wall-clock interval.
        struct CameraInterval
        {
            std::string mName;
            double mBegin = 0.0;
            double mEnd = 0.0;
        };

        /// Convert a stable work-counter definition into a statistics query.
        Metric counterMetric(const DiagnosticWorkCounter& counter)
        {
            return { std::string(counter.mStatistic),
                counter.mOwner == DiagnosticStatisticOwner::Viewer ? Metric::Owner::Viewer
                                                                   : Metric::Owner::PrimaryCamera };
        }

        /// Collect one metric across the requested frame interval.
        std::vector<double> collectMetric(
            const Metric& metric, unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer)
        {
            osg::Stats* stats = viewer.getViewerStats();
            if (metric.mOwner == Metric::Owner::PrimaryCamera)
                stats = viewer.getCamera()->getStats();

            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double value = 0.0;
                if (stats->getAttribute(frame, metric.mName, value) && std::isfinite(value))
                    values.push_back(value);
            }
            return values;
        }

        /// Collect and sum one named camera pass across the requested frame interval.
        std::vector<double> collectCameraPass(
            std::string_view cameraName, std::string_view statistic, unsigned firstFrame, unsigned lastFrame)
        {
            const std::vector<osg::ref_ptr<osg::Camera>> cameras = SceneUtil::diagnosticGpuPassCameras();
            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double total = 0.0;
                bool available = false;
                for (const osg::ref_ptr<osg::Camera>& camera : cameras)
                {
                    if (camera->getName() != cameraName)
                        continue;
                    double value = 0.0;
                    if (camera->getStats()->getAttribute(frame, std::string(statistic), value) && std::isfinite(value))
                    {
                        total += value;
                        available = true;
                    }
                }
                if (available)
                    values.push_back(total);
            }
            return values;
        }

        /// Collect and sum one diagnostic statistic across every registered camera.
        std::vector<double> collectAllCameraPasses(std::string_view statistic, unsigned firstFrame, unsigned lastFrame)
        {
            const std::vector<osg::ref_ptr<osg::Camera>> cameras = SceneUtil::diagnosticGpuPassCameras();
            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double total = 0.0;
                bool available = false;
                for (const osg::ref_ptr<osg::Camera>& camera : cameras)
                {
                    double value = 0.0;
                    if (camera->getStats()->getAttribute(frame, std::string(statistic), value) && std::isfinite(value))
                    {
                        total += value;
                        available = true;
                    }
                }
                if (available)
                    values.push_back(total);
            }
            return values;
        }

        /// Return whether one camera belongs to a versioned aggregate pass.
        bool isNamedCameraPass(const osg::Camera& camera)
        {
            return std::ranges::any_of(SceneUtil::diagnosticGpuPasses(),
                [&camera](const SceneUtil::DiagnosticGpuPass& pass) { return camera.getName() == pass.mCameraName; });
        }

        /// Collect one camera's statistic without merging it with cameras that share its name.
        std::vector<double> collectCameraStatistic(
            const osg::Camera& camera, std::string_view statistic, unsigned firstFrame, unsigned lastFrame)
        {
            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double value = 0.0;
                if (camera.getStats()->getAttribute(frame, std::string(statistic), value) && std::isfinite(value))
                    values.push_back(value);
            }
            return values;
        }

        /// Collect and sum one statistic across physical cameras that represent the same logical pass instance.
        std::vector<double> collectCameraGroupStatistic(std::span<const osg::ref_ptr<osg::Camera>> cameras,
            std::string_view statistic, unsigned firstFrame, unsigned lastFrame)
        {
            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double total = 0.0;
                bool available = false;
                for (const osg::ref_ptr<osg::Camera>& camera : cameras)
                {
                    double value = 0.0;
                    if (camera->getStats()->getAttribute(frame, std::string(statistic), value) && std::isfinite(value))
                    {
                        total += value;
                        available = true;
                    }
                }
                if (available)
                    values.push_back(total);
            }
            return values;
        }

        /// Collect one statistic across cameras that do not belong to a versioned named pass.
        std::vector<double> collectUnclassifiedCameraPasses(
            std::string_view statistic, unsigned firstFrame, unsigned lastFrame)
        {
            const std::vector<osg::ref_ptr<osg::Camera>> cameras = SceneUtil::diagnosticGpuPassCameras();
            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double total = 0.0;
                bool available = false;
                for (const osg::ref_ptr<osg::Camera>& camera : cameras)
                {
                    if (isNamedCameraPass(*camera))
                        continue;
                    double value = 0.0;
                    if (camera->getStats()->getAttribute(frame, std::string(statistic), value) && std::isfinite(value))
                    {
                        total += value;
                        available = true;
                    }
                }
                if (available)
                    values.push_back(total);
            }
            return values;
        }

        /// Collect one primary-camera statistic across the requested frame interval.
        std::vector<double> collectPrimaryCameraStatistic(
            std::string_view statistic, unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer)
        {
            osg::Stats* stats = viewer.getCamera()->getStats();

            std::vector<double> values;
            values.reserve(lastFrame - firstFrame + 1);
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double value = 0.0;
                if (stats->getAttribute(frame, std::string(statistic), value) && std::isfinite(value))
                {
                    values.push_back(value);
                }
            }
            return values;
        }

        /// Divide the OSG draw traversal into camera-covered intervals and named surrounding transitions.
        DrawAttribution collectDrawAttribution(unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer)
        {
            DrawAttribution result;
            osg::Stats* primaryStats = viewer.getCamera()->getStats();
            const std::vector<osg::ref_ptr<osg::Camera>> cameras = SceneUtil::diagnosticGpuPassCameras();
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double drawBegin = 0.0;
                double drawEnd = 0.0;
                if (!primaryStats->getAttribute(frame, "Draw traversal begin time", drawBegin)
                    || !primaryStats->getAttribute(frame, "Draw traversal end time", drawEnd) || drawEnd < drawBegin)
                    continue;

                std::vector<CameraInterval> intervals;
                for (const osg::ref_ptr<osg::Camera>& camera : cameras)
                {
                    double begin = 0.0;
                    double end = 0.0;
                    if (!camera->getStats()->getAttribute(
                            frame, std::string(SceneUtil::diagnosticCpuPassBeginStatistic), begin)
                        || !camera->getStats()->getAttribute(
                            frame, std::string(SceneUtil::diagnosticCpuPassEndStatistic), end)
                        || end < begin || end < drawBegin || begin > drawEnd)
                        continue;
                    intervals.push_back({ .mName = camera->getName().empty() ? "<unnamed-camera>" : camera->getName(),
                        .mBegin = std::max(begin, drawBegin),
                        .mEnd = std::min(end, drawEnd) });
                }
                if (intervals.empty())
                    continue;

                std::ranges::sort(intervals, [](const CameraInterval& left, const CameraInterval& right) {
                    return std::tie(left.mBegin, left.mEnd, left.mName)
                        < std::tie(right.mBegin, right.mEnd, right.mName);
                });
                const double firstCameraBegin = intervals.front().mBegin;
                double mergedBegin = intervals.front().mBegin;
                double mergedEnd = intervals.front().mEnd;
                std::string mergedName = intervals.front().mName;
                double cameraCovered = 0.0;
                double betweenCameras = 0.0;
                for (const CameraInterval& interval : std::span(intervals).subspan(1))
                {
                    if (interval.mBegin <= mergedEnd)
                    {
                        if (interval.mEnd > mergedEnd)
                        {
                            mergedEnd = interval.mEnd;
                            mergedName = interval.mName;
                        }
                        continue;
                    }
                    cameraCovered += mergedEnd - mergedBegin;
                    const double gap = interval.mBegin - mergedEnd;
                    betweenCameras += gap;
                    result.mTransitionGaps[{ mergedName, interval.mName }].push_back(gap);
                    mergedBegin = interval.mBegin;
                    mergedEnd = interval.mEnd;
                    mergedName = interval.mName;
                }
                cameraCovered += mergedEnd - mergedBegin;
                const double beforeFirstCamera = std::max(0.0, firstCameraBegin - drawBegin);
                const double afterLastCamera = std::max(0.0, drawEnd - mergedEnd);
                const double outsideCameras = std::max(0.0, drawEnd - drawBegin - cameraCovered);
                result.mCameraCovered.push_back(cameraCovered);
                result.mBeforeFirstCamera.push_back(beforeFirstCamera);
                result.mBetweenCameras.push_back(betweenCameras);
                result.mAfterLastCamera.push_back(afterLastCamera);
                result.mOutsideCameras.push_back(outsideCameras);
                result.mTransitionGaps[{ "<draw-begin>", intervals.front().mName }].push_back(beforeFirstCamera);
                result.mTransitionGaps[{ mergedName, "<draw-end>" }].push_back(afterLastCamera);
            }
            return result;
        }

        /// Attribute elapsed GPU-clock intervals between instrumented camera boundaries.
        GpuAttribution collectGpuAttribution(unsigned firstFrame, unsigned lastFrame)
        {
            GpuAttribution result;
            const std::vector<osg::ref_ptr<osg::Camera>> cameras = SceneUtil::diagnosticGpuPassCameras();
            for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
            {
                std::vector<CameraInterval> intervals;
                for (const osg::ref_ptr<osg::Camera>& camera : cameras)
                {
                    double begin = 0.0;
                    double end = 0.0;
                    if (!camera->getStats()->getAttribute(
                            frame, std::string(SceneUtil::diagnosticGpuPassBeginTimestampStatistic), begin)
                        || !camera->getStats()->getAttribute(
                            frame, std::string(SceneUtil::diagnosticGpuPassEndTimestampStatistic), end)
                        || !std::isfinite(begin) || !std::isfinite(end) || end < begin)
                        continue;
                    intervals.push_back({ .mName = camera->getName().empty() ? "<unnamed-camera>" : camera->getName(),
                        .mBegin = begin,
                        .mEnd = end });
                }
                if (intervals.empty())
                    continue;

                std::ranges::sort(intervals, [](const CameraInterval& left, const CameraInterval& right) {
                    return std::tie(left.mBegin, left.mEnd, left.mName)
                        < std::tie(right.mBegin, right.mEnd, right.mName);
                });
                const double firstCameraBegin = intervals.front().mBegin;
                double mergedBegin = firstCameraBegin;
                double mergedEnd = intervals.front().mEnd;
                std::string mergedName = intervals.front().mName;
                double cameraCovered = 0.0;
                double betweenCameras = 0.0;
                for (const CameraInterval& interval : std::span(intervals).subspan(1))
                {
                    if (interval.mBegin <= mergedEnd)
                    {
                        if (interval.mEnd > mergedEnd)
                        {
                            mergedEnd = interval.mEnd;
                            mergedName = interval.mName;
                        }
                        continue;
                    }
                    cameraCovered += mergedEnd - mergedBegin;
                    const double gap = interval.mBegin - mergedEnd;
                    betweenCameras += gap;
                    result.mTransitionGaps[{ mergedName, interval.mName }].push_back(gap);
                    mergedBegin = interval.mBegin;
                    mergedEnd = interval.mEnd;
                    mergedName = interval.mName;
                }
                cameraCovered += mergedEnd - mergedBegin;
                const double cameraSpan = mergedEnd - firstCameraBegin;
                result.mCameraSpan.push_back(cameraSpan);
                result.mBetweenCameras.push_back(betweenCameras);
                result.mCameraCovered.push_back(cameraCovered);
            }
            return result;
        }

        /// Emit one named distribution or an explicit unavailable marker.
        void reportDistribution(std::ostream& output, const Metric& metric, std::span<const double> values,
            std::string_view indentation = "  ")
        {
            output << indentation << metric.mName << ":\n";
            if (values.empty())
            {
                output << indentation << "  available: false\n";
                return;
            }
            const DiagnosticDistribution distribution = summarizeDiagnosticSamples(values);
            output << indentation << "  available: true\n"
                   << indentation << "  count: " << distribution.mCount << '\n'
                   << indentation << "  median: " << distribution.mMedian << '\n'
                   << indentation << "  p95: " << distribution.mP95 << '\n'
                   << indentation << "  p99: " << distribution.mP99 << '\n'
                   << indentation << "  worst: " << distribution.mWorst << '\n';
        }

        /// Emit one camera's bounded timing and submitted-scene distributions.
        void reportCameraDetails(std::ostream& output, std::span<const osg::ref_ptr<osg::Camera>> cameras,
            unsigned firstFrame, unsigned lastFrame, std::string_view indentation)
        {
            reportDistribution(output, { "gpu-time", Metric::Owner::PrimaryCamera },
                collectCameraGroupStatistic(cameras, SceneUtil::diagnosticGpuPassTimeStatistic, firstFrame, lastFrame),
                indentation);
            reportDistribution(output, { "cpu-time", Metric::Owner::PrimaryCamera },
                collectCameraGroupStatistic(cameras, SceneUtil::diagnosticCpuPassTimeStatistic, firstFrame, lastFrame),
                indentation);
            for (const CameraMetric& metric : cameraDetailMetrics)
            {
                reportDistribution(output, { std::string(metric.mName), Metric::Owner::PrimaryCamera },
                    collectCameraGroupStatistic(cameras, metric.mStatistic, firstFrame, lastFrame), indentation);
            }
        }
    }

    DiagnosticDistribution summarizeDiagnosticSamples(std::span<const double> samples)
    {
        if (samples.empty())
            throw std::invalid_argument("diagnostic performance distribution requires at least one sample");
        std::vector<double> sorted(samples.begin(), samples.end());
        if (std::ranges::any_of(sorted, [](double value) { return !std::isfinite(value); }))
            throw std::invalid_argument("diagnostic performance distribution contains a non-finite sample");
        std::ranges::sort(sorted);
        return { .mCount = sorted.size(),
            .mMedian = quantile(sorted, 0.5),
            .mP95 = quantile(sorted, 0.95),
            .mP99 = quantile(sorted, 0.99),
            .mWorst = sorted.back() };
    }

    void reportDiagnosticPerformance(
        unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer, std::ostream& output)
    {
        if (firstFrame > lastFrame)
            throw std::invalid_argument("diagnostic performance frame interval is reversed");

        std::vector<Metric> metrics{
            { "Frame duration", Metric::Owner::Viewer },
            { "Event traversal time taken", Metric::Owner::Viewer },
            { "Update traversal time taken", Metric::Owner::Viewer },
            { "Rendering traversals time taken", Metric::Owner::Viewer },
            { "Cull traversal time taken", Metric::Owner::PrimaryCamera },
            { "Draw traversal time taken", Metric::Owner::PrimaryCamera },
        };
        forEachUserStatsValue(
            [&](const UserStats& value) { metrics.push_back({ value.mTaken, Metric::Owner::Viewer }); });

        output << std::setprecision(17) << "provider: performance@11\n"
               << "threading-model: " << threadingModelName(viewer.getThreadingModel()) << '\n'
               << "first-sampled-frame: " << firstFrame << '\n'
               << "last-sampled-frame: " << lastFrame << '\n'
               << "requested-samples: " << lastFrame - firstFrame + 1 << '\n'
               << "distributions:\n";
        for (const Metric& metric : metrics)
            reportDistribution(output, metric, collectMetric(metric, firstFrame, lastFrame, viewer));

        output << "presentation:\n";
        const Metric swapBuffersSource{ std::string(SDLUtil::diagnosticSwapBufferTimeStatistic),
            Metric::Owner::Viewer };
        reportDistribution(output, { "swap-buffers", Metric::Owner::Viewer },
            collectMetric(swapBuffersSource, firstFrame, lastFrame, viewer));

        const DrawAttribution drawAttribution = collectDrawAttribution(firstFrame, lastFrame, viewer);
        output << "draw-breakdown:\n";
        reportDistribution(output, { "camera-covered", Metric::Owner::PrimaryCamera }, drawAttribution.mCameraCovered);
        reportDistribution(
            output, { "before-first-camera", Metric::Owner::PrimaryCamera }, drawAttribution.mBeforeFirstCamera);
        reportDistribution(
            output, { "between-cameras", Metric::Owner::PrimaryCamera }, drawAttribution.mBetweenCameras);
        reportDistribution(
            output, { "after-last-camera", Metric::Owner::PrimaryCamera }, drawAttribution.mAfterLastCamera);
        reportDistribution(
            output, { "outside-cameras", Metric::Owner::PrimaryCamera }, drawAttribution.mOutsideCameras);

        output << "camera-transition-gaps:\n";
        for (const auto& [transition, values] : drawAttribution.mTransitionGaps)
        {
            output << "  - from: " << std::quoted(transition.mFrom) << '\n'
                   << "    to: " << std::quoted(transition.mTo) << '\n';
            reportDistribution(output, { "gap-time", Metric::Owner::PrimaryCamera }, values, "    ");
        }

        const GpuAttribution gpuAttribution = collectGpuAttribution(firstFrame, lastFrame);
        output << "gpu-timeline:\n";
        reportDistribution(output, { "camera-span", Metric::Owner::PrimaryCamera }, gpuAttribution.mCameraSpan);
        reportDistribution(output, { "camera-covered", Metric::Owner::PrimaryCamera }, gpuAttribution.mCameraCovered);
        reportDistribution(output, { "between-cameras", Metric::Owner::PrimaryCamera }, gpuAttribution.mBetweenCameras);
        output << "  transition-gaps:\n";
        for (const auto& [transition, values] : gpuAttribution.mTransitionGaps)
        {
            output << "    - from: " << std::quoted(transition.mFrom) << '\n'
                   << "      to: " << std::quoted(transition.mTo) << '\n';
            reportDistribution(output, { "gap-time", Metric::Owner::PrimaryCamera }, values, "      ");
        }

        output << "diagnostic-camera-callback-overhead:\n";
        reportDistribution(output, { "before-interval", Metric::Owner::PrimaryCamera },
            collectAllCameraPasses(SceneUtil::diagnosticCpuPassBeginOverheadStatistic, firstFrame, lastFrame));
        reportDistribution(output, { "after-interval", Metric::Owner::PrimaryCamera },
            collectAllCameraPasses(SceneUtil::diagnosticCpuPassEndOverheadStatistic, firstFrame, lastFrame));

        output << "gpu-passes:\n";
        for (const SceneUtil::DiagnosticGpuPass& pass : SceneUtil::diagnosticGpuPasses())
        {
            const Metric metric{ std::string(pass.mName), Metric::Owner::PrimaryCamera };
            reportDistribution(output, metric,
                collectCameraPass(pass.mCameraName, SceneUtil::diagnosticGpuPassTimeStatistic, firstFrame, lastFrame));
        }
        reportDistribution(output, { "unclassified-cameras", Metric::Owner::PrimaryCamera },
            collectUnclassifiedCameraPasses(SceneUtil::diagnosticGpuPassTimeStatistic, firstFrame, lastFrame));

        output << "cpu-passes:\n";
        for (const SceneUtil::DiagnosticGpuPass& pass : SceneUtil::diagnosticGpuPasses())
        {
            const Metric metric{ std::string(pass.mName), Metric::Owner::PrimaryCamera };
            reportDistribution(output, metric,
                collectCameraPass(pass.mCameraName, SceneUtil::diagnosticCpuPassTimeStatistic, firstFrame, lastFrame));
        }
        reportDistribution(output, { "unclassified-cameras", Metric::Owner::PrimaryCamera },
            collectUnclassifiedCameraPasses(SceneUtil::diagnosticCpuPassTimeStatistic, firstFrame, lastFrame));

        output << "render-thread-cycles:\n";
        for (const SceneUtil::DiagnosticGpuPass& pass : SceneUtil::diagnosticGpuPasses())
        {
            reportDistribution(output, { std::string(pass.mName), Metric::Owner::PrimaryCamera },
                collectCameraPass(
                    pass.mCameraName, SceneUtil::diagnosticCpuPassThreadCycleStatistic, firstFrame, lastFrame));
        }
        reportDistribution(output, { "unclassified-cameras", Metric::Owner::PrimaryCamera },
            collectUnclassifiedCameraPasses(SceneUtil::diagnosticCpuPassThreadCycleStatistic, firstFrame, lastFrame));

        output << "shadow-map-details:\n";
        std::map<unsigned, std::vector<osg::ref_ptr<osg::Camera>>> shadowMaps;
        std::vector<osg::ref_ptr<osg::Camera>> unindexedShadowMaps;
        for (const osg::ref_ptr<osg::Camera>& camera : SceneUtil::diagnosticGpuPassCameras())
        {
            if (camera->getName() != SceneUtil::shadowCameraName)
                continue;
            const std::optional<unsigned> instance = SceneUtil::diagnosticGpuPassInstance(*camera);
            if (instance.has_value())
                shadowMaps[*instance].push_back(camera);
            else
                unindexedShadowMaps.push_back(camera);
        }
        for (const auto& [instance, cameras] : shadowMaps)
        {
            output << "  - index: " << instance + 1 << '\n' << "    physical-camera-count: " << cameras.size() << '\n';
            reportCameraDetails(output, cameras, firstFrame, lastFrame, "    ");
        }
        for (const osg::ref_ptr<osg::Camera>& camera : unindexedShadowMaps)
        {
            const std::array cameras{ camera };
            output << "  - index: unavailable\n"
                   << "    physical-camera-count: 1\n";
            reportCameraDetails(output, cameras, firstFrame, lastFrame, "    ");
        }

        output << "terrain-composite-details:\n";
        for (const CameraMetric& metric : terrainCompositeMetrics)
        {
            reportDistribution(output, { std::string(metric.mName), Metric::Owner::PrimaryCamera },
                collectCameraPass(SceneUtil::terrainCompositeCameraName, metric.mStatistic, firstFrame, lastFrame));
        }

        output << "unclassified-camera-details:\n";
        std::size_t unclassifiedCameraIndex = 0;
        for (const osg::ref_ptr<osg::Camera>& camera : SceneUtil::diagnosticGpuPassCameras())
        {
            if (isNamedCameraPass(*camera))
                continue;
            output << "  - index: " << ++unclassifiedCameraIndex << '\n'
                   << "    name: " << std::quoted(camera->getName()) << '\n';
            reportDistribution(output, { "gpu-time", Metric::Owner::PrimaryCamera },
                collectCameraStatistic(*camera, SceneUtil::diagnosticGpuPassTimeStatistic, firstFrame, lastFrame),
                "    ");
            reportDistribution(output, { "cpu-time", Metric::Owner::PrimaryCamera },
                collectCameraStatistic(*camera, SceneUtil::diagnosticCpuPassTimeStatistic, firstFrame, lastFrame),
                "    ");
        }

        output << "groundcover-passes:\n";
        for (const SceneUtil::DiagnosticGpuPass& pass : SceneUtil::diagnosticGpuPasses())
        {
            const std::string prefix = std::string(pass.mName) + '.';
            reportDistribution(output, { prefix + "instance-geometry-pairs-submitted", Metric::Owner::PrimaryCamera },
                collectCameraPass(pass.mCameraName, MWRender::groundcoverInstanceRenderCounter, firstFrame, lastFrame));
            reportDistribution(output, { prefix + "primitive-near-far-tests", Metric::Owner::PrimaryCamera },
                collectCameraPass(pass.mCameraName, MWRender::groundcoverInstanceTestCounter, firstFrame, lastFrame));
        }

        output << "gpu-render-bins:\n";
        for (const DiagnosticRenderBinPass& pass : diagnosticRenderBinPasses())
        {
            const Metric metric{ std::string(pass.mName), Metric::Owner::PrimaryCamera };
            reportDistribution(
                output, metric, collectPrimaryCameraStatistic(pass.mGpuStatistic, firstFrame, lastFrame, viewer));
        }

        output << "cpu-render-bins:\n";
        for (const DiagnosticRenderBinPass& pass : diagnosticRenderBinPasses())
        {
            const Metric metric{ std::string(pass.mName), Metric::Owner::PrimaryCamera };
            reportDistribution(
                output, metric, collectPrimaryCameraStatistic(pass.mCpuStatistic, firstFrame, lastFrame, viewer));
        }

        output << "work-counters:\n";
        for (const DiagnosticWorkCounter& counter : diagnosticWorkCounters())
        {
            const Metric metric = counterMetric(counter);
            output << "  " << counter.mName << ":\n";
            const std::vector<double> values = collectMetric(metric, firstFrame, lastFrame, viewer);
            if (values.empty())
            {
                output << "    available: false\n";
                continue;
            }
            const DiagnosticDistribution distribution = summarizeDiagnosticSamples(values);
            output << "    available: true\n"
                   << "    count: " << distribution.mCount << '\n'
                   << "    median: " << distribution.mMedian << '\n'
                   << "    p95: " << distribution.mP95 << '\n'
                   << "    p99: " << distribution.mP99 << '\n'
                   << "    worst: " << distribution.mWorst << '\n';
        }

        output << "raw-statistics:\n";
        for (unsigned frame = firstFrame; frame <= lastFrame; ++frame)
        {
            viewer.getViewerStats()->report(output, frame);
            osgViewer::Viewer::Cameras cameras;
            viewer.getCameras(cameras);
            for (osg::Camera* camera : cameras)
                camera->getStats()->report(output, frame);
        }
    }
}
