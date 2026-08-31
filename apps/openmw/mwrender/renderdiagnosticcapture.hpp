#ifndef OPENMW_MWRENDER_RENDERDIAGNOSTICCAPTURE_H
#define OPENMW_MWRENDER_RENDERDIAGNOSTICCAPTURE_H

#include "renderdiagnosticimage.hpp"

#include <optional>
#include <string>

#include <osg/GL>

namespace osg
{
    class GLExtensions;
    class RenderInfo;
    class State;
    class Texture;
}

namespace MWRender
{
    /// Reads production render attachments while preserving the caller's framebuffer state.
    class RenderDiagnosticCapture
    {
    public:
        /// Record the current read framebuffer and buffer for restoration after capture.
        explicit RenderDiagnosticCapture(osg::RenderInfo& renderInfo);

        /// Restore the read framebuffer and buffer owned by the caller.
        ~RenderDiagnosticCapture();

        RenderDiagnosticCapture(const RenderDiagnosticCapture&) = delete;
        RenderDiagnosticCapture& operator=(const RenderDiagnosticCapture&) = delete;

        /// Read a two-dimensional color texture into scene-linear RGBA floats when available.
        std::optional<RenderDiagnosticImage> captureColorTexture(osg::Texture* texture, std::string name);

        /// Read a depth texture into normalized floats, optionally reducing its longest edge before readback.
        std::optional<RenderDiagnosticImage> captureDepthTexture(
            osg::Texture* texture, std::string name, unsigned maximumDimension = 0);

        /// Read the named color attachment from a framebuffer into RGBA floats, optionally reducing its longest edge.
        RenderDiagnosticImage captureFramebuffer(unsigned int framebuffer, int width, int height, std::string name,
            unsigned maximumDimension = 0);

    private:
        /// Restore the framebuffer bindings and read target that were active before capture began.
        void restoreFramebufferState();

        osg::State& mState;
        osg::GLExtensions& mExtensions;
        GLint mPreviousReadFramebuffer = 0;
        GLint mPreviousDrawFramebuffer = 0;
        GLint mPreviousReadBuffer = GL_BACK;
    };
}

#endif
