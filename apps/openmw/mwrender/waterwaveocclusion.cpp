#include "waterwaveocclusion.hpp"

#include <osg/Camera>
#include <osg/ColorMask>
#include <osg/Depth>
#include <osg/Group>
#include <osg/Image>
#include <osg/Uniform>

#include <cmath>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/sceneutil/waterutil.hpp>
#include <components/shader/shadermanager.hpp>

#include "vismask.hpp"

namespace
{
    constexpr int sTextureSize = 256;
    constexpr float sWorldSize = 32768.f;
    constexpr float sHeightRange = 65536.f;

    /// Override authored materials so nearby terrain and placed-object triangles contribute coverage regardless of alpha.
    class WaterWaveCoverageState final : public SceneUtil::StateSetUpdater
    {
    public:
        /// Resolve the dedicated coverage shader once for the cached obstruction camera.
        explicit WaterWaveCoverageState(Resource::ResourceSystem& resourceSystem)
            : mProgram(resourceSystem.getSceneManager()->getShaderManager().getProgram(
                "waterwaveblocker", {}, nullptr, {}, Shader::ProgramCapability::Instanced))
        {
        }

    private:
        /// Install coverage-only override state for all traversed terrain and placed-object geometry.
        void setDefaults(osg::StateSet* stateSet) override
        {
            stateSet->setAttributeAndModes(mProgram, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet->setAttributeAndModes(
                new osg::ColorMask(true, true, true, true), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::GREATER),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet->setRenderBinDetails(
                osg::StateSet::OPAQUE_BIN, "RenderBin", osg::StateSet::OVERRIDE_PROTECTED_RENDERBIN_DETAILS);
        }

        /// Keep the static override state unchanged between cache refreshes.
        void apply(osg::StateSet*, osg::NodeVisitor*) override {}

        osg::ref_ptr<osg::Program> mProgram;
    };
}

namespace MWRender
{
    WaterWaveOcclusion::WaterWaveOcclusion(
        osg::Group* parent, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem)
        : mParent(parent)
        , mCoverageTexture(new osg::Texture2D)
        , mDepthTexture(new osg::Texture2D)
    {
        mCoverageTexture->setTextureSize(sTextureSize, sTextureSize);
        mCoverageTexture->setSourceFormat(GL_RGBA);
        mCoverageTexture->setInternalFormat(GL_RGBA8);
        mCoverageTexture->setSourceType(GL_UNSIGNED_BYTE);
        mCoverageTexture->setNumMipmapLevels(osg::Image::computeNumberOfMipmapLevels(sTextureSize, sTextureSize));
        mCoverageTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER);
        mCoverageTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);
        mCoverageTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        mCoverageTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mCoverageTexture->setBorderColor(osg::Vec4(0.f, 0.f, 0.f, 0.f));

