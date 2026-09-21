#include "water.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <osg/ClipNode>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Fog>
#include <osg/FrontFace>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/PositionAttitudeTransform>
#include <osg/Stats>
#include <osg/Texture3D>
#include <osg/ViewportIndexed>
#include <osg/observer_ptr>

#include <osgUtil/CullVisitor>
#include <osgUtil/IncrementalCompileOperation>

#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/fog.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/surfaceeffect.hpp>
#include <components/sceneutil/waterutil.hpp>

#include <components/misc/constants.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/nifosg/controller.hpp>

#include <components/shader/shadermanager.hpp>

#include <components/esm3/loadcell.hpp>

#include <components/fallback/fallback.hpp>

#include <components/settings/values.hpp>

#include "../mwworld/cellstore.hpp"

#include "gpuscene.hpp"
#include "renderbin.hpp"
#include "renderworkcounter.hpp"
#include "ripples.hpp"
#include "ripplesimulation.hpp"
#include "util.hpp"
#include "vismask.hpp"
#include "waterwaveocclusion.hpp"

namespace MWRender
{
    namespace
    {
        /// Encode the validated appearance for the runtime shader selector.
        int shaderAppearance(Settings::WaterAppearance appearance)
        {
            return static_cast<int>(appearance);
        }

        /// Encode the validated enhanced color for the runtime shader selector.
        int shaderEnhancedColor(Settings::EnhancedWaterColor color)
        {
            return static_cast<int>(color);
        }

        /// Resolve an optional material-specific wave height against the live mesh-water control.
        float meshWaterWaveHeight(const SceneUtil::SurfaceEffectProfile& profile)
        {
            return profile.mGeometricWaveHeight.value_or(Settings::openmwseWater().mMeshGeometricWaveHeight.get());
        }

        /// Describe conventional world water through the shared arbitrary-plane contract.
        SceneUtil::WaterSurfacePlane horizontalWaterPlane(float height)
        {
            SceneUtil::WaterSurfacePlane plane;
            plane.mDistance = height;
            return plane;
        }

        /// Build the common water shader permutation and identify only the geometry-specific surface path.
        std::map<std::string, std::string> waterShaderDefines(bool geometricWaves, bool meshSurface)
        {
            std::map<std::string, std::string> defines;
            defines["waterRefraction"] = Settings::water().mRefraction ? "1" : "0";
            defines["rainRippleDetail"] = std::to_string(Settings::water().mRainRippleDetail);
            defines["rippleMapWorldScale"] = std::to_string(RipplesSurface::sWorldScaleFactor);
            defines["rippleMapSize"] = std::to_string(RipplesSurface::sRTTSize) + ".0";
            defines["sunlightScattering"] = Settings::water().mSunlightScattering ? "1" : "0";
            defines["wobblyShores"] = Settings::water().mWobblyShores ? "1" : "0";
            defines["waterGeometricWaves"] = geometricWaves ? "1" : "0";
            defines["waterMeshSurface"] = meshSurface ? "1" : "0";
            Stereo::shaderStereoDefines(defines);
            return defines;
        }

        /// Append a uniformly subdivided local triangle using a world-space density decision.
        void appendWaterTriangle(osg::Vec3Array& output, const osg::Vec3f& localA, const osg::Vec3f& localB,
            const osg::Vec3f& localC, const osg::Vec3f& worldA, const osg::Vec3f& worldB, const osg::Vec3f& worldC)
        {
            constexpr float maximumWorldEdgeLength = 128.f;
            const unsigned int divisions
                = SceneUtil::waterTriangleSubdivisionCount(worldA, worldB, worldC, maximumWorldEdgeLength);
            const auto point = [&](unsigned int alongB, unsigned int alongC) {
                const float inverseDivisions = 1.f / static_cast<float>(divisions);
                return localA + (localB - localA) * (static_cast<float>(alongB) * inverseDivisions)
                    + (localC - localA) * (static_cast<float>(alongC) * inverseDivisions);
            };
            output.reserve(output.size() + 3u * divisions * divisions);
            for (unsigned int alongB = 0; alongB < divisions; ++alongB)
            {
                for (unsigned int alongC = 0; alongC < divisions - alongB; ++alongC)
                {
                    output.push_back(point(alongB, alongC));
                    output.push_back(point(alongB + 1, alongC));
                    output.push_back(point(alongB, alongC + 1));
                    if (alongB + alongC + 1 < divisions)
                    {
                        output.push_back(point(alongB + 1, alongC));
                        output.push_back(point(alongB + 1, alongC + 1));
                        output.push_back(point(alongB, alongC + 1));
                    }
                }
            }
        }
    }

    // --------------------------------------------------------------------------------------------------------------------------------

    /// @brief Allows to cull and clip meshes that are below a plane. Useful for reflection camera effects.
    /// Also handles flipping of the plane when the eye point goes below it.
    /// To use, simply create the scene as subgraph of this node, then do setPlane(const osg::Plane& plane);
    class ClipCullNode : public osg::Group
    {
        class PlaneCullCallback : public SceneUtil::NodeCallback<PlaneCullCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// @param cullPlane The culling plane (in world space).
            PlaneCullCallback(const osg::Plane* cullPlane)
                : mCullPlane(cullPlane)
            {
            }

            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                osg::Polytope::PlaneList origPlaneList
                    = cv->getProjectionCullingStack().back().getFrustum().getPlaneList();

                osg::Plane plane = *mCullPlane;
                plane.transform(*cv->getCurrentRenderStage()->getInitialViewMatrix());

                const osg::Vec3d eyePoint = cv->getEyePoint();
                if (mCullPlane->distance(eyePoint) > 0.0)
                    plane.flip();

                cv->getProjectionCullingStack().back().getFrustum().add(plane);

                traverse(node, cv);

                // undo
                cv->getProjectionCullingStack().back().getFrustum().set(origPlaneList);
            }

