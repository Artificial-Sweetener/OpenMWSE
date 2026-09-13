#ifndef OPENMW_MWRENDER_GPUSCENECAPTUREINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENECAPTUREINTERNAL_H

namespace MWRender
{
    class GpuCaptureCallback : public osg::DrawableCullCallback
    {
    public:
        GpuCaptureCallback(GpuScene& scene, osg::Callback* previous, bool updateMesh)
            : mScene(&scene)
            , mPrevious(previous)
            , mUpdateMesh(updateMesh)
        {
        }

        bool cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* renderInfo) const override
        {
            if (mPrevious != nullptr)
            {
                const osg::DrawableCullCallback* callback = mPrevious->asDrawableCullCallback();
                if (callback != nullptr && callback->cull(nv, drawable, renderInfo))
                    return true;
            }
            osgUtil::CullVisitor* cv = nv->asCullVisitor();
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene == nullptr || !scene->isEnabled() || cv == nullptr)
                return false;
            if (drawable->isCullingActive() && cv->isCulled(drawable->getBoundingBox()))
                return true;
            if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable))
            {
                if (!scene->recordRig(*cv, *rig, *this))
                    return false;
                if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR
                    && rig->getBoundingBox().valid())
                    cv->updateCalculatedNearFar(*cv->getModelViewMatrix(), *rig);
                return true;
            }
            osg::Geometry* geometry = drawable->asGeometry();
            if (geometry == nullptr || !scene->record(*cv, *geometry, mUpdateMesh, *this))
                return false;
            if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR
                && geometry->getBoundingBox().valid())
                cv->updateCalculatedNearFar(*cv->getModelViewMatrix(), *geometry);
            return true;
        }

        /// Drawables die under the scene lock too.
        ~GpuCaptureCallback() override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene == nullptr)
                return;
            for (const Cache& cache : mCache)
                if (cache.mMesh != GpuScene::sInvalid)
                    scene->queueMeshRelease(cache.mMesh);
        }

        void detach() { mScene.store(nullptr, std::memory_order_release); }
        void resetPath() const { mLiveMaterial.store(-1, std::memory_order_relaxed); }
        const osg::Callback* getWrapped() const { return mPrevious.get(); }

    private:
        friend class GpuScene;

        struct Cache
        {
            const osg::Geometry* mGeometry = nullptr;
            std::vector<osg::ref_ptr<const osg::StateSet>> mPath;
            GpuScene::Id mMesh = GpuScene::sInvalid;
            GpuScene::Surface mSurface;
        };

        std::pair<GpuScene::Id, GpuScene::Surface> resolveLocked(osg::Geometry& geometry,
            const std::vector<const osg::StateSet*>& path, unsigned int frame, bool liveMaterial) const;
        void adoptMeshLocked(Cache& cache, GpuScene::Id mesh) const;
        bool hasLiveMaterial(const osgUtil::CullVisitor& cv) const;

        std::atomic<GpuScene*> mScene;
        osg::ref_ptr<osg::Callback> mPrevious;
        bool mUpdateMesh;
        mutable std::array<Cache, 4> mCache;
        mutable unsigned int mNextCache = 0;
        mutable std::atomic<int> mLiveMaterial{ -1 };
    };
}

#endif
