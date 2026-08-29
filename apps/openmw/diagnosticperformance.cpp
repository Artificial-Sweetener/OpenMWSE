#include "diagnosticperformance.hpp"

#include "diagnosticrenderingtiming.hpp"
#include "diagnosticworkcounter.hpp"
#include "mwrender/renderworkcounter.hpp"
#include "profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Camera>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <components/sceneutil/diagnosticgputiming.hpp>

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

        /// Retains disjoint draw-stage timing distributions derived from camera callback intervals.
        struct DrawBreakdown
        {
            std::vector<double> mCameraCovered;
            std::vector<double> mBeforeFirstCamera;
            std::vector<double> mBetweenCameras;
            std::vector<double> mAfterLastCamera;
            std::vector<double> mOutsideCameras;
        };

        /// Represents one clipped camera-owned wall-clock interval.
        struct CameraInterval
        {
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

        /// Collect camera CPU intervals that do not belong to a versioned named pass.
        std::vector<double> collectUnclassifiedCameraPasses(unsigned firstFrame, unsigned lastFrame)
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
                    if (camera->getStats()->getAttribute(
                            frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), value)
                        && std::isfinite(value))
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

        /// Divide the OSG draw traversal into camera-covered and surrounding orchestration intervals.
        DrawBreakdown collectDrawBreakdown(unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer)
        {
            DrawBreakdown result;
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
                    intervals.push_back({ .mBegin = std::max(begin, drawBegin), .mEnd = std::min(end, drawEnd) });
                }
                if (intervals.empty())
                    continue;

                std::ranges::sort(intervals, {}, &CameraInterval::mBegin);
                const double firstCameraBegin = intervals.front().mBegin;
                double mergedBegin = intervals.front().mBegin;
                double mergedEnd = intervals.front().mEnd;
                double cameraCovered = 0.0;
                double betweenCameras = 0.0;
                for (const CameraInterval& interval : std::span(intervals).subspan(1))
                {
                    if (interval.mBegin <= mergedEnd)
                    {
                        mergedEnd = std::max(mergedEnd, interval.mEnd);
                        continue;
                    }
                    cameraCovered += mergedEnd - mergedBegin;
                    betweenCameras += interval.mBegin - mergedEnd;
                    mergedBegin = interval.mBegin;
                    mergedEnd = interval.mEnd;
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

        output << std::setprecision(17) << "provider: performance@5\n"
               << "first-sampled-frame: " << firstFrame << '\n'
               << "last-sampled-frame: " << lastFrame << '\n'
               << "requested-samples: " << lastFrame - firstFrame + 1 << '\n'
               << "distributions:\n";
        for (const Metric& metric : metrics)
            reportDistribution(output, metric, collectMetric(metric, firstFrame, lastFrame, viewer));

        const DrawBreakdown drawBreakdown = collectDrawBreakdown(firstFrame, lastFrame, viewer);
        output << "draw-breakdown:\n";
        reportDistribution(output, { "camera-covered", Metric::Owner::PrimaryCamera }, drawBreakdown.mCameraCovered);
        reportDistribution(
            output, { "before-first-camera", Metric::Owner::PrimaryCamera }, drawBreakdown.mBeforeFirstCamera);
        reportDistribution(output, { "between-cameras", Metric::Owner::PrimaryCamera }, drawBreakdown.mBetweenCameras);
        reportDistribution(
            output, { "after-last-camera", Metric::Owner::PrimaryCamera }, drawBreakdown.mAfterLastCamera);
        reportDistribution(output, { "outside-cameras", Metric::Owner::PrimaryCamera }, drawBreakdown.mOutsideCameras);

        output << "gpu-passes:\n";
        for (const SceneUtil::DiagnosticGpuPass& pass : SceneUtil::diagnosticGpuPasses())
        {
            const Metric metric{ std::string(pass.mName), Metric::Owner::PrimaryCamera };
            reportDistribution(output, metric,
                collectCameraPass(pass.mCameraName, SceneUtil::diagnosticGpuPassTimeStatistic, firstFrame, lastFrame));
        }

        output << "cpu-passes:\n";
        for (const SceneUtil::DiagnosticGpuPass& pass : SceneUtil::diagnosticGpuPasses())
        {
            const Metric metric{ std::string(pass.mName), Metric::Owner::PrimaryCamera };
            reportDistribution(output, metric,
                collectCameraPass(pass.mCameraName, SceneUtil::diagnosticCpuPassTimeStatistic, firstFrame, lastFrame));
        }
        reportDistribution(output, { "unclassified-cameras", Metric::Owner::PrimaryCamera },
            collectUnclassifiedCameraPasses(firstFrame, lastFrame));

        output << "unclassified-camera-details:\n";
        std::size_t unclassifiedCameraIndex = 0;
        for (const osg::ref_ptr<osg::Camera>& camera : SceneUtil::diagnosticGpuPassCameras())
        {
            if (isNamedCameraPass(*camera))
                continue;
            output << "  - index: " << ++unclassifiedCameraIndex << '\n'
                   << "    name: " << std::quoted(camera->getName()) << '\n';
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
