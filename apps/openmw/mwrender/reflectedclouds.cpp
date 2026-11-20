#include "reflectedclouds.hpp"

#include "renderbin.hpp"
#include "vismask.hpp"

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/shader/cloudprogram.hpp>

#include <osg/Camera>
#include <osg/Texture>
#include <osg/Uniform>
#include <osgUtil/CullVisitor>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
    /// Integrate the shared atmosphere dome into a linear HDR cloud target using the parent's reflected view.
    class CloudTarget final : public SceneUtil::RTTNode
    {
    public:
        /// Preserve the selected transport resolution in independent targets for every eye.
        CloudTarget(osg::Node* atmosphere, std::uint32_t width, std::uint32_t height)
            : RTTNode(width, height, 0, false, -1, StereoAwareness::Aware, false)
            , mAtmosphere(atmosphere)
        {
            setColorBufferInternalFormat(GL_RGBA16F);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        /// Store radiance and transmittance without blending, scene clipping, or depth rejection.
        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("ReflectedCloudCamera");
            camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
            // Reflection cameras exclude general RTT traversal; this dependent target belongs to their sky.
            camera->setNodeMask(MWRender::Mask_Sky);
            camera->setClearColor(osg::Vec4(0, 0, 0, 1));
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            osg::StateSet* state = camera->getOrCreateStateSet();
            state->setDefine(
                Shader::sReflectedCloudModeDefine, "1", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_CLIP_PLANE0, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setRenderBinDetails(MWRender::RenderBin_Sky, "RenderBin");
            camera->addChild(mAtmosphere);
        }

    private:
        osg::ref_ptr<osg::Node> mAtmosphere;
    };
}

namespace MWRender
{
    ReflectedClouds::ReflectedClouds(osg::Node* atmosphere)
        : mAtmosphere(atmosphere)
    {
    }

    ReflectedClouds::~ReflectedClouds() = default;

    void ReflectedClouds::setEnabled(bool enabled)
    {
        mEnabled = enabled;
        if (!enabled)
        {
            mClouds = nullptr;
            mViewStates.clear();
        }
    }

    void ReflectedClouds::updateCloudCover(float mainCoverage, float lowCoverage)
    {
        if (!std::isfinite(mainCoverage) || !std::isfinite(lowCoverage))
        {
            mUseCloudTarget = false;
            return;
        }
        const float clearFraction
            = (1.f - std::clamp(mainCoverage, 0.f, 1.f)) * (1.f - std::clamp(lowCoverage, 0.f, 1.f));
        // A separate target evaluates hidden rays too; sparse skies benefit from the reflection's opaque depth.
        const float coverageThreshold = mUseCloudTarget ? 0.4f : 0.5f;
        mUseCloudTarget = 1.f - clearFraction >= coverageThreshold;
    }

    void ReflectedClouds::operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
    {
        if (!mEnabled || !mUseCloudTarget || !SceneUtil::isReflectionCamera(visitor->getCurrentCamera()->getName()))
        {
            traverse(node, visitor);
            return;
        }

        const osg::Viewport* viewport = visitor->getViewport();
        const auto width = static_cast<std::uint32_t>(std::max(1.0, viewport->width()));
        const auto height = static_cast<std::uint32_t>(std::max(1.0, viewport->height()));
        const std::uint32_t cloudWidth = width / 2 + width % 2;
        const std::uint32_t cloudHeight = height / 2 + height % 2;
        if (!mClouds || mClouds->width() != cloudWidth || mClouds->height() != cloudHeight)
        {
            mClouds = new CloudTarget(mAtmosphere, cloudWidth, cloudHeight);
            mViewStates.clear();
        }
        mClouds->cull(visitor);
        auto& state = mViewStates[visitor];
        if (!state)
        {
            state = new osg::StateSet;
            state->setDefine(
                Shader::sReflectedCloudModeDefine, "2", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->addUniform(new osg::Uniform("reflectedCloudTexture", sTextureUnit));
            state->setTextureAttribute(sTextureUnit, mClouds->getColorTexture(visitor));
        }
        visitor->pushStateSet(state);
        traverse(node, visitor);
        visitor->popStateSet();
    }
}
