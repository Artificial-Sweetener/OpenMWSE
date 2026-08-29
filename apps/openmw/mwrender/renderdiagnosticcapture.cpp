#include "renderdiagnosticcapture.hpp"

#include <stdexcept>
#include <utility>

#include <osg/FrameBufferObject>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Texture2D>

namespace MWRender
{
    RenderDiagnosticCapture::RenderDiagnosticCapture(osg::RenderInfo& renderInfo)
        : mState(*renderInfo.getState())
        , mExtensions(*mState.get<osg::GLExtensions>())
    {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &mPreviousReadFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &mPreviousReadBuffer);
    }

    RenderDiagnosticCapture::~RenderDiagnosticCapture()
    {
        mExtensions.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, mPreviousReadFramebuffer);
        glReadBuffer(mPreviousReadBuffer);
    }

    std::optional<RenderDiagnosticImage> RenderDiagnosticCapture::captureColorTexture(
        osg::Texture* texture, std::string name)
    {
        auto* texture2D = dynamic_cast<osg::Texture2D*>(texture);
        if (!texture2D || texture2D->getTextureWidth() <= 0 || texture2D->getTextureHeight() <= 0)
            return std::nullopt;

        osg::ref_ptr<osg::FrameBufferObject> framebuffer = new osg::FrameBufferObject;
        framebuffer->setAttachment(
            osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(texture2D));
        framebuffer->apply(mState, osg::FrameBufferObject::READ_FRAMEBUFFER);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

        RenderDiagnosticImage image;
        image.mName = std::move(name);
        image.mWidth = static_cast<std::uint32_t>(texture2D->getTextureWidth());
        image.mHeight = static_cast<std::uint32_t>(texture2D->getTextureHeight());
        image.mChannels = 4;
        image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight * image.mChannels);
        glReadPixels(0, 0, static_cast<GLsizei>(image.mWidth), static_cast<GLsizei>(image.mHeight), GL_RGBA, GL_FLOAT,
            image.mPixels.data());
        return image;
    }

    std::optional<RenderDiagnosticImage> RenderDiagnosticCapture::captureDepthTexture(
        osg::Texture* texture, std::string name)
    {
        auto* texture2D = dynamic_cast<osg::Texture2D*>(texture);
        if (!texture2D || texture2D->getTextureWidth() <= 0 || texture2D->getTextureHeight() <= 0)
            return std::nullopt;

        osg::ref_ptr<osg::FrameBufferObject> framebuffer = new osg::FrameBufferObject;
        framebuffer->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER,
            osg::FrameBufferAttachment(texture2D));
        framebuffer->apply(mState, osg::FrameBufferObject::READ_FRAMEBUFFER);
        glReadBuffer(GL_NONE);

        RenderDiagnosticImage image;
        image.mName = std::move(name);
        image.mWidth = static_cast<std::uint32_t>(texture2D->getTextureWidth());
        image.mHeight = static_cast<std::uint32_t>(texture2D->getTextureHeight());
        image.mChannels = 1;
        image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight);
        glReadPixels(0, 0, static_cast<GLsizei>(image.mWidth), static_cast<GLsizei>(image.mHeight), GL_DEPTH_COMPONENT,
            GL_FLOAT, image.mPixels.data());
        return image;
    }

    RenderDiagnosticImage RenderDiagnosticCapture::captureFramebuffer(
        unsigned int framebuffer, int width, int height, std::string name)
    {
        if (width <= 0 || height <= 0)
            throw std::invalid_argument("render diagnostic framebuffer dimensions must be positive");

        mExtensions.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, framebuffer);
        glReadBuffer(framebuffer == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0_EXT);

        RenderDiagnosticImage image;
        image.mName = std::move(name);
        image.mWidth = static_cast<std::uint32_t>(width);
        image.mHeight = static_cast<std::uint32_t>(height);
        image.mChannels = 4;
        image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight * image.mChannels);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, image.mPixels.data());
        return image;
    }
}
