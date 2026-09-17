#include "water.hpp"

#include <algorithm>
#include <sstream>

#include <osg/ClipNode>
#include <osg/Depth>
#include <osg/Fog>
#include <osg/FrontFace>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture3D>
#include <osg/ViewportIndexed>

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

                osg::Vec3d eyePoint = cv->getEyePoint();
                if (mCullPlane->intersect(osg::BoundingSphere(osg::Vec3d(0, 0, eyePoint.z()), 0)) > 0)
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

                osg::RefMatrix* modelViewMatrix = new osg::RefMatrix(*cv->getModelViewMatrix());

                // apply the height of the plane
                // we can't apply this height in the addClipPlane() since the "flip the below graph" function would
                // otherwise flip the height as well
                modelViewMatrix->preMultTranslate(mCullPlane->getNormal() * ((*mCullPlane)[3] * -1));

                // flip the below graph if the eye point is above the plane
                if (mCullPlane->intersect(osg::BoundingSphere(osg::Vec3d(0, 0, eyePoint.z()), 0)) > 0)
                {
                    modelViewMatrix->preMultScale(osg::Vec3(1, 1, -1));
                }

                // move the plane back along its normal a little bit to prevent bleeding at the water shore
                const float fov = Settings::camera().mFieldOfView;
                constexpr double clipFudgeMin = 2.5; // minimum offset of clip plane
                constexpr double clipFudgeScale = -15000.0;
                double clipFudge
                    = std::abs(std::abs((*mCullPlane)[3]) - eyePoint.z()) * fov / clipFudgeScale - clipFudgeMin;
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
            camera->addCullCallback(new InheritViewPointCallback);

            // Inform the shader that we're in a reflection
            camera->getOrCreateStateSet()->addUniform(new osg::Uniform("isReflection", true));

            // XXX: should really flip the FrontFace on each renderable instead of forcing clockwise.
            osg::ref_ptr<osg::FrontFace> frontFace(new osg::FrontFace);
            frontFace->setMode(osg::FrontFace::CLOCKWISE);
            camera->getOrCreateStateSet()->setAttributeAndModes(frontFace, osg::StateAttribute::ON);

            camera->addChild(mClipCullNode);
            camera->setNodeMask(Mask_RenderToTexture);

            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
        }

        void apply(osg::Camera* camera) override
        {
            camera->setViewMatrix(mViewMatrix);
            camera->setCullMask(mNodeMask);
        }

        void setInterior(bool isInterior)
        {
            mInterior = isInterior;
            mNodeMask = calcNodeMask();
        }

        void setWaterLevel(float waterLevel)
        {
            mViewMatrix = osg::Matrix::scale(1, 1, -1) * osg::Matrix::translate(0, 0, 2 * waterLevel);
            mClipCullNode->setPlane(osg::Plane(osg::Vec3d(0, 0, 1), osg::Vec3d(0, 0, waterLevel)));
        }

        void setScene(osg::Node* scene)
        {
            if (mScene)
                mClipCullNode->removeChild(mScene);
            mScene = scene;
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
        , mEnabled(true)
        , mToggled(true)
        , mTop(0)
        , mInterior(false)
        , mShowWorld(true)
        , mCullCallback(nullptr)
        , mShaderWaterStateSetUpdater(nullptr)
    {
        mSimulation = std::make_unique<RippleSimulation>(mSceneRoot, resourceSystem);

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
        }

        mCullCallback = callback;

        if (callback)
        {
            mWaterNode->addCullCallback(callback);
            if (mReflection)
                mReflection->addCullCallback(callback);
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
            mReflection->setWaterLevel(mTop);
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

    class ShaderWaterStateSetUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        ShaderWaterStateSetUpdater(Water* water, Resource::ResourceSystem* resourceSystem, Reflection* reflection,
            Ripples* ripples, osg::ref_ptr<osg::Program> program, osg::ref_ptr<osg::Texture2D> normalMap,
            osg::ref_ptr<osg::Texture3D> mgeWaterVolume)
            : mWater(water)
            , mReflection(reflection)
            , mRipples(ripples)
            , mProgram(std::move(program))
            , mNormalMap(std::move(normalMap))
            , mMgeWaterVolume(std::move(mgeWaterVolume))
            , mResourceSystem(resourceSystem)
            , mOpaqueDepthTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::OpaqueDepthTexture))
            , mOpaqueColorTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::OpaqueColorTexture))
            , mWaterOpticalTextureUnit(resourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
                  Shader::ShaderManager::Slot::WaterOptics, 3))
        {
        }

        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->addUniform(new osg::Uniform("normalMap", 0));
            stateset->setTextureAttribute(0, mNormalMap, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("mgeWaterVolume", 6));
            stateset->setTextureAttribute(6, mMgeWaterVolume, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("waterOpticalNormalMap", mWaterOpticalTextureUnit));
            stateset->setTextureAttribute(mWaterOpticalTextureUnit, mNormalMap, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("waterOpticalVolume", mWaterOpticalTextureUnit + 1));
            stateset->setTextureAttribute(mWaterOpticalTextureUnit + 1, mMgeWaterVolume, osg::StateAttribute::ON);
            stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
            stateset->setAttributeAndModes(mProgram, osg::StateAttribute::ON);

            stateset->addUniform(new osg::Uniform("reflectionMap", 1));
            stateset->addUniform(new osg::Uniform("opaqueColorTex", mOpaqueColorTextureUnit));
            stateset->addUniform(new osg::Uniform("opaqueDepthTex", mOpaqueDepthTextureUnit));
            stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("waterSurface", true));
            stateset->addUniform(new osg::Uniform(
                "waterAppearance", shaderAppearance(mWater->getSurfaceParameters().mOptics.mAppearance)));
            stateset->addUniform(new osg::Uniform(
                "enhancedWaterColor", shaderEnhancedColor(mWater->getSurfaceParameters().mOptics.mEnhancedColor)));
            stateset->addUniform(
                new osg::Uniform("enhancedWaterFoam", mWater->getSurfaceParameters().mOptics.mEnhancedFoam));
            stateset->addUniform(new osg::Uniform(
                "enhancedWaterDistantWaves", mWater->getSurfaceParameters().mOptics.mEnhancedDistantWaves));
            stateset->setRenderBinDetails(MWRender::RenderBin_DepthSorted, "DepthSortedBin");
            osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
            depth->setWriteMask(false);
            stateset->setAttributeAndModes(depth, osg::StateAttribute::ON);

            if (mRipples)
            {
                stateset->addUniform(new osg::Uniform("rippleMap", 4));
            }
            stateset->addUniform(new osg::Uniform("nodePosition", osg::Vec3f(mWater->getPosition())));
            stateset->addUniform(
                new osg::Uniform("geometricWaveHeight", mWater->getSurfaceParameters().mGeometricWaveHeight));
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
        {
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
            stateset->setTextureAttribute(1, mReflection->getColorTexture(cv), osg::StateAttribute::ON);
            stateset->setTextureAttribute(mOpaqueColorTextureUnit,
                mResourceSystem->getSceneManager()->getOpaqueColorTex(cv->getTraversalNumber()),
                osg::StateAttribute::ON);
            stateset->setTextureAttribute(mOpaqueDepthTextureUnit,
                mResourceSystem->getSceneManager()->getOpaqueDepthTex(cv->getTraversalNumber()),
                osg::StateAttribute::ON);

            if (mRipples)
            {
                stateset->setTextureAttribute(4, mRipples->getColorTexture(), osg::StateAttribute::ON);
            }
            stateset->getUniform("nodePosition")->set(osg::Vec3f(mWater->getPosition()));
            stateset->getUniform("geometricWaveHeight")->set(mWater->getSurfaceParameters().mGeometricWaveHeight);
            stateset->getUniform("waterAppearance")
                ->set(shaderAppearance(mWater->getSurfaceParameters().mOptics.mAppearance));
            stateset->getUniform("enhancedWaterColor")
                ->set(shaderEnhancedColor(mWater->getSurfaceParameters().mOptics.mEnhancedColor));
            stateset->getUniform("enhancedWaterFoam")->set(mWater->getSurfaceParameters().mOptics.mEnhancedFoam);
            stateset->getUniform("enhancedWaterDistantWaves")
                ->set(mWater->getSurfaceParameters().mOptics.mEnhancedDistantWaves);
        }

    private:
        Water* mWater;
        Reflection* mReflection;
        Ripples* mRipples;
        osg::ref_ptr<osg::Program> mProgram;
        osg::ref_ptr<osg::Texture2D> mNormalMap;
        osg::ref_ptr<osg::Texture3D> mMgeWaterVolume;
        Resource::ResourceSystem* mResourceSystem;
        int mOpaqueDepthTextureUnit;
        int mOpaqueColorTextureUnit;
        int mWaterOpticalTextureUnit;
    };

    void Water::createShaderWaterStateSet(osg::Node* node)
    {
        // use a define map to conditionally compile the shader
        std::map<std::string, std::string> defineMap;
        const int rippleDetail = Settings::water().mRainRippleDetail;
        defineMap["waterRefraction"] = std::string(Settings::water().mRefraction ? "1" : "0");
        defineMap["rainRippleDetail"] = std::to_string(rippleDetail);
        defineMap["rippleMapWorldScale"] = std::to_string(RipplesSurface::sWorldScaleFactor);
        defineMap["rippleMapSize"] = std::to_string(RipplesSurface::sRTTSize) + ".0";
        defineMap["sunlightScattering"] = Settings::water().mSunlightScattering ? "1" : "0";
        defineMap["wobblyShores"] = Settings::water().mWobblyShores ? "1" : "0";
        defineMap["waterGeometricWaves"] = mSurfaceParameters.useGeometricWaves() ? "1" : "0";

        Stereo::shaderStereoDefines(defineMap);

        Shader::ShaderManager& shaderMgr = mResourceSystem->getSceneManager()->getShaderManager();
        osg::ref_ptr<osg::Program> program
            = shaderMgr.getProgram("water", defineMap, nullptr, {}, Shader::ProgramCapability::Instanced);

        constexpr VFS::Path::NormalizedView waterImage("textures/omw/water_nm.png");
        osg::ref_ptr<osg::Texture2D> normalMap(
            new osg::Texture2D(mResourceSystem->getImageManager()->getImage(waterImage)));
        normalMap->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        normalMap->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        mResourceSystem->getSceneManager()->applyFilterSettings(normalMap);

        osg::ref_ptr<osg::Texture3D> normalVolume(new osg::Texture3D(normalMap->getImage()));
        normalVolume->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        normalVolume->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        normalVolume->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
        normalVolume->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        normalVolume->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

        constexpr VFS::Path::NormalizedView mgeWaterImage("textures/openmwse/mge_xe_water_nrm.dds");
        osg::ref_ptr<osg::Texture3D> mgeWaterVolume(
            new osg::Texture3D(mResourceSystem->getImageManager()->getImage(mgeWaterImage, true)));
        mgeWaterVolume->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        mgeWaterVolume->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        mgeWaterVolume->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);
        mgeWaterVolume->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mgeWaterVolume->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);

        const int waterOpticalTextureUnit
            = shaderMgr.reserveGlobalTextureUnits(Shader::ShaderManager::Slot::WaterOptics, 3);
        osg::StateSet* sceneState = mSceneRoot->getOrCreateStateSet();
        sceneState->addUniform(new osg::Uniform("waterOpticalNormalMap", waterOpticalTextureUnit));
        sceneState->setTextureAttribute(waterOpticalTextureUnit, normalVolume, osg::StateAttribute::ON);
        sceneState->addUniform(new osg::Uniform("waterOpticalVolume", waterOpticalTextureUnit + 1));
        sceneState->setTextureAttribute(waterOpticalTextureUnit + 1, mgeWaterVolume, osg::StateAttribute::ON);
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

        mShaderWaterStateSetUpdater = new ShaderWaterStateSetUpdater(this, mResourceSystem, mReflection, mRipples,
            std::move(program), std::move(normalMap), std::move(mgeWaterVolume));
        node->addCullCallback(mShaderWaterStateSetUpdater);
    }

    void Water::processChangedSettings(const Settings::CategorySettingVector& settings)
    {
        const bool usedDisplacedGeometry = mUsesDisplacedGeometry;
        const bool usedSurfaceCoverage = mUsesSurfaceCoverage;
        mSurfaceParameters = defaultWaterSurfaceParameters();
        updateWaterGeometry();
        if (mWaveOcclusion)
        {
            mWaveOcclusion->setMaximumClearance(
                SceneUtil::waterWaveInteractionHeight(mSurfaceParameters.mGeometricWaveHeight));
            mWaveOcclusion->setCausticMaximumClearance(mSurfaceParameters.mOptics.mCaustics
                    ? SceneUtil::waterCausticInteractionHeight
                    : SceneUtil::waterWaveInteractionHeight(mSurfaceParameters.mGeometricWaveHeight));
        }
        const bool waterSettingsChanged = settings.empty()
            || std::any_of(
                settings.begin(), settings.end(), [](const auto& setting) { return setting.first == "Water"; });
        if (waterSettingsChanged)
            updateWaterMaterial();
        else if (Settings::water().mShader
            && (usedDisplacedGeometry != mUsesDisplacedGeometry || usedSurfaceCoverage != mUsesSurfaceCoverage))
            updateShaderWaterState();
    }

    Water::~Water()
    {
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
            mReflection->setWaterLevel(mTop);
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
        if (mReflection)
            mReflection->setNodeMask(visible ? Mask_RenderToTexture : 0u);
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
        mShowWorld = show;
    }

}
