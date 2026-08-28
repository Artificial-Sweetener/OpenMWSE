#ifndef OPENMW_MWRENDER_PINGPONGCULL_H
#define OPENMW_MWRENDER_PINGPONGCULL_H

#include <array>

#include <components/sceneutil/nodecallback.hpp>

#include "postprocessor.hpp"

namespace osg
{
    class StateSet;
    class Viewport;
}

namespace MWRender
{
    class PostProcessor;
    /// Publish current and previous camera transforms while routing scene rendering into alternating post-process
    /// frames.
    class PingPongCull : public SceneUtil::NodeCallback<PingPongCull, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        /// Create the cull callback for a post-processor owner.
        PingPongCull(PostProcessor* pp);
        /// Release retained viewport state.
        ~PingPongCull();

        /// Cull one frame while preserving per-eye camera history for temporal atmosphere reprojection.
        void operator()(osg::Node* node, osgUtil::CullVisitor* nv);

    private:
        std::array<osg::Matrixf, 2> mLastViewMatrix;
        std::array<osg::Matrixf, 2> mLastProjectionMatrix;
        osg::ref_ptr<osg::StateSet> mViewportStateset;
        osg::ref_ptr<osg::Viewport> mViewport;
        PostProcessor* mPostProcessor;
    };
}

#endif
