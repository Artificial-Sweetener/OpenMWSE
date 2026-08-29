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

        /// Read a packed depth-stencil texture into normalized depth floats when available.
        std::optional<RenderDiagnosticImage> captureDepthTexture(osg::Texture* texture, std::string name);

        /// Read the named color attachment from a framebuffer into RGBA floats.
        RenderDiagnosticImage captureFramebuffer(unsigned int framebuffer, int width, int height, std::string name);

    private:
        osg::State& mState;
        osg::GLExtensions& mExtensions;
        GLint mPreviousReadFramebuffer = 0;
        GLint mPreviousReadBuffer = GL_BACK;
    };
}

#endif
