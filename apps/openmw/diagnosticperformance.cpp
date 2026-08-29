#include "diagnosticperformance.hpp"

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

        /// Collect one metric across the requested frame interval.
        std::vector<double> collectMetric(
            const Metric& metric, unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer)
        {
            osg::Stats* stats = viewer.getViewerStats();
            if (metric.mOwner == Metric::Owner::PrimaryCamera)
            {
                osgViewer::Viewer::Cameras cameras;
                viewer.getCameras(cameras);
                if (cameras.empty())
                    return {};
                stats = cameras.front()->getStats();
            }

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

        /// Emit one named distribution or an explicit unavailable marker.
        void reportDistribution(std::ostream& output, const Metric& metric, std::span<const double> values)
        {
            output << "  " << metric.mName << ":\n";
            if (values.empty())
            {
                output << "    available: false\n";
                return;
            }
            const DiagnosticDistribution distribution = summarizeDiagnosticSamples(values);
            output << "    available: true\n"
                   << "    count: " << distribution.mCount << '\n'
                   << "    median: " << distribution.mMedian << '\n'
                   << "    p95: " << distribution.mP95 << '\n'
                   << "    p99: " << distribution.mP99 << '\n'
                   << "    worst: " << distribution.mWorst << '\n';
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
            { "GPU draw time taken", Metric::Owner::PrimaryCamera },
        };
        forEachUserStatsValue(
            [&](const UserStats& value) { metrics.push_back({ value.mTaken, Metric::Owner::Viewer }); });

        output << std::setprecision(17) << "provider: performance@2\n"
               << "first-sampled-frame: " << firstFrame << '\n'
               << "last-sampled-frame: " << lastFrame << '\n'
               << "requested-samples: " << lastFrame - firstFrame + 1 << '\n'
               << "distributions:\n";
        for (const Metric& metric : metrics)
            reportDistribution(output, metric, collectMetric(metric, firstFrame, lastFrame, viewer));

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
