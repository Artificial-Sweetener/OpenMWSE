#include "planarreflectionrenderutils.hpp"

#include <stdexcept>
#include <utility>

#include <osg/Camera>
#include <osg/Drawable>
#include <osg/RenderInfo>
#include <osg/StateSet>
#include <osg/Texture>
#include <osg/Uniform>
#include <osgUtil/CullVisitor>
#include <osgUtil/StateGraph>

#include <components/sceneutil/nodecallback.hpp>
#include <components/settings/values.hpp>

#include "planarreflectiondiagnostics.hpp"
#include "renderworkcounter.hpp"

namespace MWRender
{
    namespace
    {
        /// Publish completion only after the render thread finishes a reflection camera.
        class CompletionCallback final : public osg::Camera::DrawCallback
        {
        public:
            /// Retain the target-owned completion flag without extending the target's scene lifetime.
            explicit CompletionCallback(std::shared_ptr<std::atomic_bool> completed)
                : mCompleted(std::move(completed))
            {
            }

            /// Mark the rendered target safe for a frame-boundary handoff.
            void operator()(osg::RenderInfo&) const override { mCompleted->store(true, std::memory_order_release); }

        private:
            std::shared_ptr<std::atomic_bool> mCompleted;
        };

        /// Publish selected planar attachments for frame comparison without synchronous readback.
        class AttachmentCallback final : public osg::Camera::DrawCallback
        {
        public:
            /// Retain view-local attachments and provider-owned publication flags.
            AttachmentCallback(osg::Texture& color, osg::Texture& depth,
                std::shared_ptr<std::atomic_bool> publishDominant, std::shared_ptr<std::atomic_bool> publishComparison)
                : mColor(&color)
                , mDepth(&depth)
                , mPublishDominant(std::move(publishDominant))
                , mPublishComparison(std::move(publishComparison))
            {
            }

            /// Publish completed attachment identities for the current graphics context and frame.
            void operator()(osg::RenderInfo& renderInfo) const override
            {
                osg::State* state = renderInfo.getState();
                if (state == nullptr || state->getFrameStamp() == nullptr)
                    return;
                const unsigned int context = state->getContextID();
                const unsigned int frame = state->getFrameStamp()->getFrameNumber();
                if (mPublishDominant->load(std::memory_order_acquire))
                    publishDominantPlanarReflection(context, frame, *mColor, *mDepth);
                if (mPublishComparison->load(std::memory_order_acquire))
                    publishComparisonPlanarReflection(context, frame, *mColor, *mDepth);
            }

        private:
            osg::ref_ptr<osg::Texture> mColor;
            osg::ref_ptr<osg::Texture> mDepth;
            std::shared_ptr<std::atomic_bool> mPublishDominant;
            std::shared_ptr<std::atomic_bool> mPublishComparison;
        };

        /// Count submitted reconstruction-proxy draws while preserving normal geometry rendering.
        class ProxyDrawCallback final : public osg::Drawable::DrawCallback
        {
        public:
            /// Record submission and invoke the geometry's ordinary draw implementation.
            void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
            {
                osg::Camera* camera = renderInfo.getCurrentCamera();
                if (camera != nullptr && camera->getStats() != nullptr
                    && renderInfo.getState()->getFrameStamp() != nullptr)
                    addDiagnosticRenderWorkCounter(*camera->getStats(),
                        renderInfo.getState()->getFrameStamp()->getFrameNumber(), planarReflectionProxyDrawCounter,
                        1.0);
                drawable->drawImplementation(renderInfo);
            }
        };

        /// Preserve the parent viewpoint while applying a relative reflected camera transform.
        class InheritViewPointCallback final
            : public SceneUtil::NodeCallback<InheritViewPointCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// Re-enter traversal with the reflected model-view matrix and original LOD viewpoint.
            void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
            {
                osg::ref_ptr<osg::RefMatrix> modelViewMatrix = new osg::RefMatrix(*visitor->getModelViewMatrix());
                visitor->popModelViewMatrix();
                visitor->pushModelViewMatrix(modelViewMatrix, osg::Transform::ABSOLUTE_RF_INHERIT_VIEWPOINT);
                traverse(node, visitor);
            }
        };

