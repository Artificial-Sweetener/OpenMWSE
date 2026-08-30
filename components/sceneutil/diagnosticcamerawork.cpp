#include "diagnosticcamerawork.hpp"

#include "diagnosticthreadwork.hpp"

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>
#include <osg/observer_ptr>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace SceneUtil
{
    struct DiagnosticCameraWork::Impl
    {
        struct Sample
        {
            unsigned mFrame = 0;
            std::chrono::steady_clock::time_point mStart;
            std::optional<std::uint64_t> mCycleStart;
            std::optional<std::chrono::steady_clock::time_point> mDrawStart;
            std::optional<std::uint64_t> mDrawCycleStart;
            std::optional<std::chrono::steady_clock::time_point> mDrawEnd;
            std::optional<std::uint64_t> mDrawCycleEnd;
        };

        explicit Impl(osg::Camera& camera)
            : mCamera(&camera)
        {
        }

        osg::observer_ptr<osg::Camera> mCamera;
        std::mutex mMutex;
        std::map<unsigned, Sample> mSamples;
    };

    DiagnosticCameraWork::DiagnosticCameraWork(osg::Camera& camera)
        : mImpl(std::make_unique<Impl>(camera))
    {
    }

    DiagnosticCameraWork::~DiagnosticCameraWork() = default;

    void DiagnosticCameraWork::begin(osg::RenderInfo& renderInfo)
    {
        osg::State* state = renderInfo.getState();
        if (state == nullptr || state->getFrameStamp() == nullptr)
            return;
        Impl::Sample sample;
        sample.mFrame = state->getFrameStamp()->getFrameNumber();
        sample.mStart = std::chrono::steady_clock::now();
        sample.mCycleStart = diagnosticCurrentThreadCycleCount();
        std::lock_guard lock(mImpl->mMutex);
        mImpl->mSamples[state->getContextID()] = sample;
    }

    void DiagnosticCameraWork::beforeDraw(osg::RenderInfo& renderInfo)
    {
        osg::State* state = renderInfo.getState();
        if (state == nullptr)
            return;
        const auto now = std::chrono::steady_clock::now();
        const std::optional<std::uint64_t> cycles = diagnosticCurrentThreadCycleCount();
        std::lock_guard lock(mImpl->mMutex);
        auto sample = mImpl->mSamples.find(state->getContextID());
        osg::ref_ptr<osg::Camera> camera;
        if (sample == mImpl->mSamples.end() || !mImpl->mCamera.lock(camera) || camera->getStats() == nullptr)
            return;
        camera->getStats()->setAttribute(sample->second.mFrame, std::string(diagnosticCpuPassSetupTimeStatistic),
            std::chrono::duration<double>(now - sample->second.mStart).count());
        if (sample->second.mCycleStart && cycles && *cycles >= *sample->second.mCycleStart)
        {
            camera->getStats()->setAttribute(sample->second.mFrame,
                std::string(diagnosticCpuPassSetupThreadCycleStatistic),
                static_cast<double>(*cycles - *sample->second.mCycleStart));
        }
        sample->second.mDrawStart = now;
        sample->second.mDrawCycleStart = cycles;
    }

    void DiagnosticCameraWork::afterDraw(osg::RenderInfo& renderInfo)
    {
        osg::State* state = renderInfo.getState();
        if (state == nullptr)
            return;
        const auto now = std::chrono::steady_clock::now();
        const std::optional<std::uint64_t> cycles = diagnosticCurrentThreadCycleCount();
        std::lock_guard lock(mImpl->mMutex);
        auto sample = mImpl->mSamples.find(state->getContextID());
        osg::ref_ptr<osg::Camera> camera;
        if (sample == mImpl->mSamples.end() || !sample->second.mDrawStart || !mImpl->mCamera.lock(camera)
            || camera->getStats() == nullptr)
            return;
        camera->getStats()->setAttribute(sample->second.mFrame, std::string(diagnosticCpuPassDrawTimeStatistic),
            std::chrono::duration<double>(now - *sample->second.mDrawStart).count());
        if (sample->second.mDrawCycleStart && cycles && *cycles >= *sample->second.mDrawCycleStart)
        {
            camera->getStats()->setAttribute(sample->second.mFrame,
                std::string(diagnosticCpuPassDrawThreadCycleStatistic),
                static_cast<double>(*cycles - *sample->second.mDrawCycleStart));
        }
        sample->second.mDrawEnd = now;
        sample->second.mDrawCycleEnd = cycles;
    }

    void DiagnosticCameraWork::end(osg::RenderInfo& renderInfo)
    {
        osg::State* state = renderInfo.getState();
        if (state == nullptr)
            return;
        const auto now = std::chrono::steady_clock::now();
        const std::optional<std::uint64_t> cycles = diagnosticCurrentThreadCycleCount();
        std::lock_guard lock(mImpl->mMutex);
        auto sample = mImpl->mSamples.find(state->getContextID());
        if (sample == mImpl->mSamples.end())
            return;
        osg::ref_ptr<osg::Camera> camera;
        if (mImpl->mCamera.lock(camera) && camera->getStats() != nullptr)
        {
            if (sample->second.mCycleStart && cycles && *cycles >= *sample->second.mCycleStart)
            {
                camera->getStats()->setAttribute(sample->second.mFrame,
                    std::string(diagnosticCpuPassThreadCycleStatistic),
                    static_cast<double>(*cycles - *sample->second.mCycleStart));
            }
            if (sample->second.mDrawEnd)
            {
                camera->getStats()->setAttribute(sample->second.mFrame,
                    std::string(diagnosticCpuPassCompletionTimeStatistic),
                    std::chrono::duration<double>(now - *sample->second.mDrawEnd).count());
                if (sample->second.mDrawCycleEnd && cycles && *cycles >= *sample->second.mDrawCycleEnd)
                {
                    camera->getStats()->setAttribute(sample->second.mFrame,
                        std::string(diagnosticCpuPassCompletionThreadCycleStatistic),
                        static_cast<double>(*cycles - *sample->second.mDrawCycleEnd));
                }
            }
        }
        mImpl->mSamples.erase(sample);
    }

    void DiagnosticCameraWork::release(osg::State* state)
    {
        std::lock_guard lock(mImpl->mMutex);
        if (state == nullptr)
            mImpl->mSamples.clear();
        else
            mImpl->mSamples.erase(state->getContextID());
    }
}
