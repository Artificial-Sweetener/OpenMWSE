#ifndef OPENMW_MWRENDER_PINGPONGCANVAS_H
#define OPENMW_MWRENDER_PINGPONGCANVAS_H

#include <array>
#include <optional>

#include <osg/FrameBufferObject>
#include <osg/Geometry>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/buffered_value>

#include <components/fx/technique.hpp>

#include "luminancecalculator.hpp"

namespace Shader
{
    class ShaderManager;
}

namespace MWRender
{
    class AtmosphereDiagnostics;
    class RenderFrameDiagnostics;

    class PingPongCanvas : public osg::Geometry
    {
    public:
        /// Create a frame canvas that services shared production-render diagnostic requests.
        PingPongCanvas(Shader::ShaderManager& shaderManager,
            const std::shared_ptr<LuminanceCalculator>& luminanceCalculator,
            std::shared_ptr<AtmosphereDiagnostics> atmosphereDiagnostics,
            std::shared_ptr<RenderFrameDiagnostics> renderFrameDiagnostics);

        void drawGeometry(osg::RenderInfo& renderInfo) const;

        void drawImplementation(osg::RenderInfo& renderInfo) const override;

        void resizeGLObjectBuffers(unsigned int maxSize) override;

        /// Invalidate cached framebuffer attachments and all temporal atmosphere history.
        void dirty()
        {
            mDirty = true;
            mAtmosphereHistoryValid = false;
        }

        /// Reject temporal atmosphere inputs without rebuilding unrelated framebuffer attachments.
        void invalidateAtmosphereHistory() { mAtmosphereHistoryValid = false; }

        /// Replace only attachments whose technique render-target definitions changed.
        void setDirtyAttachments(const std::vector<Fx::Types::RenderTarget>& attachments);

        const Fx::DispatchArray& getPasses() { return mPasses; }

        /// Replace the frame-local dispatch graph and invalidate atmosphere history tied to the previous graph.
        void setPasses(Fx::DispatchArray&& passes);

        /// Identify the opposite frame canvas that owns temporal atmosphere inputs.
        void setPreviousCanvas(const PingPongCanvas* canvas) { mPreviousCanvas = canvas; }

        /// Return the named render target from the current dispatch graph when available.
        osg::Texture2D* findRenderTarget(std::string_view name) const;

        void setMask(bool underwater, bool exterior);

        void setTextureScene(osg::ref_ptr<osg::Texture> tex) { mTextureScene = tex; }

        void setTextureDepth(osg::ref_ptr<osg::Texture> tex) { mTextureDepth = tex; }

        void setTextureNormals(osg::ref_ptr<osg::Texture> tex) { mTextureNormals = tex; }

        void setTextureDistortion(osg::ref_ptr<osg::Texture> tex) { mTextureDistortion = tex; }

        void setCalculateAvgLum(bool enabled) { mAvgLum = enabled; }

        void setPostProcessing(bool enabled) { mPostprocessing = enabled; }

        const osg::ref_ptr<osg::Texture>& getSceneTexture(size_t frameId) const { return mTextureScene; }

    private:
        bool mAvgLum = false;
        bool mPostprocessing = false;

        Fx::DispatchArray mPasses;
        Fx::FlagsType mMask = 0;

        osg::ref_ptr<osg::Program> mFallbackProgram;
        osg::ref_ptr<osg::Program> mMultiviewResolveProgram;
        osg::ref_ptr<osg::StateSet> mFallbackStateSet;
        osg::ref_ptr<osg::StateSet> mMultiviewResolveStateSet;

        osg::ref_ptr<osg::Texture> mTextureScene;
        osg::ref_ptr<osg::Texture> mTextureDepth;
        osg::ref_ptr<osg::Texture> mTextureNormals;
        osg::ref_ptr<osg::Texture> mTextureDistortion;

        mutable bool mDirty = false;
        mutable bool mAtmosphereHistoryValid = false;
        const PingPongCanvas* mPreviousCanvas = nullptr;
        mutable std::vector<Fx::Types::RenderTarget> mDirtyAttachments;
        mutable osg::ref_ptr<osg::Viewport> mRenderViewport;
        mutable osg::ref_ptr<osg::FrameBufferObject> mMultiviewResolveFramebuffer;
        mutable osg::ref_ptr<osg::FrameBufferObject> mDestinationFBO;
        mutable std::array<osg::ref_ptr<osg::FrameBufferObject>, 3> mFbos;
        mutable std::shared_ptr<LuminanceCalculator> mLuminanceCalculator;
        std::shared_ptr<AtmosphereDiagnostics> mAtmosphereDiagnostics;
        std::shared_ptr<RenderFrameDiagnostics> mRenderFrameDiagnostics;
        mutable osg::buffered_object<osg::State::UniformMap> mEmptyUniformStacks;
    };
}

#endif