        /// Apply the direct-reflection view adjustment to a reconstruction target subtree.
        class ViewTransformCallback final
            : public SceneUtil::NodeCallback<ViewTransformCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            /// Bind traversal to a target-owned plane whose address remains stable.
            explicit ViewTransformCallback(const SceneUtil::WaterSurfacePlane* plane)
                : mPlane(plane)
            {
            }

            /// Enter the same reflected model-view space used by an exact direct capture.
            void operator()(osg::Node* node, osgUtil::CullVisitor* visitor)
            {
                const osg::Vec3d normal(mPlane->mNormal);
                const osg::Plane plane(normal, normal * static_cast<double>(mPlane->mDistance));
                osg::ref_ptr<osg::RefMatrix> modelViewMatrix = new osg::RefMatrix(
                    adjustedPlanarReflectionModelView(*visitor->getModelViewMatrix(), plane, visitor->getEyePoint()));
                visitor->pushModelViewMatrix(modelViewMatrix, osg::Transform::RELATIVE_RF);
                traverse(node, visitor);
                visitor->popModelViewMatrix();
            }

        private:
            const SceneUtil::WaterSurfacePlane* mPlane;
        };
    }

    osg::Matrixf inheritedPlanarReflectionShaderProjection(osgUtil::CullVisitor& visitor)
    {
        for (osgUtil::StateGraph* graph = visitor.getCurrentStateGraph(); graph != nullptr; graph = graph->_parent)
        {
            const osg::StateSet* state = graph->getStateSet();
            if (state == nullptr)
                continue;
            const osg::Uniform* projection = state->getUniform("projectionMatrix");
            osg::Matrixf value;
            if (projection != nullptr && projection->get(value))
                return value;
        }
        throw std::logic_error("planar reflection requires an inherited shader projection");
    }

    osg::Matrixd adjustedPlanarReflectionModelView(
        const osg::Matrixd& modelView, const osg::Plane& plane, const osg::Vec3d& eyePoint)
    {
        const double eyePlaneDistance = plane.distance(eyePoint);
        osg::Matrixd adjusted = modelView;
        adjusted.preMultTranslate(plane.getNormal() * (plane[3] * -1));
        if (eyePlaneDistance > 0.0)
        {
            SceneUtil::WaterSurfacePlane originPlane;
            originPlane.mNormal = osg::Vec3f(plane.getNormal());
            adjusted.preMult(SceneUtil::waterSurfaceReflectionMatrix(originPlane));
        }
        const float fov = Settings::camera().mFieldOfView;
        constexpr double clipFudgeMin = 2.5;
        constexpr double clipFudgeScale = -15000.0;
        const double clipFudge = std::abs(eyePlaneDistance) * fov / clipFudgeScale - clipFudgeMin;
        adjusted.preMultTranslate(plane.getNormal() * clipFudge);
        return adjusted;
    }

    osg::Callback* makePlanarReflectionViewTransformCallback(const SceneUtil::WaterSurfacePlane* plane)
    {
        return new ViewTransformCallback(plane);
    }

    osg::Camera::DrawCallback* makePlanarReflectionCompletionCallback(std::shared_ptr<std::atomic_bool> completed)
    {
        return new CompletionCallback(std::move(completed));
    }

    osg::Camera::DrawCallback* makePlanarReflectionAttachmentCallback(osg::Texture& color, osg::Texture& depth,
        std::shared_ptr<std::atomic_bool> publishDominant, std::shared_ptr<std::atomic_bool> publishComparison)
    {
        return new AttachmentCallback(color, depth, std::move(publishDominant), std::move(publishComparison));
    }

    osg::Callback* makePlanarReflectionInheritViewPointCallback()
    {
        return new InheritViewPointCallback;
    }

    osg::Drawable::DrawCallback* makePlanarReflectionProxyDrawCallback()
    {
        return new ProxyDrawCallback;
    }
}
