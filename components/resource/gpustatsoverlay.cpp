#include "gpustatsoverlay.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

#include <osg/Camera>
#include <osg/Drawable>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Stats>
#include <osg/Switch>
#include <osg/Timer>
#include <osg/observer_ptr>

#include <osgText/Font>
#include <osgText/Text>

#include <osgViewer/ViewerBase>

namespace Resource
{
    namespace
    {
        /// Clear stale timeline-bar widths before OSG redraws the statistics geometry.
        class ClearStatsBars : public osg::Drawable::DrawCallback
        {
        public:
            /// Wrap the callback that updates and draws the bar geometry.
            explicit ClearStatsBars(osg::Drawable::DrawCallback* callback)
                : mCallback(callback)
            {
            }

            /// Reset horizontal extents before delegating to OSG's bar updater.
            void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
            {
                auto* geometry = const_cast<osg::Geometry*>(drawable->asGeometry());
                auto* vertices = static_cast<osg::Vec3Array*>(geometry->getVertexArray());
                for (osg::Vec3f& vertex : *vertices)
                    vertex.x() = 0;
                mCallback->drawImplementation(info, drawable);
            }

        private:
            osg::ref_ptr<osg::Drawable::DrawCallback> mCallback;
        };

        /// Keep the timing scale large enough for completed CPU and GPU camera work.
        class StatsTimeline : public osg::Camera::DrawCallback
        {
        public:
            /// Observe viewer and camera statistics for the configured timeline width.
            StatsTimeline(osg::Stats* stats, const osgViewer::ViewerBase::Cameras& cameras, double& multiplier,
                unsigned int frames, float width)
                : mStats(stats)
                , mMultiplier(multiplier)
                , mFrames(frames)
                , mWidth(width)
            {
                for (const osg::Camera* camera : cameras)
                    mCameras.emplace_back(camera->getStats());
            }

            /// Recalculate the time-to-pixel multiplier from the latest complete interval.
            void operator()(osg::RenderInfo& info) const override
            {
                const unsigned int frame = info.getState()->getFrameStamp()->getFrameNumber();
                double begin = 0;
                double end = 0;
                if (frame < mFrames || !mStats->getAttribute(frame - mFrames, "Reference time", begin)
                    || !mStats->getAttribute(frame, "Reference time", end))
                    return;
                for (const auto& camera : mCameras)
                    for (unsigned int i = frame - mFrames; i <= frame; ++i)
                        for (const char* name : { "Draw traversal end time", "GPU draw end time" })
                        {
                            double value;
                            if (camera->getAttribute(i, name, value))
                                end = std::max(end, value);
                        }
                mMultiplier = mWidth / std::max(end - begin, mFrames / 60.0);
            }

        private:
            osg::ref_ptr<osg::Stats> mStats;
            std::vector<osg::ref_ptr<const osg::Stats>> mCameras;
            double& mMultiplier;
            unsigned int mFrames;
            float mWidth;
        };

        constexpr std::pair<const char*, const char*> sSceneStats[] = {
            { "Point lights", "Scene point lights" },
            { "Triangles", "Visible number of GL_TRIANGLES" },
            { "GPU triangles", "GPU triangles" },
            { "CPU vertices", "Visible vertex count" },
            { "CPU drawables", "Visible number of drawables" },
            { "CPU prim sets", "Visible number of PrimitiveSets" },
            { "Render bins", "Visible number of render bins" },
            { "State graphs", "Number of StateGraphs" },
            { "Quads", "Visible number of GL_QUADS" },
        };

        /// Replace one camera statistics value column with complete CPU-plus-GPU counts.
        class CameraStats : public osg::Drawable::DrawCallback
        {
        public:
            /// Track one rendered camera and the viewer-owned GPU-scene counters.
            CameraStats(osg::Camera* camera, osg::Stats* viewerStats, unsigned int index)
                : mCamera(camera)
                , mViewerStats(viewerStats)
                , mIndex(index)
            {
            }

