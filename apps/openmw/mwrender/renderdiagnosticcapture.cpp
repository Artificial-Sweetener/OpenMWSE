#include "renderdiagnosticcapture.hpp"

#include <algorithm>
#include <cmath>
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
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &mPreviousDrawFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &mPreviousReadBuffer);
    }

    RenderDiagnosticCapture::~RenderDiagnosticCapture()
    {
        restoreFramebufferState();
    }

    void RenderDiagnosticCapture::restoreFramebufferState()
    {
        mExtensions.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, mPreviousReadFramebuffer);
        mExtensions.glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, mPreviousDrawFramebuffer);
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
        restoreFramebufferState();
        framebuffer = nullptr;
        return image;
    }

    std::optional<RenderDiagnosticImage> RenderDiagnosticCapture::captureDepthTexture(
        osg::Texture* texture, std::string name, unsigned maximumDimension)
    {
        auto* texture2D = dynamic_cast<osg::Texture2D*>(texture);
        if (!texture2D || texture2D->getTextureWidth() <= 0 || texture2D->getTextureHeight() <= 0)
            return std::nullopt;

        const unsigned sourceWidth = static_cast<unsigned>(texture2D->getTextureWidth());
        const unsigned sourceHeight = static_cast<unsigned>(texture2D->getTextureHeight());
        const double scale = maximumDimension == 0
            ? 1.0
            : std::min(1.0, static_cast<double>(maximumDimension) / std::max(sourceWidth, sourceHeight));
        const unsigned captureWidth
            = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(sourceWidth) * scale)));
        const unsigned captureHeight
            = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(sourceHeight) * scale)));

        osg::ref_ptr<osg::FrameBufferObject> sourceFramebuffer = new osg::FrameBufferObject;
        sourceFramebuffer->setAttachment(
            osg::FrameBufferObject::BufferComponent::DEPTH_BUFFER, osg::FrameBufferAttachment(texture2D));
        sourceFramebuffer->apply(mState, osg::FrameBufferObject::READ_FRAMEBUFFER);
        glReadBuffer(GL_NONE);

        osg::ref_ptr<osg::Texture2D> captureTexture;
        osg::ref_ptr<osg::FrameBufferObject> captureFramebuffer;
        if (captureWidth != sourceWidth || captureHeight != sourceHeight)
        {
            captureTexture = new osg::Texture2D;
            captureTexture->setTextureSize(static_cast<int>(captureWidth), static_cast<int>(captureHeight));
            captureTexture->setInternalFormat(texture2D->getInternalFormat());
            captureTexture->setSourceFormat(GL_DEPTH_COMPONENT);
            captureTexture->setSourceType(GL_FLOAT);
            captureTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            captureTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);

            captureFramebuffer = new osg::FrameBufferObject;
            captureFramebuffer->setAttachment(
                osg::FrameBufferObject::BufferComponent::DEPTH_BUFFER, osg::FrameBufferAttachment(captureTexture));
            captureFramebuffer->apply(mState, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            mExtensions.glBlitFramebuffer(0, 0, static_cast<GLint>(sourceWidth), static_cast<GLint>(sourceHeight), 0, 0,
                static_cast<GLint>(captureWidth), static_cast<GLint>(captureHeight), GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            captureFramebuffer->apply(mState, osg::FrameBufferObject::READ_FRAMEBUFFER);
            glReadBuffer(GL_NONE);
        }

        RenderDiagnosticImage image;
        image.mName = std::move(name);
        image.mWidth = captureWidth;
        image.mHeight = captureHeight;
        image.mChannels = 1;
        image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight);
        glReadPixels(0, 0, static_cast<GLsizei>(image.mWidth), static_cast<GLsizei>(image.mHeight), GL_DEPTH_COMPONENT,
            GL_FLOAT, image.mPixels.data());
        restoreFramebufferState();
        captureFramebuffer = nullptr;
        captureTexture = nullptr;
        sourceFramebuffer = nullptr;
        return image;
    }

    RenderDiagnosticImage RenderDiagnosticCapture::captureFramebuffer(
        unsigned int framebuffer, int width, int height, std::string name, unsigned maximumDimension)
    {
        if (width <= 0 || height <= 0)
            throw std::invalid_argument("render diagnostic framebuffer dimensions must be positive");

        const unsigned sourceWidth = static_cast<unsigned>(width);
        const unsigned sourceHeight = static_cast<unsigned>(height);
        const double scale = maximumDimension == 0
            ? 1.0
            : std::min(1.0, static_cast<double>(maximumDimension) / std::max(sourceWidth, sourceHeight));
        const unsigned captureWidth
            = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(sourceWidth) * scale)));
        const unsigned captureHeight
            = std::max(1u, static_cast<unsigned>(std::lround(static_cast<double>(sourceHeight) * scale)));

        mExtensions.glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, framebuffer);
        glReadBuffer(framebuffer == 0 ? mPreviousReadBuffer : GL_COLOR_ATTACHMENT0_EXT);

        osg::ref_ptr<osg::Texture2D> captureTexture;
        osg::ref_ptr<osg::FrameBufferObject> captureFramebuffer;
        if (captureWidth != sourceWidth || captureHeight != sourceHeight)
        {
            captureTexture = new osg::Texture2D;
            captureTexture->setTextureSize(static_cast<int>(captureWidth), static_cast<int>(captureHeight));
            captureTexture->setInternalFormat(GL_RGBA8);
            captureTexture->setSourceFormat(GL_RGBA);
            captureTexture->setSourceType(GL_FLOAT);
            captureTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            captureTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

            captureFramebuffer = new osg::FrameBufferObject;
            captureFramebuffer->setAttachment(
                osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, osg::FrameBufferAttachment(captureTexture));
            captureFramebuffer->apply(mState, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            mExtensions.glBlitFramebuffer(0, 0, width, height, 0, 0, static_cast<GLint>(captureWidth),
                static_cast<GLint>(captureHeight), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            captureFramebuffer->apply(mState, osg::FrameBufferObject::READ_FRAMEBUFFER);
            glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
        }

        RenderDiagnosticImage image;
        image.mName = std::move(name);
        image.mWidth = captureWidth;
        image.mHeight = captureHeight;
        image.mChannels = 4;
        image.mPixels.resize(static_cast<std::size_t>(image.mWidth) * image.mHeight * image.mChannels);
        glReadPixels(0, 0, static_cast<GLsizei>(captureWidth), static_cast<GLsizei>(captureHeight), GL_RGBA, GL_FLOAT,
            image.mPixels.data());
        restoreFramebufferState();
        captureFramebuffer = nullptr;
        captureTexture = nullptr;
        return image;
    }
}
