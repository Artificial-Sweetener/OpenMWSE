#ifndef OPENMW_MWRENDER_PAGEDLIGHT_H
#define OPENMW_MWRENDER_PAGEDLIGHT_H

#include <vector>

#include <osg/BoundingSphere>
#include <osg/Node>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>

namespace ESM
{
    struct Light;
}

namespace SceneUtil
{
    class LightManager;
    class LightSource;
}

namespace MWRender
{
    class GpuScene;

    /// Convert a paged reference's source rotation to its renderer-space attitude.
    osg::Quat pagedObjectAttitude(const osg::Vec3f& rotation);

    /// Publish visible exterior lights from one cached object-paging chunk.
    class DistantLightBatch : public osg::Node
    {
    public:
        /// Construct an empty detached batch for OSG cloning.
        DistantLightBatch() = default;

        /// Copy entries and non-owning renderer targets for OSG cloning.
        DistantLightBatch(const DistantLightBatch& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);

        META_Node(MWRender, DistantLightBatch)

        /// Bind the batch to live GPU-scene and clustered-light lifecycles.
        DistantLightBatch(SceneUtil::LightManager* lightManager, GpuScene* gpuScene);

        /// Build and add one light using its chunk-relative source transform.
        void addLight(const ESM::Light& record, ESM::RefNum refNum, const osg::Vec3f& position,
            const osg::Vec3f& rotation, float scale, const osg::Vec3f& worldCenter, const osg::Node* model);

        /// Return whether the batch contains no lights.
        bool empty() const;

        /// Register visible lights only for the primary scene camera and active optimized renderer.
        void traverse(osg::NodeVisitor& visitor) override;

        /// Return the union of chunk-local light bounds.
        osg::BoundingSphere computeBound() const override;

    private:
        struct Entry
        {
            osg::ref_ptr<SceneUtil::LightSource> mLight;
            osg::BoundingSphere mBound;
        };

        std::vector<Entry> mEntries;
        osg::BoundingSphere mBound;
        SceneUtil::LightManager* mLightManager = nullptr;
        GpuScene* mGpuScene = nullptr;
    };
}

#endif