        private:
            const osg::Plane* mCullPlane;
        };

        class FlipCallback : public SceneUtil::NodeCallback<FlipCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            FlipCallback(const osg::Plane* cullPlane)
                : mCullPlane(cullPlane)
            {
            }

            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                osg::Vec3d eyePoint = cv->getEyePoint();
                const double eyePlaneDistance = mCullPlane->distance(eyePoint);

                osg::RefMatrix* modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());

                // apply the height of the plane
                // we can't apply this height in the addClipPlane() since the "flip the below graph" function would
                // otherwise flip the height as well
                modelViewMatrix->preMultTranslate(mCullPlane->getNormal() * ((*mCullPlane)[3] * -1));

                // Reflect through the plane's local origin when the eye is on its normal-facing side. The camera's
                // reflection matrix supplies the plane translation, matching the established horizontal-water path.
                if (eyePlaneDistance > 0.0)
                {
                    SceneUtil::WaterSurfacePlane originPlane;
                    originPlane.mNormal = osg::Vec3f(mCullPlane->getNormal());
                    modelViewMatrix->preMult(SceneUtil::waterSurfaceReflectionMatrix(originPlane));
                }

                // move the plane back along its normal a little bit to prevent bleeding at the water shore
                const float fov = Settings::camera().mFieldOfView;
                constexpr double clipFudgeMin = 2.5; // minimum offset of clip plane
                constexpr double clipFudgeScale = -15000.0;
                double clipFudge = std::abs(eyePlaneDistance) * fov / clipFudgeScale - clipFudgeMin;
                modelViewMatrix->preMultTranslate(mCullPlane->getNormal() * clipFudge);

                cv->pushModelViewMatrix(modelViewMatrix, osg::Transform::RELATIVE_RF);
                traverse(node, cv);
                cv->popModelViewMatrix();
            }

        private:
            const osg::Plane* mCullPlane;
        };

    public:
        ClipCullNode()
        {
            addCullCallback(new PlaneCullCallback(&mPlane));

            mClipNodeTransform = new osg::Group;
            mClipNodeTransform->addCullCallback(new FlipCallback(&mPlane));
            osg::Group::addChild(mClipNodeTransform);

            mClipNode = new osg::ClipNode;

            mClipNodeTransform->addChild(mClipNode);
        }

        void setPlane(const osg::Plane& plane)
        {
            if (plane == mPlane)
                return;
            mPlane = plane;

            mClipNode->getClipPlaneList().clear();
            mClipNode->addClipPlane(
                new osg::ClipPlane(0, osg::Plane(mPlane.getNormal(), 0))); // mPlane.d() applied in FlipCallback
            mClipNode->setStateSetModes(*getOrCreateStateSet(), osg::StateAttribute::ON);
            mClipNode->setCullingActive(false);
        }

    private:
        osg::ref_ptr<osg::Group> mClipNodeTransform;
        osg::ref_ptr<osg::ClipNode> mClipNode;

        osg::Plane mPlane;
    };

    /// This callback on the Camera has the effect of a RELATIVE_RF_INHERIT_VIEWPOINT transform mode (which does not
    /// exist in OSG). We want to keep the View Point of the parent camera so we will not have to recreate LODs.
    class InheritViewPointCallback
        : public SceneUtil::NodeCallback<InheritViewPointCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        InheritViewPointCallback() {}

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
        {
            osg::ref_ptr<osg::RefMatrix> modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());
            cv->popModelViewMatrix();
            cv->pushModelViewMatrix(modelViewMatrix, osg::Transform::ABSOLUTE_RF_INHERIT_VIEWPOINT);
            traverse(node, cv);
        }
    };

    /// Moves water mesh away from the camera slightly if the camera gets too close on the Z axis.
    /// The offset works around graphics artifacts that occurred with the GL_DEPTH_CLAMP when the camera gets extremely
    /// close to the mesh (seen on NVIDIA at least). Must be added as a Cull callback.
    class FudgeCallback : public SceneUtil::NodeCallback<FudgeCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
        {
            const float fudge = 0.2f;
            if (std::abs(cv->getEyeLocal().z()) < fudge)
            {
                float diff = fudge - cv->getEyeLocal().z();
                osg::RefMatrix* modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());

                if (cv->getEyeLocal().z() >= 0)
                    modelViewMatrix->preMultTranslate(osg::Vec3f(0, 0, -diff));
                else
                    modelViewMatrix->preMultTranslate(osg::Vec3f(0, 0, diff));

                cv->pushModelViewMatrix(modelViewMatrix, osg::Transform::RELATIVE_RF);
                traverse(node, cv);
                cv->popModelViewMatrix();
            }
            else
                traverse(node, cv);
        }
    };

    class RainSettingsUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        RainSettingsUpdater() = default;

        void setRainIntensity(float rainIntensity) { mRainIntensity = rainIntensity; }

    protected:
        void setDefaults(osg::StateSet* stateset) override
        {
            osg::ref_ptr<osg::Uniform> rainIntensityUniform = new osg::Uniform("rainIntensity", 0.0f);
            stateset->addUniform(rainIntensityUniform.get());
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* /*nv*/) override
        {
            osg::ref_ptr<osg::Uniform> rainIntensityUniform = stateset->getUniform("rainIntensity");
            if (rainIntensityUniform != nullptr)
                rainIntensityUniform->set(mRainIntensity);
        }

    private:
        float mRainIntensity{ 0.f };
    };

    class Reflection : public SceneUtil::RTTNode
    {
        class CompletionCallback final : public osg::Camera::DrawCallback
        {
        public:
            /// Publish completion only after the render thread finishes the reflection camera.
            explicit CompletionCallback(std::shared_ptr<std::atomic_bool> completed)
                : mCompleted(std::move(completed))
            {
            }

            void operator()(osg::RenderInfo&) const override { mCompleted->store(true, std::memory_order_release); }

        private:
            std::shared_ptr<std::atomic_bool> mCompleted;
        };

    public:
        Reflection(uint32_t rttSize, bool isInterior)
            : RTTNode(rttSize, rttSize, 0, false, 0, StereoAwareness::Aware, shouldAddMSAAIntermediateTarget())
        {
            setInterior(isInterior);
            setDepthBufferInternalFormat(GL_DEPTH32F_STENCIL8);
            mClipCullNode = new ClipCullNode;
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
            camera->setSmallFeatureCullingPixelSize(Settings::water().mSmallFeatureCullingPixelSize);
            camera->setName(std::string(SceneUtil::reflectionCameraName));
            // Define an opaque clear explicitly so newly allocated RTT stages cannot inherit a transparent clear state.
            camera->setClearColor(osg::Vec4f(0.f, 0.f, 0.f, 1.f));
            camera->addCullCallback(new InheritViewPointCallback);
            camera->addFinalDrawCallback(new CompletionCallback(mCompleted));

            // Inform the shader that we're in a reflection
            camera->getOrCreateStateSet()->addUniform(new osg::Uniform("isReflection", true));

            // The reflected view reverses handedness, so define clockwise front faces for this camera.
            osg::ref_ptr<osg::FrontFace> frontFace = new osg::FrontFace(osg::FrontFace::CLOCKWISE);
            camera->getOrCreateStateSet()->setAttributeAndModes(frontFace, osg::StateAttribute::ON);

            camera->addChild(mClipCullNode);
            camera->setNodeMask(Mask_RenderToTexture);

            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
        }

        void apply(osg::Camera* camera, osgUtil::CullVisitor* cv) override
        {
            camera->setViewMatrix(mViewMatrix);
            bool visible = true;
            const unsigned int frameNumber = cv->getFrameStamp()->getFrameNumber();
            if (mVisibilityDriven)
                visible = SceneUtil::waterReflectionShouldRender(mHasRendered, mVisibleFrame, frameNumber);
            camera->setCullMask(visible ? mNodeMask : 0u);
            // RTTNode still submits an inactive camera. Suppress its clear so a visibility handoff preserves the last
            // complete reflection instead of publishing a black or sky-coloured texture to the water draw thread.
            camera->setClearMask(visible ? GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0u);
            if (mVisibilityDriven && visible)
                mHasRendered = true;
            if (mVisibilityDriven && camera->getStats())
            {
                camera->getStats()->setAttribute(
                    frameNumber, std::string(meshWaterReflectionActiveCounter), visible ? 1.0 : 0.0);
                camera->getStats()->setAttribute(
                    frameNumber, std::string(meshWaterReflectionHeightCounter), mWaterPlane.mDistance);
                camera->getStats()->setAttribute(
                    frameNumber, std::string(meshWaterReflectionResolutionCounter), static_cast<double>(width()));
            }
        }

        /// Render this exact plane after a scene-camera traversal observed one of its surfaces this or the prior frame.
        void setVisibilityDriven(bool enabled)
        {
            mVisibilityDriven = enabled;
            mHasRendered = false;
            mCompleted->store(false, std::memory_order_release);
            mNodeMask = calcNodeMask();
        }

        /// Return whether this target has completed at least one reflection-camera draw since acquisition.
        bool hasCompletedRender() const { return mCompleted->load(std::memory_order_acquire); }

        /// Record that at least one surface using this plane survived scene-camera culling.
        void markVisible(unsigned int frameNumber) { mVisibleFrame = frameNumber; }

        void setInterior(bool isInterior)
        {
            mInterior = isInterior;
            mNodeMask = calcNodeMask();
        }

        /// Orient reflection and clipping to one arbitrary planar water surface.
        void setWaterPlane(const SceneUtil::WaterSurfacePlane& plane)
        {
            mWaterPlane = plane;
            mViewMatrix = SceneUtil::waterSurfaceReflectionMatrix(plane);
            const osg::Vec3d normal(plane.mNormal);
            mClipCullNode->setPlane(osg::Plane(normal, normal * static_cast<double>(plane.mDistance)));
        }

        void setScene(osg::Node* scene)
        {
            if (mScene)
                mClipCullNode->removeChild(mScene);
            mScene = scene;
            if (scene)
                mClipCullNode->addChild(scene);
        }

        void showWorld(bool show)
        {
            if (show)
                mNodeMask = calcNodeMask();
            else
                mNodeMask = calcNodeMask() & ~sToggleWorldMask;
        }

    private:
        unsigned int calcNodeMask()
        {
            int reflectionDetail = Settings::water().mReflectionDetail;
            reflectionDetail = std::clamp(reflectionDetail, mInterior ? 2 : 0, 5);
            // Authored surfaces can require several exact planes. Preserve dynamic scene reflections but reserve the
            // groundcover-heavy maximum detail for the single world-water reflection.
            if (mVisibilityDriven)
                reflectionDetail = std::min(reflectionDetail, 4);
            unsigned int extraMask = 0;
            if (reflectionDetail >= 1)
                extraMask |= Mask_Terrain;
            if (reflectionDetail >= 2)
                extraMask |= Mask_Static;
            if (reflectionDetail >= 3)
                extraMask |= Mask_Effect | Mask_ParticleSystem | Mask_Object;
            if (reflectionDetail >= 4)
                extraMask |= Mask_Player | Mask_Actor;
            if (reflectionDetail >= 5)
                extraMask |= Mask_Groundcover;
            return Mask_Scene | Mask_Sky | Mask_Sun | Mask_Lighting | extraMask;
        }

        osg::ref_ptr<ClipCullNode> mClipCullNode;
        osg::ref_ptr<osg::Node> mScene;
        osg::Node::NodeMask mNodeMask;
        osg::Matrix mViewMatrix{ osg::Matrix::identity() };
        bool mInterior;
        bool mVisibilityDriven = false;
        bool mHasRendered = false;
        unsigned int mVisibleFrame = 0;
        SceneUtil::WaterSurfacePlane mWaterPlane;
        std::shared_ptr<std::atomic_bool> mCompleted = std::make_shared<std::atomic_bool>(false);
    };

    /// Own one geometrically exact reflection plane shared by coplanar classified mesh surfaces.
    class MeshWaterPlane
    {
    public:
        MeshWaterPlane(SceneUtil::WaterSurfacePlane plane, unsigned int resolution, osg::ref_ptr<Reflection> reflection)
            : mPlane(std::move(plane))
            , mReflection(std::move(reflection))
            , mReflectionHandoff(resolution)
        {
        }

        const SceneUtil::WaterSurfacePlane& plane() const { return mPlane; }
        const osg::BoundingBox& worldBound() const { return mWorldBound; }
        unsigned int resolution() const { return mReflectionHandoff.activeResolution(); }
        Reflection* reflection() const { return mReflection; }
        unsigned int pendingResolution() const { return mReflectionHandoff.pendingResolution().value_or(0); }
        Reflection* pendingReflection() const { return mPendingReflection; }

        /// Extend the spatial ownership used to join neighboring authored surfaces conservatively.
        void include(const osg::BoundingBox& worldBound) { mWorldBound.expandBy(worldBound); }

        /// Clear stale spatial ownership before rebuilding it after object removal.
        void clearWorldBound() { mWorldBound.init(); }

        void markVisible(unsigned int frameNumber, unsigned int requestedResolution)
        {
            mReflection->markVisible(frameNumber);
            if (mPendingReflection)
                mPendingReflection->markVisible(frameNumber);
            mResolutionPolicy.markVisible(frameNumber, requestedResolution);
        }

        std::optional<unsigned int> consumeRequestedResolution() { return mResolutionPolicy.consume(resolution()); }

        /// Stage a resized reflection while retaining the last complete target for material sampling.
        std::pair<unsigned int, osg::ref_ptr<Reflection>> stageReflection(
            unsigned int resolution, osg::ref_ptr<Reflection> reflection)
        {
            const SceneUtil::WaterReflectionHandoffRequest request = mReflectionHandoff.request(resolution);
            std::pair<unsigned int, osg::ref_ptr<Reflection>> previous(
                request.mReleaseResolution.value_or(0), std::move(mPendingReflection));
            mPendingReflection = std::move(reflection);
            return previous;
        }

        /// Cancel a staged resize and return its target to the caller for release.
        std::pair<unsigned int, osg::ref_ptr<Reflection>> cancelStagedReflection()
        {
            const SceneUtil::WaterReflectionHandoffRequest request
                = mReflectionHandoff.request(mReflectionHandoff.activeResolution());
            std::pair<unsigned int, osg::ref_ptr<Reflection>> cancelled(
                request.mReleaseResolution.value_or(0), std::move(mPendingReflection));
            return cancelled;
        }

        /// Publish a completed staged target and return the prior active target for release.
        std::pair<unsigned int, osg::ref_ptr<Reflection>> publishStagedReflection()
        {
            const std::optional<unsigned int> previousResolution = mReflectionHandoff.publish();
            std::pair<unsigned int, osg::ref_ptr<Reflection>> previous(
                previousResolution.value_or(0), std::move(mReflection));
            mReflection = std::move(mPendingReflection);
            return previous;
        }

        std::size_t mReferenceCount = 0;

    private:
        SceneUtil::WaterSurfacePlane mPlane;
        osg::BoundingBox mWorldBound;
        osg::ref_ptr<Reflection> mReflection;
        osg::ref_ptr<Reflection> mPendingReflection;
        SceneUtil::WaterReflectionResolutionPolicy mResolutionPolicy;
        SceneUtil::WaterReflectionHandoffPolicy mReflectionHandoff;
    };

    /// Feed visible projected coverage into one plane's stable reflection-resolution policy.
    class MeshWaterVisibilityCallback
        : public SceneUtil::NodeCallback<MeshWaterVisibilityCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        MeshWaterVisibilityCallback(MeshWaterPlane* plane, osg::BoundingBox localBound)
            : mPlane(plane)
            , mLocalBound(std::move(localBound))
        {
        }

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
        {
            osg::Camera* camera = cv->getCurrentCamera();
            if (camera && camera->getName() == Constants::SceneCamera && cv->getViewport())
            {
                const osg::Matrixd inverseView = osg::Matrixd::inverse(cv->getCurrentCamera()->getViewMatrix());
                const osg::Matrixd localToWorld = *cv->getModelViewMatrix() * inverseView;
                osg::BoundingBox worldBound;
                for (unsigned int corner = 0; corner < 8; ++corner)
                    worldBound.expandBy(mLocalBound.corner(corner) * localToWorld);
                const SceneUtil::WaterReflectionScreenRect screenRect = SceneUtil::waterReflectionScreenRect(
                    worldBound, cv->getCurrentCamera()->getViewMatrix(), *cv->getProjectionMatrix());
                const double projectedDiameter
                    = screenRect.projectedDiameter(cv->getViewport()->width(), cv->getViewport()->height());
                const unsigned int maximum = std::max(
                    64u, static_cast<unsigned int>(std::max(1, static_cast<int>(Settings::water().mRttSize))));
                const unsigned int requested = SceneUtil::waterReflectionResolution(projectedDiameter, maximum);
                mPlane->markVisible(cv->getFrameStamp()->getFrameNumber(), requested);
            }
            traverse(node, cv);
        }

    private:
        MeshWaterPlane* mPlane;
        osg::BoundingBox mLocalBound;
    };

    /// Retain reversible bindings installed for one scene object containing classified water materials.
    class MeshWaterObject
    {
    public:
        struct Binding
        {
            osg::observer_ptr<osg::Group> mParent;
            osg::observer_ptr<osg::Group> mReplacement;
            osg::observer_ptr<osg::Drawable> mOriginal;

            struct Segment
            {
                osg::observer_ptr<osg::Drawable> mDrawable;
                osg::observer_ptr<osg::Group> mWrapper;
                SceneUtil::WaterSurfacePlaneKey mPlaneKey;
                osg::BoundingBox mWorldBound;
                std::vector<osg::Vec3f> mWorldTriangles;
            };

            SceneUtil::SurfaceEffectProfile mProfile;
            osg::Vec3f mNodePosition;
            std::vector<Segment> mSegments;
        };

        std::vector<Binding> mBindings;
    };

    /// DepthClampCallback enables GL_DEPTH_CLAMP for the current draw, if supported.
    class DepthClampCallback : public osg::Drawable::DrawCallback
    {
    public:
        void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
        {
            static bool supported = osg::isGLExtensionOrVersionSupported(
                renderInfo.getState()->getContextID(), "GL_ARB_depth_clamp", 3.3f);
            if (!supported)
            {
                drawable->drawImplementation(renderInfo);
                return;
            }

            glEnable(GL_DEPTH_CLAMP);

            drawable->drawImplementation(renderInfo);

            // restore default
            glDisable(GL_DEPTH_CLAMP);
        }
    };

    Water::Water(osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem,
        osgUtil::IncrementalCompileOperation* ico, GpuScene* gpuScene)
        : mRainSettingsUpdater(nullptr)
        , mParent(parent)
        , mSceneRoot(sceneRoot)
        , mResourceSystem(resourceSystem)
        , mGpuScene(gpuScene)
        , mSurfaceParameters(defaultWaterSurfaceParameters())
        , mMeshGeometricWaveHeight(Settings::openmwseWater().mMeshGeometricWaveHeight.get())
        , mEnabled(true)
        , mToggled(true)
        , mTop(0)
        , mInterior(false)
        , mShowWorld(true)
        , mCullCallback(nullptr)
        , mShaderWaterStateSetUpdater(nullptr)
    {
        mSimulation = std::make_unique<RippleSimulation>(mSceneRoot, resourceSystem);
        mSimulation->setSurfaceHeightQuery([this](const osg::Vec3f& position, float maximumVerticalDistance) {
            return meshWaterSurfaceHeight(position, maximumVerticalDistance);
        });

        mWaterGeom = SceneUtil::createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900);
        if (mGpuScene == nullptr || !mGpuScene->isEnabled())
            mWaterGeom->setDrawCallback(new DepthClampCallback);
        mWaterGeom->setNodeMask(Mask_Water);
        mWaterGeom->setDataVariance(osg::Object::STATIC);
        mWaterGeom->setName("Water Geometry");

        mWaterNode = new osg::PositionAttitudeTransform;
        mWaterNode->setName("Water Root");
        mWaterNode->addChild(mWaterGeom);
        mWaterNode->addCullCallback(new FudgeCallback);

        mMeshWaterSeamRoot = new osg::Group;
        mMeshWaterSeamRoot->setName("Mesh Water Seam Root");
        mSceneRoot->addChild(mMeshWaterSeamRoot);

        // simple water fallback for the local map
        mSimpleWaterGeom = SceneUtil::createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900);
        createSimpleWaterStateSet(mSimpleWaterGeom, Fallback::Map::getFloat("Water_Map_Alpha"));
        mSimpleWaterGeom->setNodeMask(Mask_SimpleWater);
        mSimpleWaterGeom->setName("Simple Water Geometry");
        mWaterNode->addChild(mSimpleWaterGeom);

        updateWaterGeometry();

        mSceneRoot->addChild(mWaterNode);

        setHeight(mTop);

        updateWaterMaterial();

        if (ico)
            ico->add(mWaterNode);
    }

    void Water::updateWaterGeometry()
    {
        const bool useDisplacedGeometry = Settings::water().mShader && mSurfaceParameters.useGeometricWaves();
        const bool useSurfaceCoverage = Settings::water().mShader && mSurfaceParameters.useSurfaceCoverage();
        if (mUsesDisplacedGeometry != useDisplacedGeometry)
        {
            mWaterNode->removeChild(mWaterGeom);
            mWaterGeom = useDisplacedGeometry
                ? SceneUtil::createDisplacedWaterGeometry(Constants::CellSizeInUnits * 150, 900)
                : SceneUtil::createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900);
            if (mGpuScene == nullptr || !mGpuScene->isEnabled())
                mWaterGeom->setDrawCallback(new DepthClampCallback);
            mWaterGeom->setNodeMask(Mask_Water);
            mWaterGeom->setDataVariance(osg::Object::STATIC);
            mWaterGeom->setName(useDisplacedGeometry ? "Displaced Water Geometry" : "Water Geometry");
            mWaterNode->addChild(mWaterGeom);
            mUsesDisplacedGeometry = useDisplacedGeometry;
        }

        if (mUsesSurfaceCoverage == useSurfaceCoverage)
            return;

        if (useSurfaceCoverage)
        {
            mWaveOcclusion = std::make_unique<WaterWaveOcclusion>(mParent, mSceneRoot, mResourceSystem);
            const osg::Vec3d position = mWaterNode->getPosition();
            mWaveOcclusion->setCenter(osg::Vec2f(static_cast<float>(position.x()), static_cast<float>(position.y())));
            mWaveOcclusion->setWaterHeight(mTop);
            mWaveOcclusion->setMaximumClearance(
                SceneUtil::waterWaveInteractionHeight(mSurfaceParameters.mGeometricWaveHeight));
            mWaveOcclusion->setCausticMaximumClearance(mSurfaceParameters.mOptics.mCaustics
                    ? SceneUtil::waterCausticInteractionHeight
                    : SceneUtil::waterWaveInteractionHeight(mSurfaceParameters.mGeometricWaveHeight));
            mWaveOcclusion->setEnabled(true);
        }
        else if (mWaveOcclusion)
        {
            mWaveOcclusion->setEnabled(false);
            mWaveOcclusion.reset();
        }
        mUsesSurfaceCoverage = useSurfaceCoverage;
    }

    void Water::setCullCallback(osg::Callback* callback)
    {
        if (mCullCallback)
        {
            mWaterNode->removeCullCallback(mCullCallback);
            if (mReflection)
                mReflection->removeCullCallback(mCullCallback);
            for (auto& [_, plane] : mMeshWaterPlanes)
            {
                plane->reflection()->removeCullCallback(mCullCallback);
                if (plane->pendingReflection())
                    plane->pendingReflection()->removeCullCallback(mCullCallback);
            }
        }

        mCullCallback = callback;

        if (callback)
        {
            mWaterNode->addCullCallback(callback);
            if (mReflection)
                mReflection->addCullCallback(callback);
            for (auto& [_, plane] : mMeshWaterPlanes)
            {
                plane->reflection()->addCullCallback(callback);
                if (plane->pendingReflection())
                    plane->pendingReflection()->addCullCallback(callback);
            }
        }
    }

    void Water::updateWaterMaterial()
    {
        if (mShaderWaterStateSetUpdater)
        {
            mWaterNode->removeCullCallback(mShaderWaterStateSetUpdater);
            mShaderWaterStateSetUpdater = nullptr;
        }
        if (mReflection)
        {
            mParent->removeChild(mReflection);
            mReflection = nullptr;
        }
        if (mRipples)
        {
            mParent->removeChild(mRipples);
            mRipples = nullptr;
            mSimulation->setRipples(nullptr);
        }

        mWaterNode->setStateSet(nullptr);
        mWaterGeom->setStateSet(nullptr);
        mWaterGeom->setUpdateCallback(nullptr);

        if (Settings::water().mShader)
        {
            const unsigned int rttSize = Settings::water().mRttSize;

            mReflection = new Reflection(rttSize, mInterior);
            mReflection->setWaterPlane(horizontalWaterPlane(mTop));
            mReflection->setScene(mSceneRoot);
            if (mCullCallback)
                mReflection->addCullCallback(mCullCallback);
            mParent->addChild(mReflection);

            mRipples = new Ripples(mResourceSystem);
            mSimulation->setRipples(mRipples);
            mParent->addChild(mRipples);

            showWorld(mShowWorld);

            createShaderWaterStateSet(mWaterNode);
        }
        else
            createSimpleWaterStateSet(mWaterGeom, Fallback::Map::getFloat("Water_World_Alpha"));

        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(mWaterGeom->getOrCreateStateSet(), true);
        if (mGpuScene != nullptr && mGpuScene->isEnabled())
        {
            mWaterGeom->getOrCreateStateSet()->setMode(GL_DEPTH_CLAMP, osg::StateAttribute::ON);
            mGpuScene->capture(*mWaterGeom);
        }

        updateVisible();
    }

    void Water::updateShaderWaterState()
    {
        if (mShaderWaterStateSetUpdater)
        {
            mWaterNode->removeCullCallback(mShaderWaterStateSetUpdater);
            mShaderWaterStateSetUpdater = nullptr;
        }

        mWaterNode->setStateSet(nullptr);
        mWaterGeom->setStateSet(nullptr);
        mWaterNode->setUpdateCallback(nullptr);
        createShaderWaterStateSet(mWaterNode);
        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(mWaterGeom->getOrCreateStateSet(), true);
        if (mGpuScene != nullptr && mGpuScene->isEnabled())
        {
            mWaterGeom->getOrCreateStateSet()->setMode(GL_DEPTH_CLAMP, osg::StateAttribute::ON);
            mGpuScene->capture(*mWaterGeom);
        }
    }

    osg::Vec3d Water::getPosition() const
    {
        return mWaterNode->getPosition();
    }

    osg::Drawable* Water::getDrawable() const
    {
        return mWaterGeom;
    }

    bool Water::isWaterDrawable(const osg::Drawable* drawable) const
    {
        if (drawable == mWaterGeom)
            return true;
        bool meshWaterSurface = false;
        return drawable && drawable->getUserValue("meshWaterSurface", meshWaterSurface) && meshWaterSurface;
    }

    osg::Texture* Water::getRippleTexture() const
    {
        return mRipples ? mRipples->getColorTexture() : nullptr;
    }

    void Water::createSimpleWaterStateSet(osg::Node* node, float alpha)
    {
        osg::ref_ptr<osg::StateSet> stateset
            = SceneUtil::createSimpleWaterStateSet(alpha, MWRender::RenderBin_DepthSorted);

        node->setStateSet(stateset);
        node->setUpdateCallback(nullptr);
        mRainSettingsUpdater = nullptr;

        // Add animated textures
        std::vector<osg::ref_ptr<osg::Texture2D>> textures;
        const int frameCount = std::clamp(Fallback::Map::getInt("Water_SurfaceFrameCount"), 0, 320);
        std::string_view texture = Fallback::Map::getString("Water_SurfaceTexture");
        for (int i = 0; i < frameCount; ++i)
        {
            std::ostringstream texname;
            texname << "textures/water/" << texture << std::setw(2) << std::setfill('0') << i << ".dds";
            const VFS::Path::Normalized path(texname.str());
            osg::ref_ptr<osg::Texture2D> tex(new osg::Texture2D(mResourceSystem->getImageManager()->getImage(path)));
            tex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            mResourceSystem->getSceneManager()->applyFilterSettings(tex);
            textures.push_back(tex);
        }

        if (textures.empty())
            return;

        float fps = Fallback::Map::getFloat("Water_SurfaceFPS");

        osg::ref_ptr<NifOsg::FlipController> controller(new NifOsg::FlipController(0, 1.f / fps, textures));
        controller->setSource(std::make_shared<SceneUtil::FrameTimeSource>());
        node->setUpdateCallback(controller);

        stateset->setTextureAttribute(0, textures[0], osg::StateAttribute::ON);

        // use a shader to render the simple water, ensuring that fog is applied per pixel as required.
        // this could be removed if a more detailed water mesh, using some sort of paging solution, is implemented.
        Resource::SceneManager* sceneManager = mResourceSystem->getSceneManager();
        sceneManager->recreateShaders(node);
    }

    /// Own the complete shader material contract shared by every rendered water surface.
    class WaterMaterialBindings
    {
    public:
        /// Capture the shared program, optical textures, and reserved scene-texture units used by all water geometry.
        WaterMaterialBindings(const Water* water, Resource::ResourceSystem* resourceSystem,
            osg::ref_ptr<osg::Program> program, osg::ref_ptr<osg::Texture2D> normalMap,
            osg::ref_ptr<osg::Texture3D> mgeWaterVolume)
            : mWater(water)
            , mResourceSystem(resourceSystem)
            , mProgram(std::move(program))
            , mNormalMap(std::move(normalMap))
            , mMgeWaterVolume(std::move(mgeWaterVolume))
            , mOpaqueDepthTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::OpaqueDepthTexture))
            , mOpaqueColorTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::OpaqueColorTexture))
            , mWaterOpticalTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::WaterOptics, Shader::waterOpticsTextureUnitCount))
        {
        }

        /// Install the invariant textures, uniforms, shader, blending, and depth state for one water surface.
        void setDefaults(osg::StateSet& stateset) const
        {
            stateset.setTextureAttribute(0, mNormalMap, osg::StateAttribute::ON);
            stateset.addUniform(new osg::Uniform("normalMap", 0));
            stateset.setTextureAttribute(6, mMgeWaterVolume, osg::StateAttribute::ON);
            stateset.addUniform(new osg::Uniform("mgeWaterVolume", 6));
            stateset.setTextureAttribute(mWaterOpticalTextureUnit, mNormalMap, osg::StateAttribute::ON);
            stateset.addUniform(new osg::Uniform("waterOpticalNormalMap", mWaterOpticalTextureUnit));
            stateset.setTextureAttribute(mWaterOpticalTextureUnit + 1, mMgeWaterVolume, osg::StateAttribute::ON);
            stateset.addUniform(new osg::Uniform("waterOpticalVolume", mWaterOpticalTextureUnit + 1));
            stateset.setAttributeAndModes(mProgram, osg::StateAttribute::ON);
            stateset.addUniform(new osg::Uniform("reflectionMap", 1));
            stateset.addUniform(new osg::Uniform("opaqueColorTex", mOpaqueColorTextureUnit));
            stateset.addUniform(new osg::Uniform("opaqueDepthTex", mOpaqueDepthTextureUnit));
            stateset.addUniform(new osg::Uniform("rippleMap", 4));
            stateset.setMode(GL_BLEND, osg::StateAttribute::ON);
            stateset.addUniform(new osg::Uniform("waterSurface", true));
            stateset.addUniform(new osg::Uniform(
                "waterAppearance", shaderAppearance(mWater->getSurfaceParameters().mOptics.mAppearance)));
            stateset.addUniform(new osg::Uniform(
                "enhancedWaterColor", shaderEnhancedColor(mWater->getSurfaceParameters().mOptics.mEnhancedColor)));
            stateset.addUniform(
                new osg::Uniform("enhancedWaterFoam", mWater->getSurfaceParameters().mOptics.mEnhancedFoam));
            stateset.addUniform(new osg::Uniform(
                "enhancedWaterDistantWaves", mWater->getSurfaceParameters().mOptics.mEnhancedDistantWaves));
            osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
            depth->setWriteMask(false);
            stateset.setAttributeAndModes(depth, osg::StateAttribute::ON);
        }

        /// Bind frame-local render targets and live appearance controls for one water surface.
        void apply(osg::StateSet& stateset, osgUtil::CullVisitor& cv, Reflection& reflection) const
        {
            const unsigned int frameNumber = cv.getFrameStamp()->getFrameNumber();
            stateset.setTextureAttribute(0, mNormalMap, osg::StateAttribute::ON);
            stateset.setTextureAttribute(1, reflection.getColorTexture(&cv), osg::StateAttribute::ON);
            stateset.setTextureAttribute(6, mMgeWaterVolume, osg::StateAttribute::ON);
            stateset.setTextureAttribute(mWaterOpticalTextureUnit, mNormalMap, osg::StateAttribute::ON);
            stateset.setTextureAttribute(mWaterOpticalTextureUnit + 1, mMgeWaterVolume, osg::StateAttribute::ON);
            stateset.setTextureAttribute(mOpaqueColorTextureUnit,
                mResourceSystem->getSceneManager()->getOpaqueColorTex(frameNumber), osg::StateAttribute::ON);
            stateset.setTextureAttribute(mOpaqueDepthTextureUnit,
                mResourceSystem->getSceneManager()->getOpaqueDepthTex(frameNumber), osg::StateAttribute::ON);
            if (osg::Texture* rippleTexture = mWater->getRippleTexture())
                stateset.setTextureAttribute(4, rippleTexture, osg::StateAttribute::ON);
            stateset.getUniform("waterAppearance")
                ->set(shaderAppearance(mWater->getSurfaceParameters().mOptics.mAppearance));
            stateset.getUniform("enhancedWaterColor")
                ->set(shaderEnhancedColor(mWater->getSurfaceParameters().mOptics.mEnhancedColor));
            stateset.getUniform("enhancedWaterFoam")->set(mWater->getSurfaceParameters().mOptics.mEnhancedFoam);
            stateset.getUniform("enhancedWaterDistantWaves")
                ->set(mWater->getSurfaceParameters().mOptics.mEnhancedDistantWaves);
        }

    private:
        const Water* mWater;
        Resource::ResourceSystem* mResourceSystem;
        osg::ref_ptr<osg::Program> mProgram;
        osg::ref_ptr<osg::Texture2D> mNormalMap;
        osg::ref_ptr<osg::Texture3D> mMgeWaterVolume;
        int mOpaqueDepthTextureUnit;
        int mOpaqueColorTextureUnit;
        int mWaterOpticalTextureUnit;
    };

    /// Bind shared water material state and world-plane-specific geometry controls.
    class ShaderWaterStateSetUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        /// Construct the updater for the conventional world water plane and its reflection camera.
        ShaderWaterStateSetUpdater(Water* water, Resource::ResourceSystem* resourceSystem, Reflection* reflection,
            osg::ref_ptr<osg::Program> program, osg::ref_ptr<osg::Texture2D> normalMap,
            osg::ref_ptr<osg::Texture3D> mgeWaterVolume)
            : mWater(water)
            , mReflection(reflection)
            , mMaterial(water, resourceSystem, std::move(program), std::move(normalMap), std::move(mgeWaterVolume))
        {
        }

        /// Install the shared material plus conventional world-plane render state.
        void setDefaults(osg::StateSet* stateset) override
        {
            mMaterial.setDefaults(*stateset);
            stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            stateset->setRenderBinDetails(MWRender::RenderBin_DepthSorted, "DepthSortedBin");
            stateset->addUniform(new osg::Uniform("nodePosition", osg::Vec3f(mWater->getPosition())));
            stateset->addUniform(
                new osg::Uniform("geometricWaveHeight", mWater->getSurfaceParameters().mGeometricWaveHeight));
        }

        /// Refresh live material controls and world-plane geometry inputs during cull traversal.
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
            mMaterial.apply(*stateset, *cv, *mReflection);
            stateset->getUniform("nodePosition")->set(osg::Vec3f(mWater->getPosition()));
            stateset->getUniform("geometricWaveHeight")->set(mWater->getSurfaceParameters().mGeometricWaveHeight);
        }

    private:
        Water* mWater;
        Reflection* mReflection;
        WaterMaterialBindings mMaterial;
    };

    /// Bind shared water optics and the exact reflection plane for one authored mesh-water scope.
    class MeshWaterStateSetUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        /// Construct the updater for one finite authored surface and its dedicated reflection plane.
        MeshWaterStateSetUpdater(const Water* water, Resource::ResourceSystem* resourceSystem, MeshWaterPlane* plane,
            SceneUtil::SurfaceEffectProfile profile, osg::Vec3f nodePosition, osg::ref_ptr<osg::Program> program,
            osg::ref_ptr<osg::Texture2D> normalMap, osg::ref_ptr<osg::Texture3D> mgeWaterVolume)
            : mWater(water)
            , mPlane(plane)
            , mProfile(std::move(profile))
            , mNodePosition(nodePosition)
            , mMaterial(water, resourceSystem, std::move(program), std::move(normalMap), std::move(mgeWaterVolume))
        {
        }

    protected:
        /// Install the shared material plus finite-surface culling, height, and render-bin state.
        void setDefaults(osg::StateSet* stateset) override
        {
            mMaterial.setDefaults(*stateset);
            osg::ref_ptr<osg::CullFace> cullFace = new osg::CullFace(osg::CullFace::BACK);
            stateset->setAttributeAndModes(cullFace, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset->addUniform(new osg::Uniform("nodePosition", mNodePosition));
            stateset->addUniform(new osg::Uniform("waterPlaneNormal", mPlane->plane().mNormal));
            stateset->addUniform(new osg::Uniform("waterPlaneDistance", mPlane->plane().mDistance));
            stateset->addUniform(new osg::Uniform("waterPlaneTangent", mPlane->plane().mTangent));
            stateset->addUniform(new osg::Uniform("waterPlaneBitangent", mPlane->plane().mBitangent));
            stateset->addUniform(new osg::Uniform("geometricWaveHeight", meshWaterWaveHeight(mProfile)));
            stateset->addUniform(new osg::Uniform("rainIntensity", mWater->getRainIntensity()));
            // Replacement surfaces must join the scene water bin even when the source NIF contributes an alpha bin.
            stateset->setNestRenderBins(false);
            stateset->setRenderBinDetails(
                MWRender::RenderBin_DepthSorted, "DepthSortedBin", osg::StateSet::OVERRIDE_PROTECTED_RENDERBIN_DETAILS);
        }

        /// Refresh live material controls and the authored surface's plane-specific inputs.
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
            mMaterial.apply(*stateset, *cv, *mPlane->reflection());
            stateset->getUniform("rainIntensity")->set(mWater->getRainIntensity());
            stateset->getUniform("geometricWaveHeight")->set(meshWaterWaveHeight(mProfile));
        }

    private:
        const Water* mWater;
        MeshWaterPlane* mPlane;
        SceneUtil::SurfaceEffectProfile mProfile;
        osg::Vec3f mNodePosition;
        WaterMaterialBindings mMaterial;
    };

    void Water::createShaderWaterStateSet(osg::Node* node)
    {
        const std::map<std::string, std::string> defineMap
            = waterShaderDefines(mSurfaceParameters.useGeometricWaves(), false);

        Shader::ShaderManager& shaderMgr = mResourceSystem->getSceneManager()->getShaderManager();
        osg::ref_ptr<osg::Program> program
            = shaderMgr.getProgram("water", defineMap, nullptr, {}, Shader::ProgramCapability::Instanced);

        constexpr VFS::Path::NormalizedView waterImage("textures/omw/water_nm.png");
        if (!mWaterNormalMap)
        {
            mWaterNormalMap = new osg::Texture2D(mResourceSystem->getImageManager()->getImage(waterImage));
            mWaterNormalMap->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            mWaterNormalMap->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            mResourceSystem->getSceneManager()->applyFilterSettings(mWaterNormalMap);
        }

        osg::ref_ptr<osg::Texture3D> normalVolume(new osg::Texture3D(mWaterNormalMap->getImage()));
        normalVolume->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        normalVolume->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        normalVolume->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
        normalVolume->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        normalVolume->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

        constexpr VFS::Path::NormalizedView mgeWaterImage("textures/openmwse/mge_xe_water_nrm.dds");
        if (!mMgeWaterVolume)
        {
            mMgeWaterVolume = new osg::Texture3D(mResourceSystem->getImageManager()->getImage(mgeWaterImage, true));
            mMgeWaterVolume->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            mMgeWaterVolume->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            mMgeWaterVolume->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);
            mMgeWaterVolume->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            mMgeWaterVolume->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        }

        const int waterOpticalTextureUnit = shaderMgr.reserveGlobalTextureUnits(
            Shader::ShaderManager::Slot::WaterOptics, Shader::waterOpticsTextureUnitCount);
        osg::StateSet* sceneState = mSceneRoot->getOrCreateStateSet();
        sceneState->addUniform(new osg::Uniform("waterOpticalNormalMap", waterOpticalTextureUnit));
        sceneState->setTextureAttribute(waterOpticalTextureUnit, normalVolume, osg::StateAttribute::ON);
        sceneState->addUniform(new osg::Uniform("waterOpticalVolume", waterOpticalTextureUnit + 1));
        sceneState->setTextureAttribute(waterOpticalTextureUnit + 1, mMgeWaterVolume, osg::StateAttribute::ON);
        if (mWaveOcclusion)
        {
            sceneState->addUniform(new osg::Uniform("waterSurfaceCoverageMap", waterOpticalTextureUnit + 2));
            sceneState->setTextureAttribute(
                waterOpticalTextureUnit + 2, mWaveOcclusion->texture(), osg::StateAttribute::ON);
            sceneState->addUniform(new osg::Uniform("waterSurfaceCoverageCenter", mWaveOcclusion->center()));
            sceneState->addUniform(
                new osg::Uniform("waterSurfaceCoverageInverseSize", mWaveOcclusion->inverseWorldSize()));
            sceneState->addUniform(
                new osg::Uniform("waterSurfaceCoverageResolution", mWaveOcclusion->textureResolution()));
            sceneState->addUniform(
                new osg::Uniform("waterCausticMaximumClearance", SceneUtil::waterCausticInteractionHeight));
        }

        mRainSettingsUpdater = new RainSettingsUpdater();
        node->setUpdateCallback(mRainSettingsUpdater);

        mShaderWaterStateSetUpdater = new ShaderWaterStateSetUpdater(
            this, mResourceSystem, mReflection, std::move(program), mWaterNormalMap, mMgeWaterVolume);
        node->addCullCallback(mShaderWaterStateSetUpdater);
    }

    void Water::createMeshWaterStateSet(osg::Group& wrapper, Reflection& reflection,
        const SceneUtil::SurfaceEffectProfile& profile, const osg::Vec3f& nodePosition)
    {
        const std::map<std::string, std::string> defineMap
            = waterShaderDefines(meshWaterWaveHeight(profile) > 0.f, true);

        Shader::ShaderManager& shaderManager = mResourceSystem->getSceneManager()->getShaderManager();
        osg::ref_ptr<osg::Program> program
            = shaderManager.getProgram("water", defineMap, nullptr, {}, Shader::ProgramCapability::Instanced);

        if (!mWaterNormalMap || !mMgeWaterVolume)
            createShaderWaterStateSet(mWaterNode);

        MeshWaterPlane* plane = nullptr;
        for (auto& [_, candidate] : mMeshWaterPlanes)
            if (candidate->reflection() == &reflection)
                plane = candidate.get();
        if (!plane)
            throw std::logic_error("mesh water reflection has no owning plane");

        osg::ref_ptr<MeshWaterStateSetUpdater> updater = new MeshWaterStateSetUpdater(
            this, mResourceSystem, plane, profile, nodePosition, program, mWaterNormalMap, mMgeWaterVolume);
        wrapper.addCullCallback(updater);
    }

    void Water::resizeMeshWaterPlane(MeshWaterPlane& plane, unsigned int resolution)
    {
        if (resolution == plane.resolution())
        {
            auto [cancelledResolution, cancelled] = plane.cancelStagedReflection();
            releaseMeshWaterReflection(cancelledResolution, std::move(cancelled));
            return;
        }
        if (resolution == plane.pendingResolution())
            return;

        osg::ref_ptr<Reflection> reflection = acquireMeshWaterReflection(resolution, plane.plane());
        auto [replacedResolution, replaced] = plane.stageReflection(resolution, std::move(reflection));
        releaseMeshWaterReflection(replacedResolution, std::move(replaced));
        Log(Debug::Info) << "Staged mesh-water reflection for plane distance " << plane.plane().mDistance << " at "
                         << resolution << "px while retaining the complete " << plane.resolution() << "px target";
    }

    osg::ref_ptr<Reflection> Water::acquireMeshWaterReflection(
        unsigned int resolution, const SceneUtil::WaterSurfacePlane& plane)
    {
        osg::ref_ptr<Reflection> reflection;
        auto poolIt = mMeshWaterReflectionPool.find(resolution);
        if (poolIt != mMeshWaterReflectionPool.end() && !poolIt->second.empty())
        {
            reflection = std::move(poolIt->second.back());
            poolIt->second.pop_back();
        }
        else
            reflection = new Reflection(resolution, mInterior);

        reflection->setInterior(mInterior);
        reflection->setVisibilityDriven(true);
        reflection->setWaterPlane(plane);
        reflection->setScene(mSceneRoot);
        reflection->showWorld(mShowWorld);
        if (mCullCallback)
            reflection->addCullCallback(mCullCallback);
        reflection->setNodeMask(isVisible() ? Mask_RenderToTexture : 0u);
        mParent->addChild(reflection);
        return reflection;
    }

    void Water::releaseMeshWaterReflection(unsigned int resolution, osg::ref_ptr<Reflection> reflection)
    {
        if (!reflection)
            return;
        if (mCullCallback)
            reflection->removeCullCallback(mCullCallback);
        mParent->removeChild(reflection);
        reflection->setNodeMask(0u);
        reflection->setScene(nullptr);
        std::vector<osg::ref_ptr<Reflection>>& pool = mMeshWaterReflectionPool[resolution];
        if (pool.empty())
            pool.push_back(std::move(reflection));
    }

    namespace
    {
        struct ClassifiedWaterDrawable
        {
            struct PlaneSegment
            {
                osg::ref_ptr<osg::Drawable> mDrawable;
                osg::BoundingBox mWorldBound;
                SceneUtil::WaterSurfacePlane mPlane;
                SceneUtil::WaterSurfacePlaneKey mPlaneKey;
                std::vector<osg::Vec3f> mWorldTriangles;
            };

            osg::ref_ptr<osg::Drawable> mOriginal;
            osg::observer_ptr<osg::Group> mParent;
            SceneUtil::SurfaceEffectProfile mProfile;
            osg::Vec3f mNodePosition;
            std::vector<PlaneSegment> mSegments;
        };

        /// Collect leaf geometry by resolving the nearest classified material scope on its node path.
        class ClassifiedWaterVisitor : public osg::NodeVisitor
        {
        public:
            ClassifiedWaterVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override { collect(drawable); }

            void apply(osg::Geometry& geometry) override { collect(geometry); }

            std::vector<ClassifiedWaterDrawable> mDrawables;

        private:
            void collect(osg::Drawable& drawable)
            {
                std::optional<SceneUtil::SurfaceEffectProfile> profile;
                const auto resolveProfile = [&](const osg::Object& object) {
                    const osg::UserDataContainer* userData = object.getUserDataContainer();
                    if (const auto* effect = userData
                            ? dynamic_cast<const SceneUtil::SurfaceEffect*>(userData->getUserObject("surfaceEffect"))
                            : nullptr)
                        profile = effect->profile();
                };
                resolveProfile(drawable);
                if (!profile && drawable.getStateSet())
                    resolveProfile(*drawable.getStateSet());
                const osg::NodePath& path = getNodePath();
                for (auto it = path.rbegin(); it != path.rend() && !profile; ++it)
                {
                    resolveProfile(**it);
                    if (!profile && (*it)->getStateSet())
                        resolveProfile(*(*it)->getStateSet());
                }
                if (!profile)
                    return;

                osg::Group* parent = path.size() >= 2 ? path[path.size() - 2]->asGroup() : nullptr;
                if (!parent)
                    return;

                const osg::Matrixd localToWorld = osg::computeLocalToWorld(path);
                const auto* geometry = drawable.asGeometry();
                const auto* vertices
                    = geometry ? dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray()) : nullptr;
                if (!geometry || !vertices)
                    return;

                struct PlaneGeometry
                {
                    osg::ref_ptr<osg::DrawElementsUInt> mIndices
                        = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
                    SceneUtil::WaterSurfacePlane mPlane;
                    osg::BoundingBox mWorldBound;
                    std::vector<osg::Vec3f> mWorldTriangles;
                    osg::ref_ptr<osg::Vec3Array> mDisplacedVertices = new osg::Vec3Array;
                    osg::Vec3f mLocalNormal;
                };
                struct Triangle
                {
                    std::array<unsigned int, 3> mIndices;
                    std::array<osg::Vec3f, 3> mWorldVertices;
                };
                std::vector<Triangle> triangles;
                std::vector<PlaneGeometry> planes;
                bool containsUnsupportedGeometry = false;
                for (unsigned int primitiveIndex = 0; primitiveIndex < geometry->getNumPrimitiveSets();
                     ++primitiveIndex)
                {
                    const osg::PrimitiveSet& primitives = *geometry->getPrimitiveSet(primitiveIndex);
                    const std::vector<std::array<unsigned int, 3>> primitiveTriangles
                        = SceneUtil::triangulateWaterSurfacePrimitives(primitives);
                    if (primitiveTriangles.empty() && primitives.getNumIndices() >= 3)
                    {
                        containsUnsupportedGeometry = true;
                        continue;
                    }
                    for (const std::array<unsigned int, 3>& indices : primitiveTriangles)
                    {
                        const unsigned int aIndex = indices[0];
                        const unsigned int bIndex = indices[1];
                        const unsigned int cIndex = indices[2];
                        if (aIndex >= vertices->size() || bIndex >= vertices->size() || cIndex >= vertices->size())
                        {
                            containsUnsupportedGeometry = true;
                            continue;
                        }
                        triangles.push_back({ { aIndex, bIndex, cIndex },
                            { (*vertices)[aIndex] * localToWorld, (*vertices)[bIndex] * localToWorld,
                                (*vertices)[cIndex] * localToWorld } });
                    }
                }

                std::vector<osg::Vec3f> triangleVertices;
                triangleVertices.reserve(triangles.size() * 3);
                for (const Triangle& triangle : triangles)
                    triangleVertices.insert(
                        triangleVertices.end(), triangle.mWorldVertices.begin(), triangle.mWorldVertices.end());
                constexpr float maximumPlaneDeviation = 2.f;
                constexpr float minimumUpwardNormal = 0.1f;
                constexpr float connectivityTolerance = 0.01f;
                const SceneUtil::WaterSurfacePartition partition = SceneUtil::partitionWaterSurfaceTriangles(
                    triangleVertices, maximumPlaneDeviation, minimumUpwardNormal, connectivityTolerance);
                // One classified drawable is one authored material surface. If any connected component bends,
                // becomes vertical, or uses unsupported topology, retain the complete authored surface instead of
                // replacing only convenient facets of waterfalls and rapids.
                if (containsUnsupportedGeometry || !partition.mResidualTriangleIndices.empty())
                {
                    Log(Debug::Verbose) << "Retained non-planar classified surface '" << drawable.getName()
                                        << "' as authored geometry";
                    return;
                }

                for (const SceneUtil::WaterSurfacePlanarPatch& patch : partition.mPlanarPatches)
                {
                    PlaneGeometry plane;
                    plane.mPlane = patch.mPlane;
                    for (const std::size_t triangleIndex : patch.mTriangleIndices)
                    {
                        const Triangle& triangle = triangles[triangleIndex];
                        const bool reverseWinding
                            = SceneUtil::waterSurfaceTriangleNeedsReversedWinding(triangle.mWorldVertices[0],
                                triangle.mWorldVertices[1], triangle.mWorldVertices[2], plane.mPlane);
                        plane.mIndices->push_back(triangle.mIndices[0]);
                        plane.mIndices->push_back(triangle.mIndices[reverseWinding ? 2 : 1]);
                        plane.mIndices->push_back(triangle.mIndices[reverseWinding ? 1 : 2]);
                        for (const osg::Vec3f& vertex : triangle.mWorldVertices)
                            plane.mWorldBound.expandBy(vertex);
                        plane.mWorldTriangles.insert(plane.mWorldTriangles.end(), triangle.mWorldVertices.begin(),
                            triangle.mWorldVertices.end());
                        if (meshWaterWaveHeight(*profile) > 0.f)
                        {
                            const unsigned int a = triangle.mIndices[0];
                            const unsigned int b = triangle.mIndices[reverseWinding ? 2 : 1];
                            const unsigned int c = triangle.mIndices[reverseWinding ? 1 : 2];
                            const unsigned int worldB = reverseWinding ? 2 : 1;
                            const unsigned int worldC = reverseWinding ? 1 : 2;
                            appendWaterTriangle(*plane.mDisplacedVertices, (*vertices)[a], (*vertices)[b],
                                (*vertices)[c], triangle.mWorldVertices[0], triangle.mWorldVertices[worldB],
                                triangle.mWorldVertices[worldC]);
                            if (plane.mLocalNormal.length2() == 0.f)
                            {
                                plane.mLocalNormal
                                    = ((*vertices)[b] - (*vertices)[a]) ^ ((*vertices)[c] - (*vertices)[a]);
                                plane.mLocalNormal.normalize();
                            }
                        }
                    }
                    planes.push_back(std::move(plane));
                }

                ClassifiedWaterDrawable classified;
                classified.mOriginal = &drawable;
                classified.mParent = parent;
                classified.mProfile = *profile;
                classified.mNodePosition = osg::Vec3f(localToWorld.getTrans());
                for (PlaneGeometry& plane : planes)
                {
                    osg::ref_ptr<osg::Geometry> segment;
                    if (!plane.mDisplacedVertices->empty())
                    {
                        segment = new osg::Geometry;
                        segment->setUseDisplayList(false);
                        segment->setUseVertexBufferObjects(true);
                        segment->setVertexArray(plane.mDisplacedVertices);
                        osg::ref_ptr<osg::Vec3Array> normal = new osg::Vec3Array;
                        normal->push_back(plane.mLocalNormal);
                        segment->setNormalArray(normal, osg::Array::BIND_OVERALL);
                        osg::ref_ptr<osg::DrawElementsUInt> indices
                            = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
                        indices->reserve(plane.mDisplacedVertices->size());
                        for (unsigned int index = 0; index < plane.mDisplacedVertices->size(); ++index)
                            indices->push_back(index);
                        segment->addPrimitiveSet(indices);
                        osg::BoundingBox displacedBound;
                        for (const osg::Vec3f& vertex : *plane.mDisplacedVertices)
                            displacedBound.expandBy(vertex);
                        const float boundMargin = meshWaterWaveHeight(*profile);
                        displacedBound.expandBy(osg::Vec3f(displacedBound.xMin() - boundMargin,
                            displacedBound.yMin() - boundMargin, displacedBound.zMin() - boundMargin));
                        displacedBound.expandBy(osg::Vec3f(displacedBound.xMax() + boundMargin,
                            displacedBound.yMax() + boundMargin, displacedBound.zMax() + boundMargin));
                        segment->setInitialBound(displacedBound);
                    }
                    else
                    {
                        segment = new osg::Geometry(*geometry, osg::CopyOp::SHALLOW_COPY);
                        segment->removePrimitiveSet(0, segment->getNumPrimitiveSets());
                        segment->addPrimitiveSet(plane.mIndices);
                    }
                    segment->setName(drawable.getName());
                    classified.mSegments.push_back({ std::move(segment), plane.mWorldBound, plane.mPlane,
                        SceneUtil::waterSurfacePlaneKey(plane.mPlane), std::move(plane.mWorldTriangles) });
                }
                if (!classified.mSegments.empty())
                    mDrawables.push_back(std::move(classified));
                if (planes.empty())
                    Log(Debug::Warning) << "Classified surface '" << drawable.getName()
                                        << "' contains no connected planar water components";
            }
        };

    }

    void Water::registerSurfaceEffects(osg::Node* objectRoot)
    {
        if (!objectRoot)
            return;
        mKnownSurfaceEffectObjects.insert(objectRoot);
        if (!Settings::water().mShader || mMeshWaterObjects.contains(objectRoot))
            return;

        ClassifiedWaterVisitor visitor;
        objectRoot->accept(visitor);
        if (visitor.mDrawables.empty())
        {
            bool hasSurfaceEffects = false;
            objectRoot->getUserValue("surfaceEffects", hasSurfaceEffects);
            if (hasSurfaceEffects)
                Log(Debug::Warning) << "Classified surface-effect object contains no eligible planar geometry";
            return;
        }

        auto object = std::make_unique<MeshWaterObject>();

        struct CanonicalSurface
        {
            const ClassifiedWaterDrawable::PlaneSegment* mSegment;
            SceneUtil::WaterSurfacePlaneKey mPlaneKey;
        };
        std::vector<const ClassifiedWaterDrawable::PlaneSegment*> orderedSegments;
        for (const ClassifiedWaterDrawable& candidate : visitor.mDrawables)
            for (const ClassifiedWaterDrawable::PlaneSegment& segment : candidate.mSegments)
                orderedSegments.push_back(&segment);
        // Authored animated water commonly uses a visible upper sheet over a helper sheet with the same footprint.
        // Prefer the upper sheet so collapsing the pair cannot bury the replacement below surrounding architecture.
        std::ranges::sort(
            orderedSegments, std::greater<>{}, [](const auto* segment) { return segment->mPlane.mDistance; });

        std::vector<CanonicalSurface> canonicalSurfaces;
        std::map<const ClassifiedWaterDrawable::PlaneSegment*, SceneUtil::WaterSurfacePlaneKey> canonicalPlaneKeys;
        std::unordered_set<const ClassifiedWaterDrawable::PlaneSegment*> duplicateLayers;
        for (const ClassifiedWaterDrawable::PlaneSegment* segment : orderedSegments)
        {
            const auto canonical = std::ranges::find_if(canonicalSurfaces, [&](const CanonicalSurface& candidate) {
                return SceneUtil::waterSurfacePlanesCompatible(segment->mPlane, candidate.mSegment->mPlane, 0.01f,
                           SceneUtil::waterMaterialLayerHeightTolerance)
                    && SceneUtil::waterSurfaceLayersCoincide(segment->mWorldBound, segment->mPlane.mDistance,
                        candidate.mSegment->mWorldBound, candidate.mSegment->mPlane.mDistance);
            });
            if (canonical == canonicalSurfaces.end())
            {
                canonicalSurfaces.push_back({ segment, segment->mPlaneKey });
                canonicalPlaneKeys.emplace(segment, segment->mPlaneKey);
            }
            else
            {
                canonicalPlaneKeys.emplace(segment, canonical->mPlaneKey);
                duplicateLayers.insert(segment);
            }
        }

        for (const ClassifiedWaterDrawable& candidate : visitor.mDrawables)
        {
            osg::ref_ptr<osg::Group> replacement = new osg::Group;
            replacement->setName("Mesh Water Object: " + candidate.mProfile.mId);
            if (!candidate.mParent->replaceChild(candidate.mOriginal, replacement))
                continue;

            MeshWaterObject::Binding binding;
            binding.mParent = candidate.mParent;
            binding.mReplacement = replacement;
            binding.mOriginal = candidate.mOriginal;
            binding.mProfile = candidate.mProfile;
            binding.mNodePosition = candidate.mNodePosition;

            for (const ClassifiedWaterDrawable::PlaneSegment& segment : candidate.mSegments)
            {
                if (duplicateLayers.contains(&segment))
                    continue;

                SceneUtil::WaterSurfacePlaneKey planeKey = canonicalPlaneKeys.at(&segment);
                const unsigned int maximum = std::max(
                    64u, static_cast<unsigned int>(std::max(1, static_cast<int>(Settings::water().mRttSize))));
                const unsigned int resolution = std::min(128u, maximum);

                auto planeIt = mMeshWaterPlanes.find(planeKey);
                if (planeIt == mMeshWaterPlanes.end())
                {
                    planeIt = std::ranges::find_if(mMeshWaterPlanes, [&](const auto& candidate) {
                        return SceneUtil::waterSurfacePlanesSpatiallyCompatible(segment.mPlane, segment.mWorldBound,
                            candidate.second->plane(), candidate.second->worldBound(), 0.01f, 2.f,
                            SceneUtil::waterSurfaceInteractionTolerance);
                    });
                    if (planeIt != mMeshWaterPlanes.end())
                        planeKey = planeIt->first;
                }
                if (planeIt == mMeshWaterPlanes.end())
                {
                    osg::ref_ptr<Reflection> reflection = acquireMeshWaterReflection(resolution, segment.mPlane);
                    planeIt
                        = mMeshWaterPlanes
                              .emplace(planeKey,
                                  std::make_unique<MeshWaterPlane>(segment.mPlane, resolution, std::move(reflection)))
                              .first;
                }

                MeshWaterPlane& plane = *planeIt->second;
                plane.include(segment.mWorldBound);
                ++plane.mReferenceCount;

                osg::ref_ptr<osg::Group> wrapper = new osg::Group;
                wrapper->setName("Mesh Water: " + candidate.mProfile.mId);
                // Use the renderer-owned water mask so planar reflections exclude other water surfaces just as they do
                // for the world water plane. This prevents recursive, frame-stale reflections between authored tiers.
                wrapper->setNodeMask(Mask_Water);
                replacement->addChild(wrapper);
                wrapper->addChild(segment.mDrawable);
                segment.mDrawable->setUpdateCallback(nullptr);
                segment.mDrawable->setStateSet(nullptr);
                segment.mDrawable->setNodeMask(~0u);
                segment.mDrawable->setUserValue("meshWaterSurface", true);
                createMeshWaterStateSet(*wrapper, *plane.reflection(), candidate.mProfile, candidate.mNodePosition);
                wrapper->addCullCallback(new MeshWaterVisibilityCallback(&plane, segment.mDrawable->getBoundingBox()));
                binding.mSegments.push_back(
                    { segment.mDrawable, wrapper, planeKey, segment.mWorldBound, segment.mWorldTriangles });

                Log(Debug::Info) << "Registered mesh water profile '" << candidate.mProfile.mId << "' drawable '"
                                 << segment.mDrawable->getName() << "' on plane normal " << plane.plane().mNormal
                                 << " distance " << plane.plane().mDistance << " around "
                                 << segment.mWorldBound.center() << " with " << plane.resolution()
                                 << "px reflection and " << meshWaterWaveHeight(candidate.mProfile) << "-unit waves";
            }

            object->mBindings.push_back(std::move(binding));
        }

        if (!object->mBindings.empty())
        {
            mMeshWaterObjects.emplace(objectRoot, std::move(object));
            mMeshWaterSeamRebuildDelay = 2;
        }
    }

    void Water::unregisterSurfaceEffects(const osg::Node* objectRoot)
    {
        mKnownSurfaceEffectObjects.erase(objectRoot);
        removeSurfaceEffectBindings(objectRoot);
    }

    void Water::synchronizeSurfaceEffects(osg::Node* objectRoot)
    {
        if (!objectRoot)
            return;

        if (mKnownSurfaceEffectObjects.contains(objectRoot))
            removeSurfaceEffectBindings(objectRoot);
        else
        {
            bool hasSurfaceEffects = false;
            objectRoot->getUserValue("surfaceEffects", hasSurfaceEffects);
            if (!hasSurfaceEffects)
                return;
        }
        registerSurfaceEffects(objectRoot);
    }

    std::optional<float> Water::meshWaterSurfaceHeight(const osg::Vec3f& position, float maximumVerticalDistance) const
    {
        if (!isVisible() || !Settings::water().mShader)
            return std::nullopt;

        constexpr float aboveSurfaceTolerance = 20.f;
        const osg::Vec2f horizontalPosition(position.x(), position.y());
        std::optional<float> nearestHeight;
        float nearestDistance = std::max(aboveSurfaceTolerance, maximumVerticalDistance);
        for (const auto& [_, object] : mMeshWaterObjects)
        {
            for (const MeshWaterObject::Binding& binding : object->mBindings)
            {
                for (const MeshWaterObject::Binding::Segment& segment : binding.mSegments)
                {
                    const auto planeIt = mMeshWaterPlanes.find(segment.mPlaneKey);
                    if (planeIt == mMeshWaterPlanes.end())
                        continue;
                    const SceneUtil::WaterSurfacePlane& plane = planeIt->second->plane();
                    const float height
                        = (plane.mDistance - plane.mNormal.x() * position.x() - plane.mNormal.y() * position.y())
                        / plane.mNormal.z();
                    const float signedDistance = height - position.z();
                    const float distance = std::abs(signedDistance);
                    const bool insideBound = position.x() >= segment.mWorldBound.xMin()
                        && position.x() <= segment.mWorldBound.xMax() && position.y() >= segment.mWorldBound.yMin()
                        && position.y() <= segment.mWorldBound.yMax();
                    if (signedDistance < -aboveSurfaceTolerance || signedDistance > maximumVerticalDistance
                        || distance > nearestDistance || !insideBound)
                        continue;

                    bool contains = false;
                    for (std::size_t i = 0; i + 2 < segment.mWorldTriangles.size(); i += 3)
                    {
                        const auto horizontal
                            = [](const osg::Vec3f& point) { return osg::Vec2f(point.x(), point.y()); };
                        if (SceneUtil::waterSurfaceTriangleContainsPoint(horizontalPosition,
                                horizontal(segment.mWorldTriangles[i]), horizontal(segment.mWorldTriangles[i + 1]),
                                horizontal(segment.mWorldTriangles[i + 2])))
                        {
                            contains = true;
                            break;
                        }
                    }
                    if (contains)
                    {
                        nearestDistance = distance;
                        nearestHeight = height;
                    }
                }
            }
        }
        return nearestHeight;
    }

    void Water::removeSurfaceEffectBindings(const osg::Node* objectRoot)
    {
        const auto objectIt = mMeshWaterObjects.find(objectRoot);
        if (objectIt == mMeshWaterObjects.end())
            return;

        mMeshWaterSeamRoot->removeChildren(0, mMeshWaterSeamRoot->getNumChildren());
        for (MeshWaterObject::Binding& binding : objectIt->second->mBindings)
        {
            if (binding.mParent.valid() && binding.mReplacement.valid() && binding.mOriginal.valid())
                binding.mParent->replaceChild(binding.mReplacement.get(), binding.mOriginal.get());

            for (const MeshWaterObject::Binding::Segment& segment : binding.mSegments)
            {
                const auto planeIt = mMeshWaterPlanes.find(segment.mPlaneKey);
                if (planeIt != mMeshWaterPlanes.end() && --planeIt->second->mReferenceCount == 0)
                {
                    osg::ref_ptr<Reflection> reflection = planeIt->second->reflection();
                    const unsigned int resolution = planeIt->second->resolution();
                    auto [pendingResolution, pendingReflection] = planeIt->second->cancelStagedReflection();
                    mMeshWaterPlanes.erase(planeIt);
                    releaseMeshWaterReflection(resolution, std::move(reflection));
                    releaseMeshWaterReflection(pendingResolution, std::move(pendingReflection));
                }
            }
        }
        mMeshWaterObjects.erase(objectIt);
        for (auto& [planeKey, plane] : mMeshWaterPlanes)
        {
            plane->clearWorldBound();
            for (const auto& [_, object] : mMeshWaterObjects)
                for (const MeshWaterObject::Binding& binding : object->mBindings)
                    for (const MeshWaterObject::Binding::Segment& segment : binding.mSegments)
                        if (segment.mPlaneKey == planeKey)
                            plane->include(segment.mWorldBound);
        }
        mMeshWaterSeamRebuildDelay = 2;
    }

    void Water::rebuildMeshWaterSeams()
    {
        mMeshWaterSeamRoot->removeChildren(0, mMeshWaterSeamRoot->getNumChildren());
        if (!Settings::water().mShader)
            return;

        struct EdgeSource
        {
            SceneUtil::WaterSurfaceBoundaryEdge mEdge;
            const MeshWaterObject::Binding::Segment* mSegment = nullptr;
            const SceneUtil::SurfaceEffectProfile* mProfile = nullptr;
        };
        struct SeamCandidate
        {
            std::size_t mFirst = 0;
            std::size_t mSecond = 0;
            std::array<osg::Vec3f, 6> mTriangles;
            float mDistance = 0.f;
        };
        struct SeamGeometry
        {
            SceneUtil::SurfaceEffectProfile mProfile;
            std::vector<osg::Vec3f> mTriangles;
        };

        constexpr float maximumGap = SceneUtil::waterSurfaceInteractionTolerance;
        constexpr std::size_t maximumBoundaryEdges = 8192;
        constexpr std::size_t maximumSeams = 4096;
        std::size_t seamCount = 0;
        for (const auto& [planeKey, planeOwner] : mMeshWaterPlanes)
        {
            const SceneUtil::WaterSurfacePlane& plane = planeOwner->plane();
            std::vector<EdgeSource> edges;
            for (const auto& [_, object] : mMeshWaterObjects)
            {
                for (const MeshWaterObject::Binding& binding : object->mBindings)
                {
                    for (const MeshWaterObject::Binding::Segment& segment : binding.mSegments)
                    {
                        if (segment.mPlaneKey != planeKey)
                            continue;
                        const std::vector<SceneUtil::WaterSurfaceBoundaryEdge> boundaries
                            = SceneUtil::waterSurfaceBoundaryEdges(segment.mWorldTriangles, 0.01f);
                        for (const SceneUtil::WaterSurfaceBoundaryEdge& edge : boundaries)
                        {
                            if (edges.size() >= maximumBoundaryEdges)
                                break;
                            edges.push_back({ edge, &segment, &binding.mProfile });
                        }
                    }
                }
            }

            std::map<std::pair<int, int>, std::vector<std::size_t>> spatialBins;
            std::vector<SeamCandidate> candidates;
            for (std::size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex)
            {
                const EdgeSource& edge = edges[edgeIndex];
                const osg::Vec2f midpoint = (plane.project(edge.mEdge.mStart) + plane.project(edge.mEdge.mEnd)) * 0.5f;
                const std::pair<int, int> bin{ static_cast<int>(std::floor(midpoint.x() / maximumGap)),
                    static_cast<int>(std::floor(midpoint.y() / maximumGap)) };
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const auto binIt = spatialBins.find({ bin.first + x, bin.second + y });
                        if (binIt == spatialBins.end())
                            continue;
                        for (const std::size_t otherIndex : binIt->second)
                        {
                            const EdgeSource& other = edges[otherIndex];
                            if (edge.mSegment == other.mSegment || edge.mProfile->mId != other.mProfile->mId)
                                continue;
                            const auto triangles
                                = SceneUtil::waterSurfaceSeamTriangles(other.mEdge, edge.mEdge, plane, maximumGap);
                            if (!triangles || candidates.size() >= maximumSeams)
                                continue;
                            candidates.push_back({ otherIndex, edgeIndex, *triangles,
                                (other.mEdge.mStart - edge.mEdge.mStart).length2()
                                    + (other.mEdge.mEnd - edge.mEdge.mEnd).length2() });
                        }
                    }
                }
                spatialBins[bin].push_back(edgeIndex);
            }

            std::ranges::sort(candidates, {}, &SeamCandidate::mDistance);
            std::vector<bool> used(edges.size(), false);
            std::map<std::string, SeamGeometry> geometryByProfile;
            for (const SeamCandidate& candidate : candidates)
            {
                if (used[candidate.mFirst] || used[candidate.mSecond])
                    continue;
                used[candidate.mFirst] = true;
                used[candidate.mSecond] = true;
                ++seamCount;
                SeamGeometry& geometry = geometryByProfile[edges[candidate.mFirst].mProfile->mId];
                geometry.mProfile = *edges[candidate.mFirst].mProfile;
                geometry.mTriangles.insert(
                    geometry.mTriangles.end(), candidate.mTriangles.begin(), candidate.mTriangles.end());
            }

            for (auto& [profileId, seam] : geometryByProfile)
            {
                osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
                if (meshWaterWaveHeight(seam.mProfile) > 0.f)
                {
                    for (std::size_t index = 0; index < seam.mTriangles.size(); index += 3)
                        appendWaterTriangle(*vertices, seam.mTriangles[index], seam.mTriangles[index + 1],
                            seam.mTriangles[index + 2], seam.mTriangles[index], seam.mTriangles[index + 1],
                            seam.mTriangles[index + 2]);
                }
                else
                    vertices->assign(seam.mTriangles.begin(), seam.mTriangles.end());

                osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
                geometry->setName("Mesh Water Seam: " + profileId);
                geometry->setVertexArray(vertices);
                osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
                normals->push_back(plane.mNormal);
                geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
                geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices->size())));
                geometry->setUserValue("meshWaterSurface", true);

                osg::ref_ptr<osg::Group> wrapper = new osg::Group;
                wrapper->setName("Mesh Water Seam Material: " + profileId);
                wrapper->setNodeMask(Mask_Water);
                wrapper->addChild(geometry);
                createMeshWaterStateSet(*wrapper, *planeOwner->reflection(), seam.mProfile, osg::Vec3f(0.f, 0.f, 0.f));
                // Repair geometry is a first-class part of the shared plane. It must keep that plane's reflection
                // active and sized while visible even when every authored segment happens to be outside the view.
                wrapper->addCullCallback(new MeshWaterVisibilityCallback(planeOwner.get(), geometry->getBoundingBox()));
                mMeshWaterSeamRoot->addChild(wrapper);
            }
        }
        Log(Debug::Verbose) << "Rebuilt " << seamCount << " planar mesh-water seams across " << mMeshWaterPlanes.size()
                            << " reflection planes";
    }

    void Water::rebuildMeshWaterStateSets()
    {
        for (auto& [_, object] : mMeshWaterObjects)
        {
            for (MeshWaterObject::Binding& binding : object->mBindings)
            {
                for (MeshWaterObject::Binding::Segment& segment : binding.mSegments)
                {
                    const auto planeIt = mMeshWaterPlanes.find(segment.mPlaneKey);
                    if (!segment.mWrapper.valid() || planeIt == mMeshWaterPlanes.end())
                        continue;
                    segment.mWrapper->setCullCallback(nullptr);
                    createMeshWaterStateSet(
                        *segment.mWrapper, *planeIt->second->reflection(), binding.mProfile, binding.mNodePosition);
                    segment.mWrapper->addCullCallback(
                        new MeshWaterVisibilityCallback(planeIt->second.get(), segment.mDrawable->getBoundingBox()));
                }
            }
        }
        rebuildMeshWaterSeams();
        mMeshWaterSeamRebuildDelay = 0;
    }

    void Water::processChangedSettings(const Settings::CategorySettingVector& settings)
    {
        const bool usedDisplacedGeometry = mUsesDisplacedGeometry;
        const bool usedSurfaceCoverage = mUsesSurfaceCoverage;
        const bool usedMeshGeometricWaves = mMeshGeometricWaveHeight > 0.f;
        mSurfaceParameters = defaultWaterSurfaceParameters();
        mMeshGeometricWaveHeight = Settings::openmwseWater().mMeshGeometricWaveHeight.get();
        updateWaterGeometry();
        if (mWaveOcclusion)
        {
            mWaveOcclusion->setMaximumClearance(
                SceneUtil::waterWaveInteractionHeight(mSurfaceParameters.mGeometricWaveHeight));
            mWaveOcclusion->setCausticMaximumClearance(mSurfaceParameters.mOptics.mCaustics
                    ? SceneUtil::waterCausticInteractionHeight
                    : SceneUtil::waterWaveInteractionHeight(mSurfaceParameters.mGeometricWaveHeight));
        }
        const bool rendererWaterSettingsChanged = settings.empty()
            || std::any_of(
                settings.begin(), settings.end(), [](const auto& setting) { return setting.first == "Water"; });
        const bool meshWaveGeometryChanged = usedMeshGeometricWaves != (mMeshGeometricWaveHeight > 0.f);
        if (rendererWaterSettingsChanged)
        {
            updateWaterMaterial();
            if (Settings::water().mShader)
            {
                const std::vector<const osg::Node*> known(
                    mKnownSurfaceEffectObjects.begin(), mKnownSurfaceEffectObjects.end());
                if (meshWaveGeometryChanged)
                    for (const osg::Node* root : known)
                        removeSurfaceEffectBindings(root);
                for (const osg::Node* root : known)
                    registerSurfaceEffects(const_cast<osg::Node*>(root));
                if (!meshWaveGeometryChanged)
                    rebuildMeshWaterStateSets();
            }
            else
            {
                while (!mMeshWaterObjects.empty())
                    removeSurfaceEffectBindings(mMeshWaterObjects.begin()->first);
            }
        }
        else if (Settings::water().mShader)
        {
            if (meshWaveGeometryChanged)
            {
                const std::vector<const osg::Node*> known(
                    mKnownSurfaceEffectObjects.begin(), mKnownSurfaceEffectObjects.end());
                for (const osg::Node* root : known)
                    removeSurfaceEffectBindings(root);
                for (const osg::Node* root : known)
                    registerSurfaceEffects(const_cast<osg::Node*>(root));
            }
            if (usedDisplacedGeometry != mUsesDisplacedGeometry || usedSurfaceCoverage != mUsesSurfaceCoverage)
                updateShaderWaterState();
        }
    }

    Water::~Water()
    {
        while (!mMeshWaterObjects.empty())
            removeSurfaceEffectBindings(mMeshWaterObjects.begin()->first);
        mSceneRoot->removeChild(mMeshWaterSeamRoot);
        mParent->removeChild(mWaterNode);

        if (mReflection)
        {
            mParent->removeChild(mReflection);
            mReflection = nullptr;
        }
        if (mRipples)
        {
            mParent->removeChild(mRipples);
            mRipples = nullptr;
            mSimulation->setRipples(nullptr);
        }
    }

    void Water::listAssetsToPreload(std::vector<VFS::Path::Normalized>& textures)
    {
        const int frameCount = std::clamp(Fallback::Map::getInt("Water_SurfaceFrameCount"), 0, 320);
        std::string_view texture = Fallback::Map::getString("Water_SurfaceTexture");
        for (int i = 0; i < frameCount; ++i)
        {
            std::ostringstream texname;
            texname << "textures/water/" << texture << std::setw(2) << std::setfill('0') << i << ".dds";
            textures.emplace_back(texname.str());
        }
    }

    void Water::setEnabled(bool enabled)
    {
        mEnabled = enabled;
        updateVisible();
    }

    void Water::changeCell(const MWWorld::CellStore* store)
    {
        bool isInterior = !store->getCell()->isExterior();
        bool wasInterior = mInterior;
        if (!isInterior)
        {
            mWaterNode->setPosition(
                getSceneNodeCoordinates(store->getCell()->getGridX(), store->getCell()->getGridY()));
            mInterior = false;
        }
        else
        {
            mWaterNode->setPosition(osg::Vec3f(0, 0, mTop));
            mInterior = true;
        }
        if (mInterior != wasInterior && mReflection)
            mReflection->setInterior(mInterior);
        if (mInterior != wasInterior)
            for (auto& [_, plane] : mMeshWaterPlanes)
            {
                plane->reflection()->setInterior(mInterior);
                if (plane->pendingReflection())
                    plane->pendingReflection()->setInterior(mInterior);
            }
        const osg::Vec3d position = mWaterNode->getPosition();
        if (mWaveOcclusion)
        {
            mWaveOcclusion->setCenter(osg::Vec2f(static_cast<float>(position.x()), static_cast<float>(position.y())));
            if (osg::Uniform* center = mSceneRoot->getOrCreateStateSet()->getUniform("waterSurfaceCoverageCenter"))
                center->set(mWaveOcclusion->center());
        }
    }

    void Water::setHeight(const float height)
    {
        mTop = height;

        mSimulation->setWaterHeight(height);

        osg::Vec3f pos = mWaterNode->getPosition();
        pos.z() = height;
        mWaterNode->setPosition(pos);

        if (mReflection)
            mReflection->setWaterPlane(horizontalWaterPlane(mTop));
        if (mWaveOcclusion)
            mWaveOcclusion->setWaterHeight(height);
    }

    void Water::setRainIntensity(float rainIntensity)
    {
        mRainIntensity = rainIntensity;
        if (mRainSettingsUpdater)
            mRainSettingsUpdater->setRainIntensity(rainIntensity);
    }

    void Water::update(float dt, bool paused)
    {
        if (mMeshWaterSeamRebuildDelay > 0 && --mMeshWaterSeamRebuildDelay == 0)
            rebuildMeshWaterSeams();
        for (auto& [_, plane] : mMeshWaterPlanes)
        {
            if (plane->pendingReflection() && plane->pendingReflection()->hasCompletedRender())
            {
                const unsigned int publishedResolution = plane->pendingResolution();
                auto [previousResolution, previous] = plane->publishStagedReflection();
                releaseMeshWaterReflection(previousResolution, std::move(previous));
                Log(Debug::Info) << "Published completed mesh-water reflection for plane distance "
                                 << plane->plane().mDistance << " at " << publishedResolution << "px";
            }
            if (const std::optional<unsigned int> resolution = plane->consumeRequestedResolution(); resolution)
                resizeMeshWaterPlane(*plane, *resolution);
        }

        if (!paused)
        {
            mSimulation->update(dt);
            if (!mInterior && mRipples && mSurfaceParameters.mOptics.mAppearance == Settings::WaterAppearance::Enhanced
                && mSurfaceParameters.mOptics.mEnhancedSewerRipples)
            {
                mSewerRippleTimer -= dt;
                if (mSewerRippleTimer <= 0.f)
                {
                    mRipples->emitNearbySewerRipples();
                    mSewerRippleTimer = 0.75f;
                }
            }
            else
                mSewerRippleTimer = 0.f;
        }

        if (mRipples)
        {
            mRipples->setPaused(paused);
        }
        if (mWaveOcclusion)
            mWaveOcclusion->update(paused ? 0.f : dt);
    }

    void Water::updateVisible()
    {
        bool visible = mEnabled && mToggled;
        mWaterNode->setNodeMask(visible ? ~0u : 0u);
        mMeshWaterSeamRoot->setNodeMask(visible ? ~0u : 0u);
        for (auto& [_, object] : mMeshWaterObjects)
            for (MeshWaterObject::Binding& binding : object->mBindings)
                if (binding.mReplacement.valid())
                    binding.mReplacement->setNodeMask(visible ? ~0u : 0u);
        if (mReflection)
            mReflection->setNodeMask(visible ? Mask_RenderToTexture : 0u);
        for (auto& [_, plane] : mMeshWaterPlanes)
        {
            plane->reflection()->setNodeMask(visible ? Mask_RenderToTexture : 0u);
            if (plane->pendingReflection())
                plane->pendingReflection()->setNodeMask(visible ? Mask_RenderToTexture : 0u);
        }
        if (mRipples)
            mRipples->setNodeMask(visible ? Mask_RenderToTexture : 0u);
    }

    bool Water::toggle()
    {
        mToggled = !mToggled;
        updateVisible();
        return mToggled;
    }

    bool Water::isUnderwater(const osg::Vec3f& pos) const
    {
        return pos.z() < mTop && mToggled && mEnabled;
    }

    osg::Vec3f Water::getSceneNodeCoordinates(int gridX, int gridY)
    {
        return osg::Vec3f(static_cast<float>(gridX * Constants::CellSizeInUnits + (Constants::CellSizeInUnits / 2)),
            static_cast<float>(gridY * Constants::CellSizeInUnits + (Constants::CellSizeInUnits / 2)), mTop);
    }

    void Water::addEmitter(const MWWorld::Ptr& ptr, float scale, float force)
    {
        mSimulation->addEmitter(ptr, scale, force);
    }

    void Water::removeEmitter(const MWWorld::Ptr& ptr)
    {
        mSimulation->removeEmitter(ptr);
    }

    void Water::updateEmitterPtr(const MWWorld::Ptr& old, const MWWorld::Ptr& ptr)
    {
        mSimulation->updateEmitterPtr(old, ptr);
    }

    void Water::emitRipple(const osg::Vec3f& pos)
    {
        mSimulation->emitRipple(pos);
    }

    void Water::removeCell(const MWWorld::CellStore* store)
    {
        mSimulation->removeCell(store);
    }

    void Water::clearRipples()
    {
        mSimulation->clear();
    }

    void Water::showWorld(bool show)
    {
        if (mReflection)
            mReflection->showWorld(show);
        for (auto& [_, plane] : mMeshWaterPlanes)
        {
            plane->reflection()->showWorld(show);
            if (plane->pendingReflection())
                plane->pendingReflection()->showWorld(show);
        }
        mShowWorld = show;
    }

}
