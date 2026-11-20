#include "waterscreencoverage.hpp"

#include <array>
#include <mutex>
#include <unordered_map>

#include <osg/Camera>
#include <osg/Drawable>
#include <osg/GLExtensions>
#include <osg/OcclusionQueryNode>
#include <osg/RenderInfo>
#include <osg/State>

#include <components/misc/constants.hpp>

namespace MWRender
{
    namespace
    {
        constexpr std::size_t queryRingSize = 8;
        constexpr GLenum samplesPassedQuery = 0x8914;

        /// Return whether the current context can issue and retire sample-count queries asynchronously.
        bool supportsCoverageQueries(const osg::GLExtensions* extensions)
        {
            return extensions != nullptr && extensions->glGenQueries != nullptr
                && extensions->glDeleteQueries != nullptr && extensions->glBeginQuery != nullptr
                && extensions->glEndQuery != nullptr && extensions->glGetQueryObjectiv != nullptr
                && extensions->glGetQueryObjectuiv != nullptr;
        }

        struct CoverageQuery
        {
            GLuint mId = 0;
            unsigned int mFrame = 0;
            bool mPending = false;
        };

        struct ContextQueries
        {
            std::array<CoverageQuery, queryRingSize> mRing;
            std::size_t mNext = 0;
        };

        /// Measure the samples that actually survive the water drawable's production depth and fragment tests.
        class WaterScreenCoverageDrawCallback final : public osg::Drawable::DrawCallback
        {
        public:
            /// Preserve an existing callback while publishing results through the shared observation.
            WaterScreenCoverageDrawCallback(WaterScreenCoverage& coverage, osg::Drawable::DrawCallback* delegate)
                : mCoverage(&coverage)
                , mDelegate(delegate)
            {
            }

            /// Enclose only the main scene draw in a non-blocking sample-count query.
            void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
            {
                osg::State* state = renderInfo.getState();
                osg::Camera* camera = renderInfo.getCurrentCamera();
                if (state == nullptr || state->getFrameStamp() == nullptr || camera == nullptr
                    || camera->getName() != Constants::SceneCamera)
                {
                    drawDelegate(renderInfo, drawable);
                    return;
                }

                osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                if (!supportsCoverageQueries(extensions))
                {
                    drawDelegate(renderInfo, drawable);
                    return;
                }

                std::lock_guard lock(mMutex);
                ContextQueries& context = mContexts[state->getContextID()];
                retireCompleted(*extensions, context);
                CoverageQuery& query = context.mRing[context.mNext];
                if (query.mPending)
                {
                    drawDelegate(renderInfo, drawable);
                    return;
                }
                if (query.mId == 0)
                    extensions->glGenQueries(1, &query.mId);
                query.mFrame = state->getFrameStamp()->getFrameNumber();
                extensions->glBeginQuery(samplesPassedQuery, query.mId);
                drawDelegate(renderInfo, drawable);
                extensions->glEndQuery(samplesPassedQuery);
                query.mPending = true;
                context.mNext = (context.mNext + 1) % context.mRing.size();
            }

            /// Release context-owned query objects through the context that created them.
            void releaseGLObjects(osg::State* state) const override
            {
                std::lock_guard lock(mMutex);
                for (auto& [contextId, context] : mContexts)
                {
                    for (CoverageQuery& query : context.mRing)
                    {
                        if (query.mId == 0)
                            continue;
                        if (state != nullptr && state->getContextID() == contextId)
                        {
                            if (osg::GLExtensions* extensions = state->get<osg::GLExtensions>();
                                extensions != nullptr && extensions->glDeleteQueries != nullptr)
                                extensions->glDeleteQueries(1, &query.mId);
                        }
                        else
                            osg::QueryGeometry::deleteQueryObject(contextId, query.mId);
                        query = {};
                    }
                }
                mContexts.clear();
                osg::Drawable::DrawCallback::releaseGLObjects(state);
            }

        private:
            /// Delegate the production draw exactly once.
            void drawDelegate(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const
            {
                if (mDelegate)
                    mDelegate->drawImplementation(renderInfo, drawable);
                else
                    drawable->drawImplementation(renderInfo);
            }

            /// Poll completed slots without waiting for in-flight GPU work.
            void retireCompleted(osg::GLExtensions& extensions, ContextQueries& context) const
            {
                for (CoverageQuery& query : context.mRing)
                {
                    if (!query.mPending)
                        continue;
                    GLint available = GL_FALSE;
                    extensions.glGetQueryObjectiv(query.mId, GL_QUERY_RESULT_AVAILABLE, &available);
                    if (available == GL_FALSE)
                        continue;
                    GLuint samples = 0;
                    extensions.glGetQueryObjectuiv(query.mId, GL_QUERY_RESULT, &samples);
                    mCoverage->publish(query.mFrame, samples);
                    query.mPending = false;
                }
            }

            osg::ref_ptr<WaterScreenCoverage> mCoverage;
            osg::ref_ptr<osg::Drawable::DrawCallback> mDelegate;
            mutable std::mutex mMutex;
            mutable std::unordered_map<unsigned int, ContextQueries> mContexts;
        };
    }

    std::optional<WaterScreenCoverageResult> WaterScreenCoverage::recentResult(unsigned int currentFrame) const noexcept
    {
        std::lock_guard lock(mMutex);
        constexpr unsigned int maximumQueryAgeFrames = queryRingSize + 4;
        if (!mAvailable || currentFrame < mFrame || currentFrame - mFrame > maximumQueryAgeFrames)
            return std::nullopt;
        return WaterScreenCoverageResult{ mFrame, mSamples };
    }

    void WaterScreenCoverage::publish(unsigned int frame, std::uint64_t samples) noexcept
    {
        std::lock_guard lock(mMutex);
        if (mAvailable && frame < mFrame)
            return;
        mFrame = frame;
        mSamples = samples;
        mAvailable = true;
    }

    osg::ref_ptr<WaterScreenCoverage> attachWaterScreenCoverageQuery(
        osg::Drawable& drawable, WaterScreenCoverage* sharedCoverage)
    {
        osg::ref_ptr<WaterScreenCoverage> coverage = sharedCoverage;
        if (coverage == nullptr)
            coverage = new WaterScreenCoverage;
        drawable.setDrawCallback(new WaterScreenCoverageDrawCallback(*coverage, drawable.getDrawCallback()));
        return coverage;
    }
}
