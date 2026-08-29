#ifndef OPENMW_COMPONENTS_SCENEUTIL_OFFSCREENCONTEXT_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_OFFSCREENCONTEXT_HPP

#include <osg/GraphicsContext>
#include <osg/ref_ptr>

namespace SceneUtil
{
    /// Describes the framebuffer traits required by a windowless production OpenGL context.
    struct OffscreenContextSettings
    {
        int mWidth = 0;
        int mHeight = 0;
        unsigned mSamples = 0;
    };

    /// Create a pbuffer graphics context without creating a visible native desktop window.
    osg::ref_ptr<osg::GraphicsContext> createOffscreenContext(const OffscreenContextSettings& settings);
}

#endif
