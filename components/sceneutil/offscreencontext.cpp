#include "offscreencontext.hpp"

#include <osg/GL>

#include <stdexcept>

namespace SceneUtil
{
    namespace
    {
        /// Submit a windowless frame when the platform's single-buffered swap does no work.
        class OffscreenFrameSubmission final : public osg::GraphicsContext::SwapCallback
        {
        public:
            /// Preserve native swap bookkeeping and submit commands without waiting for GPU completion.
            void swapBuffersImplementation(osg::GraphicsContext* context) override
            {
                context->swapBuffersImplementation();
                glFlush();
            }
        };
    }

    osg::ref_ptr<osg::GraphicsContext> createOffscreenContext(const OffscreenContextSettings& settings)
    {
        if (settings.mWidth <= 0 || settings.mHeight <= 0)
            throw std::invalid_argument("offscreen graphics context dimensions must be positive");

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->x = 0;
        traits->y = 0;
        traits->width = settings.mWidth;
        traits->height = settings.mHeight;
        traits->windowName = "OpenMWSE diagnostic offscreen context";
        traits->windowDecoration = false;
        traits->useCursor = false;
        traits->doubleBuffer = false;
        traits->pbuffer = true;
        traits->vsync = false;
        traits->red = 8;
        traits->green = 8;
        traits->blue = 8;
        traits->alpha = 0;
        traits->depth = 24;
        traits->sampleBuffers = settings.mSamples > 0 ? 1 : 0;
        traits->samples = settings.mSamples;
        traits->glContextProfileMask = 0;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (context)
            context->setSwapCallback(new OffscreenFrameSubmission);
        return context;
    }
}
