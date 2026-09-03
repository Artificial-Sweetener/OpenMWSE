#include "diagnosticgputiming.hpp"

#include "diagnosticcamerawork.hpp"

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/GLExtensions>
#include <osg/NodeVisitor>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>
#include <osg/Timer>
#include <osg/observer_ptr>
#include <osgUtil/RenderBin>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
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
        constexpr std::string_view passInstanceKey = "OpenMW diagnostic GPU pass instance";
        constexpr std::array passes{
            DiagnosticGpuPass{ "main-scene", sceneCameraName },
            DiagnosticGpuPass{ "shadow-maps", shadowCameraName },
            DiagnosticGpuPass{ "water-reflection", reflectionCameraName },
            DiagnosticGpuPass{ "sky-render-target", skyCameraName },
            DiagnosticGpuPass{ "post-processing", postProcessingCameraName },
            DiagnosticGpuPass{ "precipitation-occlusion", precipitationOcclusionCameraName },
            DiagnosticGpuPass{ "ripples", ripplesCameraName },
            DiagnosticGpuPass{ "terrain-composite", terrainCompositeCameraName },
        };

        std::mutex registryMutex;
        std::vector<osg::observer_ptr<osg::Camera>> registeredCameras;
        bool collectionEnabled = false;

        /// Remove destroyed cameras before comparing addresses that may have been reused by a new camera.
        void removeExpiredCameras()
        {
            std::erase_if(
                registeredCameras, [](const osg::observer_ptr<osg::Camera>& camera) { return !camera.valid(); });
        }

        /// Return whether the current context exposes timestamp-query entry points.
        bool supportsTimestamps(const osg::GLExtensions* extensions)
        {
            return extensions != nullptr && (extensions->isTimerQuerySupported || extensions->isARBTimerQuerySupported)
                && extensions->glGenQueries != nullptr && extensions->glQueryCounter != nullptr
                && extensions->glDeleteQueries != nullptr && extensions->glGetQueryObjectiv != nullptr
                && extensions->glGetQueryObjectui64v != nullptr;
        }

        /// Return whether a delayed GPU query may be polled without targeting recent in-flight work.
        bool queryReadbackReady(unsigned sampleFrame, unsigned currentFrame) noexcept
        {
            return currentFrame >= sampleFrame
                && currentFrame - sampleFrame >= diagnosticGpuTimingReadbackLatencyFrames;
        }

        /// Owns asynchronous timestamp queries for one instrumented camera.
        class GpuTimestampState final : public osg::Referenced
        {
        public:
            explicit GpuTimestampState(osg::Camera& camera)
                : mCamera(&camera)
                , mCameraWork(camera)
            {
            }

            /// Issue the timestamp that begins this camera's GPU interval.
            void begin(osg::RenderInfo& renderInfo)
            {
                osg::State* state = renderInfo.getState();
                if (state == nullptr || state->getFrameStamp() == nullptr)
                    return;
                osg::ref_ptr<osg::Camera> camera;
                if (!mCamera.lock(camera) || camera->getStats() == nullptr
                    || !camera->getStats()->collectStats(std::string(collectionCategory)))
                    return;

                const auto callbackStart = std::chrono::steady_clock::now();
                const unsigned currentFrame = state->getFrameStamp()->getFrameNumber();
                std::lock_guard lock(mMutex);
                ContextQueries& queries = mContexts[state->getContextID()];
                osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                if (supportsTimestamps(extensions))
                    retireCompleted(*extensions, queries, currentFrame);
                if (queries.mActive.has_value())
                    return;

                TimestampPair pair;
                pair.mFrame = currentFrame;
                camera->getStats()->setAttribute(pair.mFrame, std::string(diagnosticGpuQueryStateStatistic),
                    static_cast<int>(supportsTimestamps(extensions) ? DiagnosticGpuQueryState::Pending
                                                                    : DiagnosticGpuQueryState::Unsupported));
                if (supportsTimestamps(extensions))
                {
                    const std::array<GLuint, 2> ids = acquireQueries(*extensions, queries);
                    pair.mStart = ids[0];
                    pair.mEnd = ids[1];
                    extensions->glQueryCounter(pair.mStart, GL_TIMESTAMP);
                }
                pair.mCpuStart = std::chrono::steady_clock::now();
                pair.mCpuBegin = osg::Timer::instance()->delta_s(state->getStartTick(), osg::Timer::instance()->tick());
                camera->getStats()->setAttribute(
                    pair.mFrame, std::string(diagnosticCpuPassBeginStatistic), pair.mCpuBegin);
                camera->getStats()->setAttribute(pair.mFrame, std::string(diagnosticCpuPassBeginOverheadStatistic),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - callbackStart).count());
                queries.mActive = pair;
                mCameraWork.begin(renderInfo);
            }

            /// Issue the ending timestamp and queue the interval for non-blocking readback.
            void end(osg::RenderInfo& renderInfo)
            {
                osg::State* state = renderInfo.getState();
                if (state == nullptr)
                    return;

                std::lock_guard lock(mMutex);
                auto context = mContexts.find(state->getContextID());
                if (context == mContexts.end() || !context->second.mActive.has_value())
                    return;

                TimestampPair& active = *context->second.mActive;
                const unsigned frame = active.mFrame;
                osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                const bool gpuTimingActive = active.mStart != 0 && active.mEnd != 0 && supportsTimestamps(extensions);
                osg::ref_ptr<osg::Camera> camera;
                if (mCamera.lock(camera) && camera->getStats() != nullptr)
                {
                    const double cpuSeconds
                        = std::chrono::duration<double>(std::chrono::steady_clock::now() - active.mCpuStart).count();
                    camera->getStats()->setAttribute(
                        active.mFrame, std::string(diagnosticCpuPassTimeStatistic), cpuSeconds);
                    camera->getStats()->setAttribute(active.mFrame, std::string(diagnosticCpuPassEndStatistic),
                        osg::Timer::instance()->delta_s(state->getStartTick(), osg::Timer::instance()->tick()));
                }
                mCameraWork.end(renderInfo);
                const auto trailingOverheadStart = std::chrono::steady_clock::now();
                if (gpuTimingActive)
                    extensions->glQueryCounter(active.mEnd, GL_TIMESTAMP);
                if (gpuTimingActive)
                    context->second.mPending.push_back(active);
                context->second.mActive.reset();
                if (supportsTimestamps(extensions))
                {
                    retireCompleted(*extensions, context->second, frame);
                    trimPending(*extensions, context->second);
                }
                if (camera != nullptr && camera->getStats() != nullptr)
                {
                    camera->getStats()->setAttribute(frame, std::string(diagnosticCpuPassEndOverheadStatistic),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - trailingOverheadStart)
                            .count());
                }
            }

            /// Mark completion of camera setup immediately before its draw body.
            void beforeDraw(osg::RenderInfo& renderInfo) { mCameraWork.beforeDraw(renderInfo); }

            /// Mark completion of the camera draw body before post-render work.
            void afterDraw(osg::RenderInfo& renderInfo) { mCameraWork.afterDraw(renderInfo); }

            /// Release timestamp queries belonging to the supplied graphics context.
            void release(osg::State* state)
            {
                mCameraWork.release(state);
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
                double mCpuBegin = 0.0;
            };

            struct ContextQueries
            {
                std::optional<TimestampPair> mActive;
                std::deque<TimestampPair> mPending;
                std::vector<std::array<GLuint, 2>> mReusable;
            };

            /// Acquire one query pair without repeatedly allocating driver objects after warmup.
            static std::array<GLuint, 2> acquireQueries(osg::GLExtensions& extensions, ContextQueries& queries)
            {
                if (!queries.mReusable.empty())
                {
                    const std::array<GLuint, 2> result = queries.mReusable.back();
                    queries.mReusable.pop_back();
                    return result;
                }
                std::array<GLuint, 2> result{};
                extensions.glGenQueries(static_cast<GLsizei>(result.size()), result.data());
                return result;
            }

            /// Publish every completed interval without waiting for unfinished GPU work.
            void retireCompleted(osg::GLExtensions& extensions, ContextQueries& queries, unsigned currentFrame)
            {
                osg::ref_ptr<osg::Camera> camera;
                if (!mCamera.lock(camera) || camera->getStats() == nullptr)
                    return;

                while (!queries.mPending.empty())
                {
                    const TimestampPair& pair = queries.mPending.front();
                    if (!queryReadbackReady(pair.mFrame, currentFrame))
                        break;
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
                        camera->getStats()->setAttribute(pair.mFrame,
                            std::string(diagnosticGpuPassBeginTimestampStatistic),
                            static_cast<double>(start) * nanosecondsToSeconds);
                        camera->getStats()->setAttribute(pair.mFrame,
                            std::string(diagnosticGpuPassEndTimestampStatistic),
                            static_cast<double>(end) * nanosecondsToSeconds);
                    }
                    // Publish completion last so the report owner cannot observe a complete but unwritten interval.
                    camera->getStats()->setAttribute(pair.mFrame, std::string(diagnosticGpuQueryStateStatistic),
                        static_cast<int>(
                            end >= start ? DiagnosticGpuQueryState::Complete : DiagnosticGpuQueryState::Invalid));
                    queries.mReusable.push_back({ pair.mStart, pair.mEnd });
                    queries.mPending.pop_front();
                }
            }

            /// Bound unresolved query storage if a driver stops returning timestamp results.
            void trimPending(osg::GLExtensions& extensions, ContextQueries& queries)
            {
                while (queries.mPending.size() > diagnosticGpuTimingRetainedFrames)
                {
                    const TimestampPair& pair = queries.mPending.front();
                    osg::ref_ptr<osg::Camera> camera;
                    if (mCamera.lock(camera) && camera->getStats() != nullptr)
                        camera->getStats()->setAttribute(pair.mFrame, std::string(diagnosticGpuQueryStateStatistic),
                            static_cast<int>(DiagnosticGpuQueryState::Dropped));
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
                    if (queries.mActive->mStart != 0 && queries.mActive->mEnd != 0)
                    {
                        const GLuint ids[]{ queries.mActive->mStart, queries.mActive->mEnd };
                        extensions.glDeleteQueries(2, ids);
                    }
                }
                for (const TimestampPair& pair : queries.mPending)
                {
                    const GLuint ids[]{ pair.mStart, pair.mEnd };
                    extensions.glDeleteQueries(2, ids);
                }
                for (const std::array<GLuint, 2>& ids : queries.mReusable)
                    extensions.glDeleteQueries(static_cast<GLsizei>(ids.size()), ids.data());
            }

            osg::observer_ptr<osg::Camera> mCamera;
            std::mutex mMutex;
            DiagnosticCameraWork mCameraWork;
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
                    && camera->getStats()->collectStats(std::string(collectionCategory));
                if (!enabled)
                {
                    drawDelegate(bin, renderInfo, previous);
                    return;
                }

                const unsigned contextId = state->getContextID();
                const unsigned currentFrame = state->getFrameStamp()->getFrameNumber();
                const bool gpuTimingEnabled = supportsTimestamps(extensions);
                if (gpuTimingEnabled)
                    retireCompleted(*extensions, contextId, currentFrame);

                PendingInterval interval{
                    .mCamera = camera,
                    .mFrame = currentFrame,
                };
                if (gpuTimingEnabled)
                {
                    GLuint ids[2]{};
                    extensions->glGenQueries(2, ids);
                    interval.mStart = ids[0];
                    interval.mEnd = ids[1];
                    extensions->glQueryCounter(interval.mStart, GL_TIMESTAMP);
                }
                const auto cpuStart = std::chrono::steady_clock::now();
                drawDelegate(bin, renderInfo, previous);
                const double cpuSeconds
                    = std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuStart).count();
                double existingCpuSeconds = 0.0;
                camera->getStats()->getAttribute(interval.mFrame, mCpuStatistic, existingCpuSeconds);
                camera->getStats()->setAttribute(interval.mFrame, mCpuStatistic, existingCpuSeconds + cpuSeconds);
                if (!gpuTimingEnabled)
                    return;
                extensions->glQueryCounter(interval.mEnd, GL_TIMESTAMP);

                {
                    std::lock_guard lock(mMutex);
                    std::deque<PendingInterval>& pending = mPending[contextId];
                    pending.push_back(interval);
                    while (pending.size() > diagnosticGpuTimingRetainedFrames)
                    {
                        deleteInterval(*extensions, pending.front());
                        pending.pop_front();
                    }
                }
                retireCompleted(*extensions, contextId, currentFrame);
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
            void retireCompleted(osg::GLExtensions& extensions, unsigned contextId, unsigned currentFrame)
            {
                std::lock_guard lock(mMutex);
                std::deque<PendingInterval>& pending = mPending[contextId];
                while (!pending.empty())
                {
                    const PendingInterval& interval = pending.front();
                    if (!queryReadbackReady(interval.mFrame, currentFrame))
                        break;
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

        /// Marks the boundary between camera setup and its draw body.
        class BeforeDrawTimestampCallback final : public osg::Camera::DrawCallback
        {
        public:
            explicit BeforeDrawTimestampCallback(GpuTimestampState& state)
                : mState(&state)
            {
            }

            /// Record the camera setup boundary.
            void operator()(osg::RenderInfo& renderInfo) const override { mState->beforeDraw(renderInfo); }

        private:
            osg::ref_ptr<GpuTimestampState> mState;
        };

        /// Marks the boundary between the camera draw body and completion work.
        class AfterDrawTimestampCallback final : public osg::Camera::DrawCallback
        {
        public:
            explicit AfterDrawTimestampCallback(GpuTimestampState& state)
                : mState(&state)
            {
            }

            /// Record the camera draw-body boundary.
            void operator()(osg::RenderInfo& renderInfo) const override { mState->afterDraw(renderInfo); }

        private:
            osg::ref_ptr<GpuTimestampState> mState;
        };

        /// Attach explicit CPU and GPU timestamp callbacks to one render camera.
        void attachGpuTimestampCallbacks(osg::Camera& camera)
        {
            osg::ref_ptr<GpuTimestampState> state = new GpuTimestampState(camera);
            camera.addInitialDrawCallback(new BeginGpuTimestampCallback(*state));
            camera.addPreDrawCallback(new BeforeDrawTimestampCallback(*state));
            camera.addPostDrawCallback(new AfterDrawTimestampCallback(*state));
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
        stats->allocate(diagnosticGpuTimingRetainedFrames);
        std::lock_guard lock(registryMutex);
        removeExpiredCameras();
        const bool firstRegistration = std::ranges::none_of(registeredCameras,
            [&camera](const osg::observer_ptr<osg::Camera>& registered) { return registered == &camera; });
        if (firstRegistration)
        {
            registeredCameras.emplace_back(&camera);
            attachGpuTimestampCallbacks(camera);
        }
        stats->collectStats(std::string(collectionCategory), collectionEnabled);
    }

    void enableDiagnosticGpuPassStatistics(osg::Node& root)
    {
        EnableStatisticsVisitor visitor;
        root.accept(visitor);
    }

    void setDiagnosticGpuPassInstance(osg::Camera& camera, unsigned instance)
    {
        camera.setUserValue(std::string(passInstanceKey), instance);
    }

    std::optional<unsigned> diagnosticGpuPassInstance(const osg::Camera& camera)
    {
        unsigned instance = 0;
        if (!camera.getUserValue(std::string(passInstanceKey), instance))
            return std::nullopt;
        return instance;
    }

    bool diagnosticGpuPassStatisticsCollectionEnabled(const osg::Camera& camera)
    {
        return camera.getStats() != nullptr && camera.getStats()->collectStats(std::string(collectionCategory));
    }

    void setDiagnosticGpuPassStatisticsCollection(bool enabled)
    {
        std::lock_guard lock(registryMutex);
        collectionEnabled = enabled;
        for (auto camera = registeredCameras.begin(); camera != registeredCameras.end();)
        {
            osg::ref_ptr<osg::Camera> retained;
            if (camera->lock(retained))
            {
                if (retained->getStats() != nullptr)
                    retained->getStats()->collectStats(std::string(collectionCategory), enabled);
                ++camera;
            }
            else
                camera = registeredCameras.erase(camera);
        }
    }

    void enableDiagnosticGpuRenderBinStatistics(
        osgUtil::RenderBin& prototype, int binNumber, std::string_view gpuStatistic, std::string_view cpuStatistic)
    {
        prototype.setDrawCallback(
            new RenderBinGpuTimestampCallback(prototype.getDrawCallback(), binNumber, gpuStatistic, cpuStatistic));
    }

    void inheritDiagnosticGpuPassStatistics(osg::Camera& target, const osg::Camera& source)
    {
        std::lock_guard lock(registryMutex);
        removeExpiredCameras();
        const bool sourceRegistered = std::ranges::any_of(registeredCameras,
            [&source](const osg::observer_ptr<osg::Camera>& registered) { return registered == &source; });
        if (!sourceRegistered)
            return;
        const bool targetRegistered = std::ranges::any_of(registeredCameras,
            [&target](const osg::observer_ptr<osg::Camera>& registered) { return registered == &target; });
        if (targetRegistered)
            return;

        osg::Stats* stats = target.getStats();
        if (stats == nullptr)
        {
            target.setStats(new osg::Stats(target.getName()));
            stats = target.getStats();
        }
        stats->allocate(diagnosticGpuTimingRetainedFrames);
        stats->collectStats(std::string(collectionCategory), collectionEnabled);
        registeredCameras.emplace_back(&target);
        attachGpuTimestampCallbacks(target);
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

    bool diagnosticGpuQueriesPending(unsigned firstFrame, unsigned lastFrame)
    {
        const std::string statistic(diagnosticGpuQueryStateStatistic);
        for (const auto& camera : diagnosticGpuPassCameras())
        {
            const osg::Stats* stats = camera->getStats();
            if (stats == nullptr)
                continue;
            for (std::uint64_t frame = firstFrame; frame <= lastFrame; ++frame)
            {
                double queryState = 0;
                if (stats->getAttribute(static_cast<unsigned>(frame), statistic, queryState)
                    && queryState == static_cast<int>(DiagnosticGpuQueryState::Pending))
                    return true;
            }
        }
        return false;
    }
}
