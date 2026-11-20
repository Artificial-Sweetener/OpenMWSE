#ifndef OPENMW_MWRENDER_PLANARREFLECTIONRENDERUTILS_H
#define OPENMW_MWRENDER_PLANARREFLECTIONRENDERUTILS_H

#include <atomic>
#include <memory>

#include <osg/Callback>
#include <osg/Camera>
#include <osg/Drawable>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Plane>

#include <components/sceneutil/waterutil.hpp>

namespace osg
{
    class Texture;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace MWRender
{
    /// Return the inherited shader projection active at a reflection cull location.
    osg::Matrixf inheritedPlanarReflectionShaderProjection(osgUtil::CullVisitor& visitor);

    /// Build the established reflected model-view adjustment, including its receiver-distance clip margin.
    osg::Matrixd adjustedPlanarReflectionModelView(
        const osg::Matrixd& modelView, const osg::Plane& plane, const osg::Vec3d& eyePoint);

    /// Create a cull callback that applies the established direct-reflection view adjustment for one plane.
    osg::Callback* makePlanarReflectionViewTransformCallback(const SceneUtil::WaterSurfacePlane* plane);

    /// Create a callback that publishes completion after the render thread finishes a reflection camera.
    osg::Camera::DrawCallback* makePlanarReflectionCompletionCallback(std::shared_ptr<std::atomic_bool> completed);

    /// Create a callback that exposes completed reflection attachments to frame diagnostics.
    osg::Camera::DrawCallback* makePlanarReflectionAttachmentCallback(osg::Texture& color, osg::Texture& depth,
        std::shared_ptr<std::atomic_bool> publishDominant, std::shared_ptr<std::atomic_bool> publishComparison);

    /// Create a cull callback that preserves the parent viewpoint under a relative reflected transform.
    osg::Callback* makePlanarReflectionInheritViewPointCallback();

    /// Create a draw callback that records reconstruction-proxy submissions.
    osg::Drawable::DrawCallback* makePlanarReflectionProxyDrawCallback();
}

#endif
