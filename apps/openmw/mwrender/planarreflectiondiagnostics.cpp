#include "planarreflectiondiagnostics.hpp"

#include "renderdiagnosticcapture.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <osg/FrameStamp>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Texture>
#include <osg/ref_ptr>

namespace MWRender
{
    namespace
    {
        constexpr unsigned int retainedFrames = 8;

        struct PlanarReflectionAttachments
        {
            osg::ref_ptr<osg::Texture> mColor;
            osg::ref_ptr<osg::Texture> mDepth;
        };

        std::mutex reflectionMutex;
        using ReflectionMap = std::map<std::pair<unsigned int, unsigned int>, PlanarReflectionAttachments>;
        ReflectionMap dominantReflections;
        ReflectionMap comparisonReflections;

        /// Retain one completed attachment pair and discard records outside the bounded diagnostic window.
        void publish(ReflectionMap& reflections, unsigned int context, unsigned int frame, osg::Texture& color,
            osg::Texture& depth)
        {
            reflections[{ context, frame }] = { &color, &depth };
            std::erase_if(reflections, [context, frame](const auto& entry) {
                return entry.first.first == context && frame > entry.first.second
                    && frame - entry.first.second > retainedFrames;
            });
        }

        /// Return one retained attachment pair without holding the registry lock during readback.
        std::optional<PlanarReflectionAttachments> findPublished(
            const ReflectionMap& reflections, unsigned int context, unsigned int frame)
        {
            const auto found = reflections.find({ context, frame });
            if (found == reflections.end())
                return std::nullopt;
            return found->second;
        }

        /// Append one named color/depth pair when its render-thread publication exists.
        void capturePublished(std::vector<RenderDiagnosticImage>& images, RenderDiagnosticCapture& capture,
            const std::optional<PlanarReflectionAttachments>& reflection, std::string_view name)
        {
            if (!reflection)
                return;
            if (auto image = capture.captureColorTexture(reflection->mColor, std::string(name) + "-color"))
                images.emplace_back(std::move(*image));
            if (auto image = capture.captureDepthTexture(reflection->mDepth, std::string(name) + "-depth"))
                images.emplace_back(std::move(*image));
        }
    }

    void publishDominantPlanarReflection(
        unsigned int context, unsigned int frame, osg::Texture& color, osg::Texture& depth)
    {
        std::lock_guard lock(reflectionMutex);
        publish(dominantReflections, context, frame, color, depth);
    }

    void publishComparisonPlanarReflection(
        unsigned int context, unsigned int frame, osg::Texture& color, osg::Texture& depth)
    {
        std::lock_guard lock(reflectionMutex);
        publish(comparisonReflections, context, frame, color, depth);
    }

    std::vector<RenderDiagnosticImage> capturePlanarReflectionDiagnostics(
        RenderDiagnosticCapture& capture, osg::RenderInfo& renderInfo)
    {
        osg::State* state = renderInfo.getState();
        if (state == nullptr || state->getFrameStamp() == nullptr)
            return {};

        std::optional<PlanarReflectionAttachments> dominant;
        std::optional<PlanarReflectionAttachments> comparison;
        {
            std::lock_guard lock(reflectionMutex);
            const unsigned int context = state->getContextID();
            const unsigned int frame = state->getFrameStamp()->getFrameNumber();
            dominant = findPublished(dominantReflections, context, frame);
            comparison = findPublished(comparisonReflections, context, frame);
        }
        std::vector<RenderDiagnosticImage> images;
        capturePublished(images, capture, dominant, "dominant-planar-reflection");
        capturePublished(images, capture, comparison, "comparison-planar-reflection");
        return images;
    }
}
