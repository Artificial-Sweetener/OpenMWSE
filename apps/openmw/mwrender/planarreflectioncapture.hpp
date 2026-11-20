#ifndef OPENMW_MWRENDER_PLANARREFLECTIONCAPTURE_H
#define OPENMW_MWRENDER_PLANARREFLECTIONCAPTURE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Referenced>
#include <osg/ref_ptr>

#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/waterutil.hpp>

namespace osg
{
    class Camera;
    class Node;
    class Uniform;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace MWRender
{
    class PlanarReflectionClipCullNode;

    /// Retain matrices needed to reconstruct one exact-capture view.
    struct ExactPlanarReflectionCaptureView
    {
        osg::ref_ptr<osg::Uniform> mInverseProjection;
        osg::Matrixf mViewToWorld;
    };

    /// Render one plane with the established direct-reflection camera contract.
    class ExactPlanarReflectionCapture final : public SceneUtil::RTTNode
    {
    public:
        /// Allocate one exact capture at the requested dimensions and render order.
        ExactPlanarReflectionCapture(unsigned int width, unsigned int height, bool interior, bool visibilityDriven,
            int renderOrder, bool sharedSource, float projectionScale);

        /// Release capture-owned scene nodes after the final render traversal.
        ~ExactPlanarReflectionCapture() override;

        /// Configure one per-view direct camera with the established reflection contract.
        void setDefaults(osg::Camera* camera) override;

        /// Apply visibility and the exact receiver reflection matrix for the current view.
        void apply(osg::Camera* camera, osgUtil::CullVisitor* visitor) override;

        /// Replace the exact reflection and clip plane.
        void setPlane(const SceneUtil::WaterSurfacePlane& plane);

        /// Clip a shared source to one conservative receiver-union half-space, or preserve the full scene.
        void setSharedClipPlane(const std::optional<SceneUtil::WaterSurfacePlane>& plane);

        /// Replace the reflected scene without reallocating the target.
        void setScene(osg::Node* scene);

        /// Record receiver visibility for the direct camera's bounded update policy.
        void markVisible(unsigned int frame);

        /// Match the selected receiver's established direct-render visibility and detail policy.
        void setVisibilityDriven(bool visibilityDriven);

        /// Return the last source-camera record for this view, or null before that source has been culled.
        const ExactPlanarReflectionCaptureView* viewIfAvailable(osgUtil::CullVisitor& visitor) const noexcept;

        /// Return the exact world-space plane positioning this source camera.
        const SceneUtil::WaterSurfacePlane& plane() const noexcept { return mPlane; }

        /// Return whether this source's camera and acquisition clip exactly match one receiver plane.
        bool capturesExactPlane(const SceneUtil::WaterSurfacePlane& plane) const noexcept;

        /// Update bounded provider-state counters recorded alongside the source pass.
        void setSourceDiagnostics(std::size_t receiverCount, std::uint64_t anchorId, unsigned int anchorChanges,
            double anchorImpact, std::uint64_t comparisonId, double comparisonImpact, unsigned int handoffState,
            double priorWeight);

        /// Require a fresh direct draw before publishing a transition into this path.
        void beginStage();

        /// Return whether this camera has completed a draw since its latest stage request.
        bool completed() const;

        /// Apply the current interior detail constraint.
        void setInterior(bool interior);

        /// Apply ToggleWorld filtering without changing other reflection detail.
        void showWorld(bool show);

        /// Enable camera traversal while preserving the last completed texture when disabled.
        void setEnabled(bool enabled);

        /// Publish this exact target as the dominant reflection in render-frame diagnostics.
        void setDiagnosticDominant(bool dominant);

        /// Publish this target as the strongest non-dominant receiver in render-frame diagnostics.
        void setDiagnosticComparison(bool comparison);

    private:
        /// Retain matrices that produced source attachments after reflection clipping transforms.
        void captureEffectiveView(osgUtil::CullVisitor& visitor, const osg::Matrixd& view);

        /// Build the direct camera mask from live detail and world visibility.
        unsigned int currentMask(bool background) const;

        bool mInterior = false;
        bool mVisibilityDriven = false;
        bool mSharedSource = false;
        float mProjectionScale = 1.f;
        bool mShowWorld = true;
        bool mEnabled = false;
        bool mHasRendered = false;
        unsigned int mVisibleFrame = 0;
        SceneUtil::WaterSurfacePlane mPlane;
        std::optional<SceneUtil::WaterSurfacePlane> mSharedClipPlane;
        osg::Matrixd mReflectionMatrix;
        osg::ref_ptr<PlanarReflectionClipCullNode> mClipCullNode;
        osg::ref_ptr<osg::Node> mScene;
        std::unordered_map<osgUtil::CullVisitor*, ExactPlanarReflectionCaptureView> mViews;
        std::shared_ptr<std::atomic_bool> mCompleted = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> mPublishDiagnosticDominant = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> mPublishDiagnosticComparison = std::make_shared<std::atomic_bool>(false);
        double mReceiverCount = 0.0;
        double mAnchorId = 0.0;
        double mAnchorChanges = 0.0;
        double mAnchorImpact = 0.0;
        double mComparisonId = 0.0;
        double mComparisonImpact = 0.0;
        double mHandoffState = 0.0;
        double mPriorWeight = 0.0;
    };
}

#endif
