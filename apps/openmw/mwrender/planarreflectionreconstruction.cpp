#include "planarreflectionreconstruction.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <osg/Camera>
#include <osg/FrontFace>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/PrimitiveSet>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osgUtil/CullVisitor>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/shader/shadermanager.hpp>

#include "planarreflectioncapture.hpp"
#include "planarreflectionrenderutils.hpp"
#include "renderworkcounter.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        constexpr float reconstructionPaddingPixels = 1.f;

        /// Build six local corners instanced once per independently reconstructed source sample.
        osg::ref_ptr<osg::Geometry> createProxyGeometry(unsigned int resolution)
        {
            const std::uint64_t instanceCount = static_cast<std::uint64_t>(resolution) * resolution;
            if (instanceCount > static_cast<std::uint64_t>(std::numeric_limits<GLsizei>::max()))
                throw std::invalid_argument("planar reflection resolution exceeds the instanced proxy limit");
            osg::ref_ptr<osg::Vec3Array> corners = new osg::Vec3Array;
            corners->push_back(osg::Vec3f(-0.5f, -0.5f, 0.f));
            corners->push_back(osg::Vec3f(0.5f, -0.5f, 0.f));
            corners->push_back(osg::Vec3f(0.5f, 0.5f, 0.f));
            corners->push_back(osg::Vec3f(-0.5f, -0.5f, 0.f));
            corners->push_back(osg::Vec3f(0.5f, 0.5f, 0.f));
            corners->push_back(osg::Vec3f(-0.5f, 0.5f, 0.f));
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(corners);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(instanceCount)));
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            geometry->setCullingActive(false);
            geometry->setInitialBound(
                osg::BoundingBox(osg::Vec3f(-1.0e7f, -1.0e7f, -1.0e7f), osg::Vec3f(1.0e7f, 1.0e7f, 1.0e7f)));
            geometry->setDrawCallback(makePlanarReflectionProxyDrawCallback());
            return geometry;
        }

        /// Bind one source capture to one target camera using exact per-view cull matrices.
        class ReconstructionStateUpdater final : public SceneUtil::StateSetUpdater
        {
        public:
            /// Retain source and target ownership while allocating view-local state lazily.
            ReconstructionStateUpdater(ExactPlanarReflectionCapture& source, const bool& sourceUpdates,
                const SceneUtil::WaterSurfacePlane& targetPlane, unsigned int resolution)
                : mSource(source)
                , mSourceUpdates(&sourceUpdates)
                , mTargetPlane(targetPlane)
                , mResolution(resolution)
            {
            }

            /// Allocate stable sampler and coordinate-convention uniforms for one cull view.
            void setDefaults(osg::StateSet* state) override
            {
                state->addUniform(new osg::Uniform("sourceColor", 0));
                state->addUniform(new osg::Uniform("sourceDepth", 1));
                state->addUniform(new osg::Uniform("proxyResolution", static_cast<float>(mResolution)));
                state->addUniform(new osg::Uniform("sourceResolution", static_cast<float>(mSource.width())));
                state->addUniform(new osg::Uniform("sourceClearDepth", SceneUtil::AutoDepth::isReversed() ? 0.f : 1.f));
                state->addUniform(new osg::Uniform("sourceDepthNdcScaleBias",
                    SceneUtil::AutoDepth::isReversed() ? osg::Vec2f(1.f, 0.f) : osg::Vec2f(2.f, -1.f)));
                state->addUniform(new osg::Uniform("sourceViewToWorld", osg::Matrixf()));
                state->addUniform(new osg::Uniform("sourceViewToTarget", osg::Matrixf()));
                state->addUniform(new osg::Uniform("targetProjection", osg::Matrixf()));
                state->addUniform(new osg::Uniform("targetNdcPadding",
                    osg::Vec2f(reconstructionPaddingPixels * 2.f / static_cast<float>(mResolution),
                        reconstructionPaddingPixels * 2.f / static_cast<float>(mResolution))));
                state->addUniform(new osg::Uniform("targetPlane", osg::Vec4f()));
                state->addUniform(new osg::Uniform("targetClipEnabled", 1.f));
                state->addUniform(new osg::Uniform("sourceTargetCoincident", 0.f));
            }

            /// Bind source attachments and compose them into the target camera's effective cull view.
            void apply(osg::StateSet* state, osg::NodeVisitor* visitor) override
            {
                osgUtil::CullVisitor* cullVisitor = visitor->asCullVisitor();
                const ExactPlanarReflectionCaptureView* sourceView = mSource.viewIfAvailable(*cullVisitor);
                if (sourceView == nullptr)
                    return;
                state->setTextureAttribute(0, mSource.getColorTexture(cullVisitor), osg::StateAttribute::ON);
                state->setTextureAttribute(1, mSource.getDepthTexture(cullVisitor), osg::StateAttribute::ON);
                state->addUniform(
                    sourceView->mInverseProjection, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                state->getUniform("sourceViewToWorld")->set(sourceView->mViewToWorld);
                const osg::Matrixf targetView(*cullVisitor->getModelViewMatrix());
                const osg::Matrixf sourceViewToTarget = sourceView->mViewToWorld * targetView;
                const osg::Matrixf targetProjection = inheritedPlanarReflectionShaderProjection(*cullVisitor);
                state->getUniform("sourceViewToTarget")->set(sourceViewToTarget);
                state->getUniform("targetProjection")->set(targetProjection);
                state->getUniform("targetPlane")
                    ->set(osg::Vec4f(mTargetPlane.mNormal.x(), mTargetPlane.mNormal.y(), mTargetPlane.mNormal.z(),
                        -mTargetPlane.mDistance));
                const bool coincident = *mSourceUpdates && mSource.capturesExactPlane(mTargetPlane);
                state->getUniform("targetClipEnabled")->set(coincident ? 0.f : 1.f);
                state->getUniform("sourceTargetCoincident")->set(coincident ? 1.f : 0.f);
            }

        private:
            ExactPlanarReflectionCapture& mSource;
            const bool* mSourceUpdates;
            const SceneUtil::WaterSurfacePlane& mTargetPlane;
            unsigned int mResolution;
        };
    }

    SharedPlanarReflectionTarget::SharedPlanarReflectionTarget(unsigned int resolution,
        ExactPlanarReflectionCapture& exactSource, ExactPlanarReflectionCapture& unionSource, osg::Group& sceneRoot,
        Shader::ShaderManager& shaderManager, bool visibilityDriven)
        : RTTNode(resolution, resolution, 0, false, 10, StereoAwareness::Aware, shouldAddMSAAIntermediateTarget())
        , mSources{ &exactSource, &unionSource }
        , mSceneRoot(sceneRoot)
        , mViewRoot(new osg::Group)
        , mViewContent(new osg::Group)
        , mVisibilityDriven(visibilityDriven)
    {
        setDepthBufferInternalFormat(GL_DEPTH32F_STENCIL8);
        for (std::size_t layer = 0; layer < mSources.size(); ++layer)
        {
            mProxyRoots[layer] = new osg::Group;
            mProxies[layer] = new osg::Geode;
            mProxies[layer]->setName("Shared Planar Reflection Reconstruction Layer " + std::to_string(layer));
            mProxies[layer]->setCullingActive(false);
            mProxies[layer]->setNodeMask(~0u);
            mProxies[layer]->addDrawable(createProxyGeometry(resolution));
            mProxyRoots[layer]->setCullingActive(false);
            mProxyRoots[layer]->addCullCallback(
                new ReconstructionStateUpdater(*mSources[layer], mSourceUpdates, mPlane, resolution));
            mProxyRoots[layer]->addChild(mProxies[layer]);
            mViewContent->addChild(mProxyRoots[layer]);
            osg::StateSet* state = mProxies[layer]->getOrCreateStateSet();
            state->setAttributeAndModes(shaderManager.getProgram("planarreflectionproxy"),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setAttributeAndModes(
                new SceneUtil::AutoDepth, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }
        mViewRoot->addCullCallback(makePlanarReflectionViewTransformCallback(&mPlane));
        mViewContent->addChild(&mSceneRoot);
        mViewRoot->addChild(mViewContent);
    }

    SharedPlanarReflectionTarget::~SharedPlanarReflectionTarget() = default;

    void SharedPlanarReflectionTarget::setDefaults(osg::Camera* camera)
    {
        camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
        camera->setName(std::string(SceneUtil::reflectionCameraName));
        camera->setClearColor(osg::Vec4f(0.f, 0.f, 0.f, 1.f));
        camera->addCullCallback(makePlanarReflectionInheritViewPointCallback());
        camera->addFinalDrawCallback(makePlanarReflectionCompletionCallback(mCompleted));
        camera->getOrCreateStateSet()->addUniform(new osg::Uniform("isReflection", true));
        camera->getOrCreateStateSet()->setAttributeAndModes(
            new osg::FrontFace(osg::FrontFace::CLOCKWISE), osg::StateAttribute::ON);
        camera->addChild(mViewRoot);
        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
    }

    void SharedPlanarReflectionTarget::apply(osg::Camera* camera, osgUtil::CullVisitor* visitor)
    {
        constexpr std::string_view callbackKey = "OpenMW comparison planar reflection diagnostic callback";
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
        const bool sourceAvailable = mSources.front()->viewIfAvailable(*visitor) != nullptr
            && (!mSources.back()->capturesExactPlane(mSources.front()->plane())
                    ? mSources.back()->viewIfAvailable(*visitor) != nullptr
                    : true);
        const bool exactCopy = mSourceUpdates && mSources.front()->capturesExactPlane(mPlane);
        const bool distinctUnionLayer = !mSources.back()->capturesExactPlane(mSources.front()->plane());
        mProxyRoots.front()->setNodeMask(~0u);
        mProxyRoots.back()->setNodeMask(!exactCopy && distinctUnionLayer ? ~0u : 0u);
        if (camera->getStats() != nullptr)
        {
            camera->getStats()->setAttribute(frame, std::string(meshWaterReflectionActiveCounter), visible ? 1.0 : 0.0);
            camera->getStats()->setAttribute(frame, std::string(meshWaterReflectionHeightCounter), mPlane.mDistance);
            camera->getStats()->setAttribute(
                frame, std::string(meshWaterReflectionResolutionCounter), static_cast<double>(width()));
        }
        const unsigned int visibleMask = exactCopy
            ? Mask_RenderToTexture
            : Mask_Scene | Mask_Sky | Mask_Sun | Mask_Lighting | Mask_RenderToTexture;
        camera->setCullMask(visible && sourceAvailable ? visibleMask : 0u);
        camera->setClearMask(
            visible && sourceAvailable ? GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0u);
        if (!visible || !sourceAvailable)
            return;
        mHasRendered = true;
    }

    void SharedPlanarReflectionTarget::setPlane(const SceneUtil::WaterSurfacePlane& plane)
    {
        mPlane = plane;
        mReflectionMatrix = SceneUtil::waterSurfaceReflectionMatrix(plane);
    }

    void SharedPlanarReflectionTarget::markVisible(unsigned int frame)
    {
        mVisibleFrame = frame;
    }

    void SharedPlanarReflectionTarget::beginStage()
    {
        mCompleted->store(false, std::memory_order_release);
        mHasRendered = false;
    }

    bool SharedPlanarReflectionTarget::completed() const
    {
        return mCompleted->load(std::memory_order_acquire);
    }

    void SharedPlanarReflectionTarget::setEnabled(bool enabled)
    {
        mEnabled = enabled;
        setNodeMask(enabled ? Mask_RenderToTexture : 0u);
    }

    void SharedPlanarReflectionTarget::setSourceUpdates(bool sourceUpdates)
    {
        mSourceUpdates = sourceUpdates;
    }

    void SharedPlanarReflectionTarget::setDiagnosticDominant(bool dominant)
    {
        mPublishDiagnosticDominant->store(dominant, std::memory_order_release);
    }

    void SharedPlanarReflectionTarget::setDiagnosticComparison(bool comparison)
    {
        mPublishDiagnosticComparison->store(comparison, std::memory_order_release);
    }
}
