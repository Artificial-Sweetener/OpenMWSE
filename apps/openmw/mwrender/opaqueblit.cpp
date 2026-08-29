#include "opaqueblit.hpp"

#include "renderworkcounter.hpp"

#include <osg/Camera>
#include <osg/Stats>
#include <osgUtil/RenderStage>

#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

namespace MWRender
{
    void OpaqueColorBinCallback::drawImplementation(
        osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous)
    {
        resolve(bin, renderInfo);
        bin->drawImplementation(renderInfo, previous);
    }

    void OpaqueColorBinCallback::resolve(osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo)
    {
        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        const unsigned int frameId = state.getFrameStamp()->getFrameNumber() % 2;
        const auto& fbo = mFbo[frameId];
        const auto& msaaFbo = mMsaaFbo[frameId];
        const auto& opaqueFbo = mOpaqueFbo[frameId];

        if (!bin->getStage()->getFrameBufferObject()
            || (bin->getStage()->getFrameBufferObject() != fbo && bin->getStage()->getFrameBufferObject() != msaaFbo))
            return;

        const osg::Texture* colorTex
            = opaqueFbo->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0).getTexture();
        if (Stereo::getMultiview())
        {
            if (!mMultiviewResolve[frameId])
                mMultiviewResolve[frameId] = std::make_unique<Stereo::MultiviewFramebufferResolve>(
                    msaaFbo ? msaaFbo : fbo, opaqueFbo, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            else
            {
                mMultiviewResolve[frameId]->setResolveFbo(opaqueFbo);
                mMultiviewResolve[frameId]->setMsaaFbo(msaaFbo ? msaaFbo : fbo);
            }
            mMultiviewResolve[frameId]->resolveImplementation(state);
        }
        else
        {
            (msaaFbo ? msaaFbo : fbo)->apply(state, osg::FrameBufferObject::READ_FRAMEBUFFER);
            opaqueFbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
            ext->glBlitFramebuffer(0, 0, colorTex->getTextureWidth(), colorTex->getTextureHeight(), 0, 0,
                colorTex->getTextureWidth(), colorTex->getTextureHeight(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                GL_NEAREST);
        }

        if (osg::Stats* stats = bin->getStage()->getCamera()->getStats())
        {
            const unsigned int frameNumber = state.getFrameStamp()->getFrameNumber();
            const double layers = Stereo::getMultiview() ? 2.0 : 1.0;
            const double destinationBytes
                = static_cast<double>(colorTex->getTextureWidth()) * colorTex->getTextureHeight() * 8.0 * layers;
            addPostProcessingWorkCounter(*stats, frameNumber, colorCopyCounter, 1.0);
            addPostProcessingWorkCounter(*stats, frameNumber, depthCopyCounter, 1.0);
            addPostProcessingWorkCounter(*stats, frameNumber, resolveByteCounter, destinationBytes);
        }

        (msaaFbo ? msaaFbo : fbo)->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
    }
}
