#ifndef OPENMW_MWRENDER_GPUSCENENODESINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENENODESINTERNAL_H

namespace MWRender
{
    class GpuSceneNode : public osg::Group
    {
    public:
        GpuSceneNode() = default;
        GpuSceneNode(const GpuSceneNode& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Group(copy, copyop)
            , mScene(copy.mScene.load(std::memory_order_relaxed))
        {
        }
        META_Node(MWRender, GpuSceneNode)

        explicit GpuSceneNode(GpuScene* scene)
            : mScene(scene)
        {
            setUpdateCallback(new UpdateCallback);
        }

        void setScene(GpuScene* scene) { mScene.store(scene, std::memory_order_release); }

        void traverse(osg::NodeVisitor& nv) override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene != nullptr && nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
                scene->recordView(static_cast<osgUtil::CullVisitor&>(nv));
            osg::Group::traverse(nv);
        }

        osg::BoundingSphere computeBound() const override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            return scene != nullptr ? scene->getResidentBound() : osg::BoundingSphere();
        }

    private:
        struct UpdateCallback : osg::NodeCallback
        {
            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                GpuSceneNode& self = static_cast<GpuSceneNode&>(*node);
                if (GpuScene* const scene = self.mScene.load(std::memory_order_acquire))
                    scene->update(nv->getFrameStamp()->getFrameNumber());
                traverse(node, nv);
            }
        };

        std::atomic<GpuScene*> mScene{ nullptr };
    };

    class GpuChunkNode : public osg::Node
    {
    public:
        GpuChunkNode() = default;
        GpuChunkNode(const GpuChunkNode& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Node(copy, copyop)
            , mScene(nullptr)
            , mLit(copy.mLit)
            , mBound(copy.mBound)
        {
        }
        META_Node(MWRender, GpuChunkNode)

        GpuChunkNode(
            GpuScene& scene, GpuScene::Id slot, GpuScene::Id generation, bool lit, const osg::BoundingSphere& bound)
            : mScene(&scene)
            , mSlot(slot)
            , mGeneration(generation)
            , mLit(lit)
            , mBound(bound)
        {
        }

        ~GpuChunkNode() override
        {
            if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                scene->releaseChunk(*this, mSlot);
        }

        void detach() { mScene.store(nullptr, std::memory_order_release); }
        GpuScene::Id getSlot() const { return mSlot; }
        GpuScene::Id getGeneration() const { return mGeneration; }
        bool isLit() const { return mLit; }
        /// Retain terrain visibility policy while its draw data lives in the GPU scene.
        void setTerrainDrawable(Terrain::TerrainDrawable* drawable) { mTerrainDrawable = drawable; }
        void setBound(const osg::BoundingSphere& bound)
        {
            mBound = bound;
            dirtyBound();
        }
        static constexpr std::size_t sNone = std::numeric_limits<std::size_t>::max();
        std::size_t mResident = sNone;

        void traverse(osg::NodeVisitor& nv) override
        {
            GpuScene* const scene = mScene.load(std::memory_order_acquire);
            if (scene == nullptr || nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
                return;
            osgUtil::CullVisitor& cv = static_cast<osgUtil::CullVisitor&>(nv);
            if (mTerrainDrawable != nullptr && cv.getCurrentCamera()->getName() != "ShadowCamera")
                mTerrainDrawable->requireCompositeMap();
            scene->markChunk(cv, mSlot, mGeneration, mLit);
            if (cv.getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR && mBound.valid())
            {
                osg::BoundingBox box;
                box.expandBy(mBound);
                cv.updateCalculatedNearFar(*cv.getModelViewMatrix(), box);
            }
        }

        osg::BoundingSphere computeBound() const override { return mBound; }

    private:
        std::atomic<GpuScene*> mScene{ nullptr };
        GpuScene::Id mSlot = 0;
        GpuScene::Id mGeneration = 0;
        bool mLit = false;
        osg::BoundingSphere mBound;
        osg::ref_ptr<Terrain::TerrainDrawable> mTerrainDrawable;
    };

    class DispatchDrawable : public osg::Drawable
    {
    public:
        DispatchDrawable() = default;
        DispatchDrawable(const DispatchDrawable& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Drawable(copy, copyop)
            , mScene(copy.mScene.load(std::memory_order_relaxed))
            , mDispatcher(copy.mDispatcher)
        {
        }
        META_Node(MWRender, DispatchDrawable)

        DispatchDrawable(GpuScene& scene, GpuScene::Id dispatcher)
            : mScene(&scene)
            , mDispatcher(dispatcher)
        {
            setCullingActive(false);
            setSupportsDisplayList(false);
            setUseVertexArrayObject(false);
        }

        osg::BoundingBox computeBoundingBox() const override { return osg::BoundingBox(); }

        void drawImplementation(osg::RenderInfo& renderInfo) const override
        {
            if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                scene->draw(renderInfo, mDispatcher);
        }

        void releaseGLObjects(osg::State* state) const override
        {
            if (GpuScene* const scene = mScene.load(std::memory_order_acquire))
                scene->releaseGLObjects(state);
            osg::Drawable::releaseGLObjects(state);
        }

        void detach() { mScene.store(nullptr, std::memory_order_release); }

    private:
        std::atomic<GpuScene*> mScene{ nullptr };
        GpuScene::Id mDispatcher = 0;
    };

    /// One object per sampler key.
    class GpuSampler : public osg::Sampler
    {
    public:
        GLuint object(unsigned int context) const { return _PCsampler[context]; }

        void compileGLObjects(osg::State& state) const override;

        // osg::Sampler compares every instance equal.
        int compare(const osg::StateAttribute& sa) const override
        {
            COMPARE_StateAttribute_Types(GpuSampler, sa);
            if (this < &rhs)
                return -1;
            if (this > &rhs)
                return 1;
            return 0;
        }

    protected:
        ~GpuSampler() override = default;
    };

    /// Unbinds the unit when array states pop.
    class SamplerReset final : public osg::Sampler
    {
    public:
        void apply(osg::State& state) const override
        {
            osg::GLExtensions* ext = state.get<osg::GLExtensions>();
            if (ext != nullptr && ext->glBindSampler != nullptr)
                ext->glBindSampler(state.getActiveTextureUnit(), 0);
        }

    protected:
        ~SamplerReset() override = default;
    };

    /// Raw array texture unit.
    constexpr unsigned int sArrayUnit = 31;

    /// Units the reset sampler covers.
    constexpr unsigned int sSamplerUnits = 16;

    inline void clearErrors()
    {
        for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        {
        }
    }

    inline void resetUnpack()
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#if !defined(OSG_GLES1_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
#endif
    }
}

#endif