        mDepthTexture->setTextureSize(sTextureSize, sTextureSize);
        mDepthTexture->setSourceFormat(GL_DEPTH_COMPONENT);
        mDepthTexture->setInternalFormat(GL_DEPTH_COMPONENT24);
        mDepthTexture->setSourceType(GL_UNSIGNED_INT);
        mDepthTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER);
        mDepthTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);
        mDepthTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mDepthTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mDepthTexture->setBorderColor(
            SceneUtil::AutoDepth::isReversed() ? osg::Vec4(0.f, 0.f, 0.f, 0.f) : osg::Vec4(1.f, 1.f, 1.f, 1.f));

        mCamera = new osg::Camera;
        mCamera->setName(std::string(SceneUtil::waterWaveOcclusionCameraName));
        mCamera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        mCamera->setRenderOrder(osg::Camera::PRE_RENDER);
        mCamera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT);
        mCamera->setNodeMask(0u);
        mCamera->setCullMask(Mask_Scene | Mask_Terrain | Mask_Object | Mask_Static);
        mCamera->setViewport(0, 0, sTextureSize, sTextureSize);
        mCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        mCamera->setClearColor(osg::Vec4(0.f, 0.f, 0.f, 0.f));
        mCamera->attach(osg::Camera::COLOR_BUFFER, mCoverageTexture, 0, 0, true);
        mCamera->attach(osg::Camera::DEPTH_BUFFER, mDepthTexture, 0, 0, false);
        mCamera->addChild(sceneRoot);
        mWaterLevelUniform = new osg::Uniform("waterWaveLevel", mWaterHeight);
        mMaximumClearanceUniform = new osg::Uniform("waterWaveMaximumClearance", mMaximumClearance);
        mCausticMaximumClearanceUniform
            = new osg::Uniform("waterCausticMaximumClearance", mCausticMaximumClearance);
        mCamera->getOrCreateStateSet()->addUniform(mWaterLevelUniform);
        mCamera->getOrCreateStateSet()->addUniform(mMaximumClearanceUniform);
        mCamera->getOrCreateStateSet()->addUniform(mCausticMaximumClearanceUniform);
        mCamera->getOrCreateStateSet()->addUniform(
            new osg::Uniform("waterWaveSurfaceTolerance", SceneUtil::waterSurfaceInteractionTolerance));
        mCamera->setCullCallback(new WaterWaveCoverageState(*resourceSystem));
        mCamera->setClearDepth(SceneUtil::AutoDepth::isReversed() ? 1.0 : 0.0);
        updateCamera();
    }

    void WaterWaveOcclusion::setEnabled(bool enabled)
    {
        if (mEnabled == enabled)
            return;
        mEnabled = enabled;
        if (enabled)
        {
            mParent->addChild(mCamera);
            requestRefresh();
        }
        else
        {
            mParent->removeChild(mCamera);
            mCamera->setNodeMask(0u);
        }
    }

    void WaterWaveOcclusion::setCenter(const osg::Vec2f& center)
    {
        const float texelSize = sWorldSize / static_cast<float>(sTextureSize);
        const osg::Vec2f snapped(
            std::floor(center.x() / texelSize) * texelSize, std::floor(center.y() / texelSize) * texelSize);
        if (snapped == mCenter)
            return;
        mCenter = snapped;
        updateCamera();
        requestRefresh();
    }

    void WaterWaveOcclusion::setWaterHeight(float height)
    {
        if (height == mWaterHeight)
            return;
        mWaterHeight = height;
        mWaterLevelUniform->set(height);
        updateCamera();
        requestRefresh();
    }

    void WaterWaveOcclusion::setMaximumClearance(float height)
    {
        if (height == mMaximumClearance)
            return;
        mMaximumClearance = height;
        mMaximumClearanceUniform->set(height);
        requestRefresh();
    }

    void WaterWaveOcclusion::setCausticMaximumClearance(float height)
    {
        if (height == mCausticMaximumClearance)
            return;
        mCausticMaximumClearance = height;
        mCausticMaximumClearanceUniform->set(height);
        requestRefresh();
    }

    void WaterWaveOcclusion::update(float)
    {
        if (!mEnabled)
            return;
        if (mRefreshFrames > 0)
        {
            --mRefreshFrames;
            if (mRefreshFrames == 0)
                mCamera->setNodeMask(0u);
        }
    }

    float WaterWaveOcclusion::inverseWorldSize() const
    {
        return 1.f / sWorldSize;
    }

    float WaterWaveOcclusion::textureResolution() const
    {
        return static_cast<float>(sTextureSize);
    }

    void WaterWaveOcclusion::updateCamera()
    {
        const float halfSize = sWorldSize * 0.5f;
        if (SceneUtil::AutoDepth::isReversed())
            mCamera->setProjectionMatrix(SceneUtil::getReversedZProjectionMatrixAsOrtho(
                -halfSize, halfSize, -halfSize, halfSize, 0, sHeightRange));
        else
            mCamera->setProjectionMatrix(
                osg::Matrixf::ortho(-halfSize, halfSize, -halfSize, halfSize, 0, sHeightRange));
        mCamera->setViewMatrixAsLookAt(osg::Vec3(mCenter.x(), mCenter.y(), mWaterHeight + sHeightRange),
            osg::Vec3(mCenter.x(), mCenter.y(), mWaterHeight), osg::Vec3(0.f, 1.f, 0.f));
    }

    void WaterWaveOcclusion::requestRefresh()
    {
        if (!mEnabled)
            return;
        mRefreshFrames = 2;
        mCamera->setNodeMask(Mask_RenderToTexture);
    }
}
