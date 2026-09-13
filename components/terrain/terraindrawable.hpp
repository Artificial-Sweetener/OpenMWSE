#ifndef OPENMW_COMPONENTS_TERRAIN_DRAWABLE_H
#define OPENMW_COMPONENTS_TERRAIN_DRAWABLE_H

#include <osg/Geometry>

#include <atomic>

namespace osg
{
    class ClusterCullingCallback;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace SceneUtil
{
    class LightListCallback;
}

namespace Terrain
{

    class CompositeMap;
    class CompositeMapRenderer;

    /**
     * Subclass of Geometry that supports built in multi-pass rendering and built in LightListCallback.
     */
    class TerrainDrawable : public osg::Geometry
    {
    public:
        osg::Object* cloneType() const override { return new TerrainDrawable(); }
        osg::Object* clone(const osg::CopyOp& copyop) const override { return new TerrainDrawable(*this, copyop); }
        bool isSameKindAs(const osg::Object* obj) const override
        {
            return dynamic_cast<const TerrainDrawable*>(obj) != nullptr;
        }
        const char* className() const override { return "TerrainDrawable"; }
        const char* libraryName() const override { return "Terrain"; }

        TerrainDrawable();
        ~TerrainDrawable(); // has to be defined in the cpp file because we only forward declared some members.
        TerrainDrawable(const TerrainDrawable& copy, const osg::CopyOp& copyop);

        void accept(osg::NodeVisitor& nv) override;
        void cull(osgUtil::CullVisitor* cv);

        typedef std::vector<osg::ref_ptr<osg::StateSet>> PassVector;
        void setPasses(const PassVector& passes);
        const PassVector& getPasses() const { return mPasses; }

        void setLightListCallback(SceneUtil::LightListCallback* lightListCallback);

        void createClusterCullingCallback();

        void compileGLObjects(osg::RenderInfo& renderInfo) const override;

        void setupWaterBoundingBox(float waterheight, float margin);
        const osg::BoundingBox& getWaterBoundingBox() const { return mWaterBoundingBox; }

        /// Select the composite map whose compilation follows this drawable's visibility.
        void setCompositeMap(CompositeMap* map)
        {
            mCompositeMap = map;
            mCompositeMapRequested.store(false, std::memory_order_relaxed);
        }
        CompositeMap* getCompositeMap() const { return mCompositeMap; }
        /// Select the renderer that owns the composite map's pending work.
        void setCompositeMapRenderer(CompositeMapRenderer* renderer) { mCompositeMapRenderer = renderer; }

        /// Promote the composite map for compilation when this terrain representation becomes visible.
        void requireCompositeMap();

    private:
        osg::BoundingBox mWaterBoundingBox;
        PassVector mPasses;

        osg::ref_ptr<osg::ClusterCullingCallback> mClusterCullingCallback;

        osg::ref_ptr<SceneUtil::LightListCallback> mLightListCallback;
        osg::ref_ptr<CompositeMap> mCompositeMap;
        osg::ref_ptr<CompositeMapRenderer> mCompositeMapRenderer;
        std::atomic_bool mCompositeMapRequested{ false };
    };

}

#endif