            /// Update the text periodically from the newest frame with a complete GPU query result.
            void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
            {
                auto* text = const_cast<osgText::Text*>(static_cast<const osgText::Text*>(drawable));
                const auto tick = osg::Timer::instance()->tick();
                if (osg::Timer::instance()->delta_m(mUpdated, tick) >= 100 && mCamera.valid())
                {
                    mUpdated = tick;
                    const osg::Stats& stats = *mCamera->getStats();
                    unsigned int frame = info.getState()->getFrameStamp()->getFrameNumber();
                    double gpu = 0;
                    double active = 0;
                    double value = 0;
                    bool complete = false;
                    while (frame > stats.getEarliestFrameNumber())
                    {
                        --frame;
                        gpu = 0;
                        complete = stats.getAttribute(frame, "Visible number of GL_TRIANGLES", value)
                            && (!mViewerStats->getAttribute(frame, "GPU scene active", active)
                                || mViewerStats->getAttribute(frame, "GPU triangles", gpu));
                        if (complete)
                            break;
                    }
                    std::ostringstream stream;
                    stream << std::fixed << std::setprecision(0) << '#' << mIndex << '\n' << mCamera->getName() << '\n';
                    for (const auto& [label, name] : sSceneStats)
                    {
                        const std::string_view attribute(name);
                        if (complete && attribute == "GPU triangles")
                            stream << gpu;
                        else if (complete && stats.getAttribute(frame, name, value))
                            stream << value + (attribute == "Visible number of GL_TRIANGLES" ? gpu : 0);
                        else
                            stream << '.';
                        stream << '\n';
                    }
                    text->setText(stream.str());
                }
                text->drawImplementation(info);
            }

        private:
            osg::observer_ptr<osg::Camera> mCamera;
            osg::ref_ptr<osg::Stats> mViewerStats;
            unsigned int mIndex;
            mutable osg::Timer_t mUpdated = 0;
        };

        /// Rebind OSG's scene-stat labels and values to the extended OpenMWSE camera table.
        class CameraStatsVisitor : public osg::NodeVisitor
        {
        public:
            /// Associate statistic text columns with cameras in display order.
            CameraStatsVisitor(const osgViewer::ViewerBase::Cameras& cameras, osg::Stats* stats, float characterSize)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mCameras(cameras)
                , mStats(stats)
                , mCharacterSize(characterSize)
            {
            }

            /// Replace camera text callbacks and resize the surrounding background geometry.
            void apply(osg::Drawable& drawable) override
            {
                if (auto* text = dynamic_cast<osgText::Text*>(&drawable))
                {
                    if (text->getDrawCallback() != nullptr && mIndex < mCameras.size())
                    {
                        const unsigned int index = mIndex++;
                        text->setDrawCallback(new CameraStats(mCameras[index], mStats, index));
                    }
                    else if (text->getDrawCallback() == nullptr)
                    {
                        std::string labels = "Camera\n\n";
                        for (const auto& stat : sSceneStats)
                            labels += std::string(stat.first) + '\n';
                        text->setText(labels);
                    }
                }
                else if (auto* geometry = drawable.asGeometry())
                {
                    auto* vertices = static_cast<osg::Vec3Array*>(geometry->getVertexArray());
                    (*vertices)[1].y() = (*vertices)[2].y()
                        = (*vertices)[0].y() - (std::size(sSceneStats) + 2) * mCharacterSize - 10;
                    vertices->dirty();
                }
            }

        private:
            const osgViewer::ViewerBase::Cameras& mCameras;
            osg::Stats* mStats;
            float mCharacterSize;
            unsigned int mIndex = 0;
        };

        /// Apply the configured font and clarify OSG's CPU-only scene label.
        class SetFontVisitor : public osg::NodeVisitor
        {
        public:
            /// Traverse text drawables using the supplied optional font.
            explicit SetFontVisitor(osgText::Font* font)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mFont(font)
            {
            }

            /// Update one text drawable owned by the profiler overlay.
            void apply(osg::Drawable& drawable) override
            {
                if (auto* text = dynamic_cast<osgText::Text*>(&drawable))
                {
                    if (mFont != nullptr)
                        text->setFont(mFont);
                    const std::string value = text->getText().createUTF8EncodedString();
                    if (value.starts_with("View") && value.find("Stateset") != std::string::npos)
                        text->setText("CPU scene" + value.substr(4));
                }
            }

        private:
            osgText::Font* mFont;
        };
    }

    void configureGpuStatsOverlay(osg::Switch& overlay, unsigned int cameraSceneChild, osg::Geode& timelineBars,
        osg::Camera& camera, const osgViewer::ViewerBase::Cameras& cameras, osg::Stats* viewerStats,
        osgText::Font* font, double& blockMultiplier, unsigned int frames, float timelineWidth, float characterSize)
    {
        SetFontVisitor fontVisitor(font);
        overlay.accept(fontVisitor);

        CameraStatsVisitor sceneVisitor(cameras, viewerStats, characterSize);
        overlay.getChild(cameraSceneChild)->accept(sceneVisitor);
        for (unsigned int i = 0; i < timelineBars.getNumDrawables(); ++i)
        {
            osg::Drawable* drawable = timelineBars.getDrawable(i);
            if (drawable->asGeometry() != nullptr && drawable->getDrawCallback() != nullptr)
                drawable->setDrawCallback(new ClearStatsBars(drawable->getDrawCallback()));
        }
        camera.addPreDrawCallback(new StatsTimeline(viewerStats, cameras, blockMultiplier, frames, timelineWidth));
    }
}
