#ifndef OPENMW_MWRENDER_PLANARREFLECTIONRECONSTRUCTION_H
#define OPENMW_MWRENDER_PLANARREFLECTIONRECONSTRUCTION_H

#include <array>
#include <atomic>
#include <memory>

#include <osg/Matrixd>
#include <osg/ref_ptr>

#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/waterutil.hpp>

namespace osg
{
    class Camera;
    class Geode;
    class Group;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace Shader
{
    class ShaderManager;
}

namespace MWRender
{
    class ExactPlanarReflectionCapture;

    /// Reconstruct one receiver texture from shared finite-scene attachments and receiver-local background.
    class SharedPlanarReflectionTarget final : public SceneUtil::RTTNode
    {
    public:
        /// Allocate one receiver-local output and independent-patch proxy grid.
        SharedPlanarReflectionTarget(unsigned int resolution, ExactPlanarReflectionCapture& exactSource,
            ExactPlanarReflectionCapture& unionSource, osg::Group& sceneRoot, Shader::ShaderManager& shaderManager,
            bool visibilityDriven);

        /// Release target-owned reconstruction nodes after the final render traversal.
        ~SharedPlanarReflectionTarget() override;

        /// Configure one per-view target camera with its reflected background and reconstruction proxy.
        void setDefaults(osg::Camera* camera) override;

        /// Transform reconstructed samples into this receiver's reflected view.
        void apply(osg::Camera* camera, osgUtil::CullVisitor* visitor) override;

        /// Replace the receiver plane and relative reflected view.
        void setPlane(const SceneUtil::WaterSurfacePlane& plane);

        /// Record receiver visibility for bounded reconstruction work.
        void markVisible(unsigned int frame);

        /// Require a fresh reconstructed frame before publishing shared mode.
        void beginStage();

        /// Return whether this receiver completed reconstruction since its latest stage request.
        bool completed() const;

        /// Enable or suspend this receiver target.
        void setEnabled(bool enabled);

        /// Declare whether this lane's source is refreshed rather than retained as a transition snapshot.
        void setSourceUpdates(bool sourceUpdates);

        /// Publish this target as the dominant diagnostic receiver.
        void setDiagnosticDominant(bool dominant);

        /// Publish this target as the strongest non-dominant diagnostic receiver.
        void setDiagnosticComparison(bool comparison);

    private:
        std::array<ExactPlanarReflectionCapture*, 2> mSources;
        osg::Group& mSceneRoot;
        osg::ref_ptr<osg::Group> mViewRoot;
        osg::ref_ptr<osg::Group> mViewContent;
        bool mVisibilityDriven = false;
        bool mEnabled = false;
        bool mSourceUpdates = false;
        bool mHasRendered = false;
        unsigned int mVisibleFrame = 0;
        SceneUtil::WaterSurfacePlane mPlane;
        osg::Matrixd mReflectionMatrix;
        std::array<osg::ref_ptr<osg::Group>, 2> mProxyRoots;
        std::array<osg::ref_ptr<osg::Geode>, 2> mProxies;
        std::shared_ptr<std::atomic_bool> mCompleted = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> mPublishDiagnosticDominant = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> mPublishDiagnosticComparison = std::make_shared<std::atomic_bool>(false);
    };
}

#endif
