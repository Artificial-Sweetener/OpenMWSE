#include "diagnosticgputiming.hpp"

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/GLExtensions>
#include <osg/NodeVisitor>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>
#include <osg/observer_ptr>
#include <osgUtil/RenderBin>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace SceneUtil
{
    namespace
    {
        constexpr std::string_view collectionCategory = "diagnostic_gpu_passes";
        constexpr std::size_t retainedFrameCount = 1024;
        constexpr std::array passes{
            DiagnosticGpuPass{ "main-scene", sceneCameraName },
            DiagnosticGpuPass{ "shadow-maps", shadowCameraName },
            DiagnosticGpuPass{ "water-reflection", reflectionCameraName },
            DiagnosticGpuPass{ "sky-render-target", skyCameraName },
            DiagnosticGpuPass{ "post-processing", postProcessingCameraName },
            DiagnosticGpuPass{ "precipitation-occlusion", precipitationOcclusionCameraName },
            DiagnosticGpuPass{ "ripples", ripplesCameraName },
        };

        std::mutex registryMutex;
        std::vector<osg::observer_ptr<osg::Camera>> registeredCameras;

        /// Return whether the current context exposes timestamp-query entry points.
        bool supportsTimestamps(const osg::GLExtensions* extensions)
        {
            return extensions != nullptr && (extensions->isTimerQuerySupported || extensions->isARBTimerQuerySupported)
                && extensions->glGenQueries != nullptr && extensions->glQueryCounter != nullptr
                && extensions->glDeleteQueries != nullptr && extensions->glGetQueryObjectiv != nullptr
                && extensions->glGetQueryObjectui64v != nullptr;
        }

        /// Owns asynchronous timestamp queries for one instrumented camera.
        class GpuTimestampState final : public osg::Referenced
        {
        public:
            explicit GpuTimestampState(osg::Camera& camera)
                : mCamera(&camera)
            {
            }

            /// Issue the timestamp that begins this camera's GPU interval.
            void begin(osg::RenderInfo& renderInfo)
            {
                osg::State* state = renderInfo.getState();
                if (state == nullptr || state->getFrameStamp() == nullptr)
                    return;
                osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                if (!supportsTimestamps(extensions))
                    return;

                std::lock_guard lock(mMutex);
                ContextQueries& queries = mContexts[state->getContextID()];
                retireCompleted(*extensions, queries);
                if (queries.mActive.has_value())
                    return;

                TimestampPair pair;
                pair.mFrame = state->getFrameStamp()->getFrameNumber();
                pair.mCpuStart = std::chrono::steady_clock::now();
                GLuint ids[2]{};
                extensions->glGenQueries(2, ids);
                pair.mStart = ids[0];
                pair.mEnd = ids[1];
                extensions->glQueryCounter(pair.mStart, GL_TIMESTAMP);
                queries.mActive = pair;
            }

            /// Issue the ending timestamp and queue the interval for non-blocking readback.
            void end(osg::RenderInfo& renderInfo)
            {
                osg::State* state = renderInfo.getState();
                if (state == nullptr)
                    return;
                osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                if (!supportsTimestamps(extensions))
                    return;

                std::lock_guard lock(mMutex);
                auto context = mContexts.find(state->getContextID());
                if (context == mContexts.end() || !context->second.mActive.has_value())
                    return;

                extensions->glQueryCounter(context->second.mActive->mEnd, GL_TIMESTAMP);
                osg::ref_ptr<osg::Camera> camera;
                if (mCamera.lock(camera) && camera->getStats() != nullptr)
                {
                    const double cpuSeconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - context->second.mActive->mCpuStart)
                                                  .count();
                    camera->getStats()->setAttribute(
                        context->second.mActive->mFrame, std::string(diagnosticCpuPassTimeStatistic), cpuSeconds);
                }
                context->second.mPending.push_back(*context->second.mActive);
                context->second.mActive.reset();
                retireCompleted(*extensions, context->second);
                trimPending(*extensions, context->second);
            }

            /// Release timestamp queries belonging to the supplied graphics context.
            void release(osg::State* state)
            {
                std::lock_guard lock(mMutex);
                if (state == nullptr)
                {
                    mContexts.clear();
                    return;
                }

                auto context = mContexts.find(state->getContextID());
                if (context == mContexts.end())
                    return;
                osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                if (extensions != nullptr && extensions->glDeleteQueries != nullptr)
                    deleteQueries(*extensions, context->second);
                mContexts.erase(context);
            }

        private:
            struct TimestampPair
            {
                GLuint mStart = 0;
                GLuint mEnd = 0;
                unsigned mFrame = 0;
                std::chrono::steady_clock::time_point mCpuStart;
            };

            struct ContextQueries
            {
                std::optional<TimestampPair> mActive;
                std::deque<TimestampPair> mPending;
            };

            /// Publish every completed interval without waiting for unfinished GPU work.
            void retireCompleted(osg::GLExtensions& extensions, ContextQueries& queries)
            {
                osg::ref_ptr<osg::Camera> camera;
                if (!mCamera.lock(camera) || camera->getStats() == nullptr)
                    return;

                while (!queries.mPending.empty())
                {
                    const TimestampPair& pair = queries.mPending.front();
                    GLint available = GL_FALSE;
                    extensions.glGetQueryObjectiv(pair.mEnd, GL_QUERY_RESULT_AVAILABLE, &available);
                    if (available != GL_TRUE)
                        break;

                    GLuint64 start = 0;
                    GLuint64 end = 0;
                    extensions.glGetQueryObjectui64v(pair.mStart, GL_QUERY_RESULT, &start);
                    extensions.glGetQueryObjectui64v(pair.mEnd, GL_QUERY_RESULT, &end);
                    if (end >= start)
                    {
                        constexpr double nanosecondsToSeconds = 1e-9;
                        camera->getStats()->setAttribute(pair.mFrame, std::string(diagnosticGpuPassTimeStatistic),
                            static_cast<double>(end - start) * nanosecondsToSeconds);
                    }
                    const GLuint ids[]{ pair.mStart, pair.mEnd };
                    extensions.glDeleteQueries(2, ids);
                    queries.mPending.pop_front();
                }
            }

            /// Bound unresolved query storage if a driver stops returning timestamp results.
            static void trimPending(osg::GLExtensions& extensions, ContextQueries& queries)
            {
                while (queries.mPending.size() > retainedFrameCount)
                {
                    const TimestampPair& pair = queries.mPending.front();
                    const GLuint ids[]{ pair.mStart, pair.mEnd };
                    extensions.glDeleteQueries(2, ids);
                    queries.mPending.pop_front();
                }
            }

            /// Delete every query retained for one graphics context.
            static void deleteQueries(osg::GLExtensions& extensions, ContextQueries& queries)
            {
                if (queries.mActive.has_value())
                {
                    const GLuint ids[]{ queries.mActive->mStart, queries.mActive->mEnd };
                    extensions.glDeleteQueries(2, ids);
                }
                for (const TimestampPair& pair : queries.mPending)
                {
                    const GLuint ids[]{ pair.mStart, pair.mEnd };
                    extensions.glDeleteQueries(2, ids);
                }
            }

            osg::observer_ptr<osg::Camera> mCamera;
            std::mutex mMutex;
            std::map<unsigned, ContextQueries> mContexts;
        };

        /// Measures one render-bin interval and delegates unchanged rendering to the original callback.
        class RenderBinGpuTimestampCallback final : public osgUtil::RenderBin::DrawCallback
        {
        public:
            RenderBinGpuTimestampCallback(osgUtil::RenderBin::DrawCallback* delegate, int binNumber,
                std::string_view gpuStatistic, std::string_view cpuStatistic)
                : mDelegate(delegate)
                , mBinNumber(binNumber)
                , mGpuStatistic(gpuStatistic)
                , mCpuStatistic(cpuStatistic)
            {
            }

            /// Time matching diagnostic draws and preserve the prototype's original behavior.
            void drawImplementation(
                osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override
            {
                osg::State* state = renderInfo.getState();
                osg::Camera* camera = renderInfo.getCurrentCamera();
                osg::GLExtensions* extensions = state != nullptr ? state->get<osg::GLExtensions>() : nullptr;
                const bool enabled = bin->getBinNum() == mBinNumber && state != nullptr
                    && state->getFrameStamp() != nullptr && camera != nullptr && camera->getStats() != nullptr
                    && camera->getStats()->collectStats(std::string(collectionCategory))
                    && supportsTimestamps(extensions);
                if (!enabled)
                {
                    drawDelegate(bin, renderInfo, previous);
                    return;
                }

                const unsigned contextId = state->getContextID();
                retireCompleted(*extensions, contextId);

                PendingInterval interval{
                    .mCamera = camera,
                    .mFrame = state->getFrameStamp()->getFrameNumber(),
                };
                GLuint ids[2]{};
                extensions->glGenQueries(2, ids);
                interval.mStart = ids[0];
                interval.mEnd = ids[1];
                extensions->glQueryCounter(interval.mStart, GL_TIMESTAMP);
                const auto cpuStart = std::chrono::steady_clock::now();
                drawDelegate(bin, renderInfo, previous);
                const double cpuSeconds
                    = std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuStart).count();
                double existingCpuSeconds = 0.0;
                camera->getStats()->getAttribute(interval.mFrame, mCpuStatistic, existingCpuSeconds);
                camera->getStats()->setAttribute(interval.mFrame, mCpuStatistic, existingCpuSeconds + cpuSeconds);
                extensions->glQueryCounter(interval.mEnd, GL_TIMESTAMP);

                {
                    std::lock_guard lock(mMutex);
                    std::deque<PendingInterval>& pending = mPending[contextId];
                    pending.push_back(interval);
                    while (pending.size() > retainedFrameCount)
                    {
                        deleteInterval(*extensions, pending.front());
                        pending.pop_front();
                    }
                }
                retireCompleted(*extensions, contextId);
            }

        private:
            struct PendingInterval
            {
                osg::observer_ptr<osg::Camera> mCamera;
                GLuint mStart = 0;
                GLuint mEnd = 0;
                unsigned mFrame = 0;
            };

            /// Invoke the wrapped callback or the render bin's normal implementation.
            void drawDelegate(
                osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) const
            {
                if (mDelegate != nullptr)
                    mDelegate->drawImplementation(bin, renderInfo, previous);
                else
                    bin->drawImplementation(renderInfo, previous);
            }

            /// Publish completed intervals without synchronizing with unfinished GPU work.
            void retireCompleted(osg::GLExtensions& extensions, unsigned contextId)
            {
                std::lock_guard lock(mMutex);
                std::deque<PendingInterval>& pending = mPending[contextId];
                while (!pending.empty())
                {
                    const PendingInterval& interval = pending.front();
                    GLint available = GL_FALSE;
                    extensions.glGetQueryObjectiv(interval.mEnd, GL_QUERY_RESULT_AVAILABLE, &available);
                    if (available != GL_TRUE)
                        break;

                    osg::ref_ptr<osg::Camera> camera;
                    GLuint64 start = 0;
                    GLuint64 end = 0;
                    extensions.glGetQueryObjectui64v(interval.mStart, GL_QUERY_RESULT, &start);
                    extensions.glGetQueryObjectui64v(interval.mEnd, GL_QUERY_RESULT, &end);
                    if (interval.mCamera.lock(camera) && camera->getStats() != nullptr && end >= start)
                    {
                        constexpr double nanosecondsToSeconds = 1e-9;
                        double duration = static_cast<double>(end - start) * nanosecondsToSeconds;
                        double existing = 0.0;
                        camera->getStats()->getAttribute(interval.mFrame, mGpuStatistic, existing);
                        camera->getStats()->setAttribute(interval.mFrame, mGpuStatistic, existing + duration);
                    }
                    deleteInterval(extensions, interval);
                    pending.pop_front();
                }
            }

            /// Delete the query pair associated with one completed or abandoned interval.
            static void deleteInterval(osg::GLExtensions& extensions, const PendingInterval& interval)
            {
                const GLuint ids[]{ interval.mStart, interval.mEnd };
                extensions.glDeleteQueries(2, ids);
            }

            osg::ref_ptr<osgUtil::RenderBin::DrawCallback> mDelegate;
            int mBinNumber;
            std::string mGpuStatistic;
            std::string mCpuStatistic;
            std::mutex mMutex;
            std::map<unsigned, std::deque<PendingInterval>> mPending;
        };

        /// Starts one explicit camera GPU interval while preserving existing callbacks.
        class BeginGpuTimestampCallback final : public osg::Camera::DrawCallback
        {
        public:
            explicit BeginGpuTimestampCallback(GpuTimestampState& state)
                : mState(&state)
            {
            }

            /// Begin this camera's asynchronous timestamp interval.
            void operator()(osg::RenderInfo& renderInfo) const override { mState->begin(renderInfo); }

            /// Release query objects for a destroyed graphics context.
            void releaseGLObjects(osg::State* state) const override
            {
                mState->release(state);
                osg::Camera::DrawCallback::releaseGLObjects(state);
            }

        private:
            osg::ref_ptr<GpuTimestampState> mState;
        };

        /// Ends one explicit camera GPU interval while preserving existing callbacks.
        class EndGpuTimestampCallback final : public osg::Camera::DrawCallback
        {
        public:
            explicit EndGpuTimestampCallback(GpuTimestampState& state)
                : mState(&state)
            {
            }

            /// End this camera's asynchronous timestamp interval.
            void operator()(osg::RenderInfo& renderInfo) const override { mState->end(renderInfo); }

            /// Release query objects for a destroyed graphics context.
            void releaseGLObjects(osg::State* state) const override
            {
                mState->release(state);
                osg::Camera::DrawCallback::releaseGLObjects(state);
            }

        private:
            osg::ref_ptr<GpuTimestampState> mState;
        };

        /// Return whether a named camera owns a stable diagnostic GPU pass.
        bool isDiagnosticPassCamera(const osg::Camera& camera)
        {
            return std::ranges::any_of(
                passes, [&camera](const DiagnosticGpuPass& pass) { return camera.getName() == pass.mCameraName; });
        }

        /// Attach explicit GPU timestamp callbacks to a named render camera.
        void attachGpuTimestampCallbacks(osg::Camera& camera)
        {
            osg::ref_ptr<GpuTimestampState> state = new GpuTimestampState(camera);
            camera.addInitialDrawCallback(new BeginGpuTimestampCallback(*state));
            camera.addFinalDrawCallback(new EndGpuTimestampCallback(*state));
        }

        /// Enables GPU-pass statistics for cameras discovered below one scene owner.
        class EnableStatisticsVisitor final : public osg::NodeVisitor
        {
        public:
            EnableStatisticsVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            /// Enable statistics for this camera and continue through nested render stages.
            void apply(osg::Camera& camera) override
            {
                enableDiagnosticGpuPassStatistics(camera);
                traverse(camera);
            }
        };
    }

    std::span<const DiagnosticGpuPass> diagnosticGpuPasses()
    {
        return passes;
    }

    void enableDiagnosticGpuPassStatistics(osg::Camera& camera)
    {
        osg::Stats* stats = camera.getStats();
        if (stats == nullptr)
        {
            camera.setStats(new osg::Stats(camera.getName()));
            stats = camera.getStats();
        }
        const bool firstEnable = !stats->collectStats(std::string(collectionCategory));
        stats->allocate(retainedFrameCount);
        stats->collectStats(std::string(collectionCategory), true);
        if (firstEnable && isDiagnosticPassCamera(camera))
            attachGpuTimestampCallbacks(camera);

        std::lock_guard lock(registryMutex);
        if (std::ranges::none_of(registeredCameras,
                [&camera](const osg::observer_ptr<osg::Camera>& registered) { return registered == &camera; }))
            registeredCameras.emplace_back(&camera);
    }

    void enableDiagnosticGpuPassStatistics(osg::Node& root)
    {
        EnableStatisticsVisitor visitor;
        root.accept(visitor);
    }

    void enableDiagnosticGpuRenderBinStatistics(
        osgUtil::RenderBin& prototype, int binNumber, std::string_view gpuStatistic, std::string_view cpuStatistic)
    {
        prototype.setDrawCallback(
            new RenderBinGpuTimestampCallback(prototype.getDrawCallback(), binNumber, gpuStatistic, cpuStatistic));
    }

    void inheritDiagnosticGpuPassStatistics(osg::Camera& target, const osg::Camera& source)
    {
        const osg::Stats* sourceStats = source.getStats();
        if (sourceStats == nullptr || !sourceStats->collectStats(std::string(collectionCategory)))
            return;

        osg::Stats* targetStats = target.getStats();
        if (targetStats == nullptr || !targetStats->collectStats(std::string(collectionCategory)))
            enableDiagnosticGpuPassStatistics(target);
    }

    std::vector<osg::ref_ptr<osg::Camera>> diagnosticGpuPassCameras()
    {
        std::lock_guard lock(registryMutex);
        std::vector<osg::ref_ptr<osg::Camera>> result;
        for (auto camera = registeredCameras.begin(); camera != registeredCameras.end();)
        {
            osg::ref_ptr<osg::Camera> retained;
            if (camera->lock(retained))
            {
                result.push_back(std::move(retained));
                ++camera;
            }
            else
                camera = registeredCameras.erase(camera);
        }
        return result;
    }
}
