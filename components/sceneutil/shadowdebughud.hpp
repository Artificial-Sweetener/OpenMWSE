#ifndef OPENMW_COMPONENTS_SCENEUTIL_SHADOWDEBUGHUD_H
#define OPENMW_COMPONENTS_SCENEUTIL_SHADOWDEBUGHUD_H

#include <array>
#include <vector>

#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Program>
#include <osg/Referenced>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec3>

#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    /// Owns the optional shadow-map and cascade-frustum debug visualization.
    class ShadowDebugHud final : public osg::Referenced
    {
    public:
        /// Create visualization resources for the configured number of shadow maps.
        explicit ShadowDebugHud(int numberOfShadowMapsPerLight);

        /// Submit one shadow texture and its frustum transform to the debug cameras.
        void draw(osg::ref_ptr<osg::Texture2D> texture, unsigned int shadowMapNumber, const osg::Matrixd& matrix,
            osgUtil::CullVisitor& visitor);

        /// Release graphics objects owned by the debug visualization.
        void releaseGLObjects(osg::State* state = nullptr) const;

        /// Publish main-view frustum vertices for one traversal buffer.
        void setFrustumVertices(osg::ref_ptr<osg::Vec3Array> vertices, unsigned int traversalNumber);

    private:
        /// Add resources for one more runtime shadow-map cascade.
        void addShadowMap();

        static constexpr int sDebugTextureUnit = 0;

        std::vector<osg::ref_ptr<osg::Camera>> mDebugCameras;
        osg::ref_ptr<osg::Program> mDebugProgram;
        std::vector<osg::ref_ptr<osg::Node>> mDebugGeometry;
        std::vector<osg::ref_ptr<osg::Group>> mFrustumTransforms;
        std::array<std::vector<osg::ref_ptr<osg::Uniform>>, 2> mFrustumUniforms;
        std::array<osg::ref_ptr<osg::Geometry>, 2> mFrustumGeometries;
    };
}

#endif
