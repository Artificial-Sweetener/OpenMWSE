#include "planarreflectioncapture.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Camera>
#include <osg/ClipNode>
#include <osg/ClipPlane>
#include <osg/FrontFace>
#include <osg/Group>
#include <osg/StateSet>
#include <osg/Texture>
#include <osg/Uniform>
#include <osgUtil/CullVisitor>

#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>

#include "planarreflectiondiagnostics.hpp"
#include "planarreflectionrenderutils.hpp"
#include "renderworkcounter.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    /// Apply the established reflected-view transform and clipping contract to one scene subtree.
    class PlanarReflectionClipCullNode final : public osg::Group
    {
        using ViewCapture = std::function<void(osgUtil::CullVisitor&, const osg::Matrixd&)>;

        class ViewCaptureCallback final
            : public SceneUtil::NodeCallback<ViewCaptureCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// Retain the effective scene model-view after the relative clip transform enters the cull stack.
            explicit ViewCaptureCallback(ViewCapture capture)
                : mCapture(std::move(capture))
            {
            }

            /// Publish the model-view used by descendants before continuing scene traversal.
            void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
            {
                mCapture(*visitor, *visitor->getModelViewMatrix());
                traverse(node, visitor);
            }

        private:
            ViewCapture mCapture;
        };

        class PlaneCullCallback final
            : public SceneUtil::NodeCallback<PlaneCullCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// Bind the callback to the plane updated by its owning clip node.
            PlaneCullCallback(const osg::Plane* clipPlane, const bool* enabled, const bool* cpuCullingEnabled)
                : mClipPlane(clipPlane)
                , mEnabled(enabled)
                , mCpuCullingEnabled(cpuCullingEnabled)
            {
            }

            /// Add the receiver plane to the reflected frustum for conservative CPU rejection.
            void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
            {
                if (!*mEnabled || !*mCpuCullingEnabled)
                {
                    traverse(node, visitor);
                    return;
                }
                osg::Polytope::PlaneList originalPlanes
                    = visitor->getProjectionCullingStack().back().getFrustum().getPlaneList();
                osg::Plane plane = *mClipPlane;
                plane.transform(*visitor->getCurrentRenderStage()->getInitialViewMatrix());
                if (mClipPlane->distance(visitor->getEyePoint()) > 0.0)
                    plane.flip();
                visitor->getProjectionCullingStack().back().getFrustum().add(plane);
                traverse(node, visitor);
                visitor->getProjectionCullingStack().back().getFrustum().set(originalPlanes);
            }

        private:
            const osg::Plane* mClipPlane;
            const bool* mEnabled;
            const bool* mCpuCullingEnabled;
        };

        class FlipCallback final : public SceneUtil::NodeCallback<FlipCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// Bind reflected traversal to the plane updated by its owning clip node.
            explicit FlipCallback(const osg::Plane* plane)
                : mPlane(plane)
            {
            }

            /// Apply the established reflection-space transform for one receiver.
            void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
            {
                osg::ref_ptr<osg::RefMatrix> modelViewMatrix = new osg::RefMatrix(
                    adjustedPlanarReflectionModelView(*visitor->getModelViewMatrix(), *mPlane, visitor->getEyePoint()));
                visitor->pushModelViewMatrix(modelViewMatrix, osg::Transform::RELATIVE_RF);
                traverse(node, visitor);
                visitor->popModelViewMatrix();
            }

        private:
            const osg::Plane* mPlane;
        };

    public:
        /// Build the capture's clip-plane hierarchy once for subsequent plane updates.
        explicit PlanarReflectionClipCullNode(ViewCapture capture = {})
        {
            addCullCallback(new PlaneCullCallback(&mClipPlane, &mEnabled, &mCpuCullingEnabled));
            mClipNodeTransform = new osg::Group;
            mClipNodeTransform->addCullCallback(new FlipCallback(&mReflectionPlane));
            osg::Group::addChild(mClipNodeTransform);
            mClipNode = new osg::ClipNode;
            if (capture)
                mClipNode->addCullCallback(new ViewCaptureCallback(std::move(capture)));
            mClipNodeTransform->addChild(mClipNode);
        }

        /// Replace the world-space plane that defines the reflected camera transform.
        void setReflectionPlane(const osg::Plane& plane)
        {
            mReflectionPlane = plane;
            updateClipPlane();
        }

        /// Replace the independently conservative world-space source clipping plane.
        void setClipPlane(const osg::Plane& plane)
        {
            mClipPlane = plane;
            updateClipPlane();
        }

        /// Enable or remove this node's GPU and CPU half-space clipping.
        void setEnabled(bool enabled)
        {
            if (enabled == mEnabled)
                return;
            mEnabled = enabled;
            updateClipPlane();
        }

        /// Enable conservative CPU rejection only when the clip and reflection planes coincide.
        void setCpuCullingEnabled(bool enabled) { mCpuCullingEnabled = enabled; }

    private:
        /// Synchronize the transformed OpenGL clip plane with CPU culling state.
        void updateClipPlane()
        {
            mClipNode->getClipPlaneList().clear();
            if (mEnabled)
            {
                const double localDistance = mClipPlane[3] - mReflectionPlane[3];
                mClipNode->addClipPlane(new osg::ClipPlane(0, osg::Plane(mClipPlane.getNormal(), localDistance)));
            }
            mClipNode->setStateSetModes(
                *getOrCreateStateSet(), mEnabled ? osg::StateAttribute::ON : osg::StateAttribute::OFF);
            mClipNode->setCullingActive(false);
        }

        osg::ref_ptr<osg::Group> mClipNodeTransform;
        osg::ref_ptr<osg::ClipNode> mClipNode;
        osg::Plane mReflectionPlane;
        osg::Plane mClipPlane;
        bool mEnabled = true;
        bool mCpuCullingEnabled = true;
    };

    namespace
    {
        /// Return the finite-scene mask for the configured reflection-detail level.
        unsigned int reflectionSceneMask(bool interior, bool includeBackground, bool visibilityDriven, bool showWorld)
        {
            int detail = std::clamp(Settings::water().mReflectionDetail.get(), interior ? 2 : 0, 5);
            if (visibilityDriven)
                detail = std::min(detail, 4);
            unsigned int mask = Mask_Scene | Mask_Lighting;
            if (includeBackground)
                mask |= Mask_Sky | Mask_Sun;
            if (detail >= 1)
                mask |= Mask_Terrain;
            if (detail >= 2)
                mask |= Mask_Static;
            if (detail >= 3)
                mask |= Mask_Effect | Mask_ParticleSystem | Mask_Object;
            if (detail >= 4)
                mask |= Mask_Player | Mask_Actor;
            if (detail >= 5)
                mask |= Mask_Groundcover;
            return showWorld ? mask : mask & ~sToggleWorldMask;
        }
    }

    ExactPlanarReflectionCapture::ExactPlanarReflectionCapture(unsigned int width, unsigned int height, bool interior,
        bool visibilityDriven, int renderOrder, bool sharedSource, float projectionScale)
        : RTTNode(width, height, 0, false, renderOrder, StereoAwareness::Aware, shouldAddMSAAIntermediateTarget())
        , mInterior(interior)
        , mVisibilityDriven(visibilityDriven)
        , mSharedSource(sharedSource)
        , mProjectionScale(projectionScale)
        , mClipCullNode(new PlanarReflectionClipCullNode(
              [this](osgUtil::CullVisitor& visitor, const osg::Matrixd& view) { captureEffectiveView(visitor, view); }))
    {
        setDepthBufferInternalFormat(GL_DEPTH32F_STENCIL8);
        // A shared source must retain geometry required by receivers on either side of its anchor plane. Each
        // receiver applies its own half-space clip after reconstruction.
        mClipCullNode->setEnabled(!mSharedSource);
    }

    ExactPlanarReflectionCapture::~ExactPlanarReflectionCapture() = default;

    void ExactPlanarReflectionCapture::setDefaults(osg::Camera* camera)
    {
        camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
        camera->setSmallFeatureCullingPixelSize(Settings::water().mSmallFeatureCullingPixelSize);
        camera->setName(std::string(
            mSharedSource ? SceneUtil::sharedPlanarReflectionSourceCameraName : SceneUtil::reflectionCameraName));
        camera->setClearColor(osg::Vec4f(0.f, 0.f, 0.f, 1.f));
        camera->addCullCallback(makePlanarReflectionInheritViewPointCallback());
        camera->addFinalDrawCallback(makePlanarReflectionCompletionCallback(mCompleted));
        camera->getOrCreateStateSet()->addUniform(new osg::Uniform("isReflection", true));
        camera->getOrCreateStateSet()->setAttributeAndModes(
            new osg::FrontFace(osg::FrontFace::CLOCKWISE), osg::StateAttribute::ON);
        camera->addChild(mClipCullNode);
        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
    }

    void ExactPlanarReflectionCapture::apply(osg::Camera* camera, osgUtil::CullVisitor* visitor)
    {
        if (mSharedSource)
        {
            camera->setProjectionMatrix(osg::Matrixd::scale(mProjectionScale, mProjectionScale, 1.0));
            osg::Matrixf projection = inheritedPlanarReflectionShaderProjection(*visitor);
            projection.postMult(osg::Matrixf::scale(mProjectionScale, mProjectionScale, 1.f));
            osg::Uniform* shaderProjection = camera->getOrCreateStateSet()->getUniform("projectionMatrix");
            if (shaderProjection == nullptr)
            {
                shaderProjection = new osg::Uniform("projectionMatrix", projection);
                camera->getOrCreateStateSet()->addUniform(
                    shaderProjection, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }
            else
                shaderProjection->set(projection);

            ExactPlanarReflectionCaptureView& sourceView = mViews[visitor];
            if (sourceView.mInverseProjection == nullptr)
                sourceView.mInverseProjection
                    = new osg::Uniform("sourceInverseProjection", osg::Matrixf::inverse(projection));
            else
                sourceView.mInverseProjection->set(osg::Matrixf::inverse(projection));
        }
        constexpr std::string_view callbackKey = "OpenMW dominant planar reflection diagnostic callback";
        bool callbackInstalled = false;
        if (!camera->getUserValue(std::string(callbackKey), callbackInstalled))
        {
            osg::Texture* color = getColorTexture(visitor);
            osg::Texture* depth = getDepthTexture(visitor);
            if (color != nullptr && depth != nullptr)
            {
                camera->addFinalDrawCallback(makePlanarReflectionAttachmentCallback(
                    *color, *depth, mPublishDiagnosticDominant, mPublishDiagnosticComparison));
                camera->setUserValue(std::string(callbackKey), true);
            }
        }
        camera->setViewMatrix(mReflectionMatrix);
        const unsigned int frame = visitor->getFrameStamp()->getFrameNumber();
        const bool visible = mEnabled
            && (!mVisibilityDriven || SceneUtil::waterReflectionShouldRender(mHasRendered, mVisibleFrame, frame));
        camera->setCullMask(visible ? currentMask(true) : 0u);
        camera->setClearMask(visible ? GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0u);
        if (mSharedSource)
        {
            osg::Texture* depth = getDepthTexture(visitor);
            if (depth)
            {
                depth->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
                depth->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            }
            if (camera->getStats())
            {
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionSourceResolutionCounter), static_cast<double>(width()));
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionReceiverCountCounter), mReceiverCount);
                camera->getStats()->setAttribute(frame, std::string(planarReflectionAnchorIdCounter), mAnchorId);
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionAnchorChangeCounter), mAnchorChanges);
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionAnchorImpactCounter), mAnchorImpact);
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionComparisonIdCounter), mComparisonId);
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionComparisonImpactCounter), mComparisonImpact);
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionSourcePlaneDistanceCounter), mPlane.mDistance);
                camera->getStats()->setAttribute(
                    frame, std::string(planarReflectionHandoffStateCounter), mHandoffState);
                camera->getStats()->setAttribute(frame, std::string(planarReflectionPriorWeightCounter), mPriorWeight);
            }
        }
        if (visible)
            mHasRendered = true;
        if (mVisibilityDriven && camera->getStats())
        {
            camera->getStats()->setAttribute(frame, std::string(meshWaterReflectionActiveCounter), visible ? 1.0 : 0.0);
            camera->getStats()->setAttribute(frame, std::string(meshWaterReflectionHeightCounter), mPlane.mDistance);
            camera->getStats()->setAttribute(
                frame, std::string(meshWaterReflectionResolutionCounter), static_cast<double>(width()));
        }
    }

    void ExactPlanarReflectionCapture::setPlane(const SceneUtil::WaterSurfacePlane& plane)
    {
        mPlane = plane;
        mReflectionMatrix = SceneUtil::waterSurfaceReflectionMatrix(plane);
        const osg::Vec3d normal(plane.mNormal);
        const osg::Plane worldPlane(normal, normal * static_cast<double>(plane.mDistance));
        mClipCullNode->setReflectionPlane(worldPlane);
        if (!mSharedSource)
        {
            mClipCullNode->setClipPlane(worldPlane);
            mClipCullNode->setCpuCullingEnabled(true);
            mClipCullNode->setEnabled(true);
        }
    }

    void ExactPlanarReflectionCapture::setSharedClipPlane(const std::optional<SceneUtil::WaterSurfacePlane>& plane)
    {
        if (!mSharedSource)
            throw std::logic_error("only a shared planar reflection source accepts a receiver-union clip plane");
        mSharedClipPlane = plane;
        if (!plane)
        {
            mClipCullNode->setEnabled(false);
            return;
        }
        const osg::Vec3d normal(plane->mNormal);
        mClipCullNode->setClipPlane(osg::Plane(normal, normal * static_cast<double>(plane->mDistance)));
        mClipCullNode->setCpuCullingEnabled(false);
        mClipCullNode->setEnabled(true);
    }

    bool ExactPlanarReflectionCapture::capturesExactPlane(const SceneUtil::WaterSurfacePlane& plane) const noexcept
    {
        if (!mSharedSource || !mSharedClipPlane)
            return false;
        return mPlane.mNormal * plane.mNormal > 0.999999f && mSharedClipPlane->mNormal * plane.mNormal > 0.999999f
            && std::abs(mPlane.mDistance - plane.mDistance) < 0.001f
            && std::abs(mSharedClipPlane->mDistance - plane.mDistance) < 0.001f
            && std::abs(mProjectionScale - 1.f) < 0.000001f;
    }

    void ExactPlanarReflectionCapture::setScene(osg::Node* scene)
    {
        if (mScene)
            mClipCullNode->removeChild(mScene);
        mScene = scene;
        if (mScene)
            mClipCullNode->addChild(mScene);
    }

    void ExactPlanarReflectionCapture::markVisible(unsigned int frame)
    {
        mVisibleFrame = frame;
    }

    void ExactPlanarReflectionCapture::setVisibilityDriven(bool visibilityDriven)
    {
        if (mVisibilityDriven == visibilityDriven)
            return;
        mVisibilityDriven = visibilityDriven;
        mHasRendered = false;
    }

    const ExactPlanarReflectionCaptureView* ExactPlanarReflectionCapture::viewIfAvailable(
        osgUtil::CullVisitor& visitor) const noexcept
    {
        const auto it = mViews.find(&visitor);
        return it != mViews.end() ? &it->second : nullptr;
    }

    void ExactPlanarReflectionCapture::setSourceDiagnostics(std::size_t receiverCount, std::uint64_t anchorId,
        unsigned int anchorChanges, double anchorImpact, std::uint64_t comparisonId, double comparisonImpact,
        unsigned int handoffState, double priorWeight)
    {
        mReceiverCount = static_cast<double>(receiverCount);
        mAnchorId = static_cast<double>(anchorId);
        mAnchorChanges = static_cast<double>(anchorChanges);
        mAnchorImpact = anchorImpact;
        mComparisonId = static_cast<double>(comparisonId);
        mComparisonImpact = comparisonImpact;
        mHandoffState = static_cast<double>(handoffState);
        mPriorWeight = priorWeight;
    }

    void ExactPlanarReflectionCapture::beginStage()
    {
        mCompleted->store(false, std::memory_order_release);
        mHasRendered = false;
    }

    bool ExactPlanarReflectionCapture::completed() const
    {
        return mCompleted->load(std::memory_order_acquire);
    }

    void ExactPlanarReflectionCapture::setInterior(bool interior)
    {
        mInterior = interior;
    }

    void ExactPlanarReflectionCapture::showWorld(bool show)
    {
        mShowWorld = show;
    }

    void ExactPlanarReflectionCapture::setEnabled(bool enabled)
    {
        mEnabled = enabled;
        setNodeMask(enabled ? Mask_RenderToTexture : 0u);
    }

    void ExactPlanarReflectionCapture::setDiagnosticDominant(bool dominant)
    {
        mPublishDiagnosticDominant->store(dominant, std::memory_order_release);
    }

    void ExactPlanarReflectionCapture::setDiagnosticComparison(bool comparison)
    {
        mPublishDiagnosticComparison->store(comparison, std::memory_order_release);
    }

    void ExactPlanarReflectionCapture::captureEffectiveView(osgUtil::CullVisitor& visitor, const osg::Matrixd& view)
    {
        if (!mSharedSource)
            return;
        ExactPlanarReflectionCaptureView& data = mViews[&visitor];
        data.mViewToWorld = osg::Matrixf::inverse(osg::Matrixf(view));
    }

    unsigned int ExactPlanarReflectionCapture::currentMask(bool background) const
    {
        return reflectionSceneMask(mInterior, background, mVisibilityDriven, mShowWorld);
    }
}
