#ifndef OPENMW_MWRENDER_OBJECTPAGING_H
#define OPENMW_MWRENDER_OBJECTPAGING_H

#include <components/esm3/refnum.hpp>
#include <components/resource/resourcemanager.hpp>
#include <components/terrain/quadtreeworld.hpp>

#include <memory>
#include <mutex>

#include <osg/observer_ptr>

#include "pagedref.hpp"

namespace Resource
{
    class SceneManager;
}

namespace SceneUtil
{
    class LightManager;
}

namespace MWRender
{
    class GpuScene;
    struct GpuTemplate;

    typedef std::tuple<osg::Vec2f, float, bool> ChunkId; // Center, Size, ActiveGrid

    class ObjectPaging : public Resource::GenericResourceManager<ChunkId>, public Terrain::QuadTreeWorld::ChunkManager
    {
    public:
        /// Create object paging with optional GPU geometry and distant-light targets.
        ObjectPaging(Resource::SceneManager* sceneManager, ESM::RefId worldspace, GpuScene* gpuScene,
            SceneUtil::LightManager* distantLights);
        ~ObjectPaging() = default;

        /// Expire CPU chunks and source-node GPU templates at their shared cache cadence.
        void updateCache(double referenceTime) override;
        /// Clear both CPU chunks and cached GPU extraction templates.
        void clearCache() override;
        /// Replace the GPU geometry and distant-light targets used by subsequently built chunks.
        void setGpuScene(GpuScene* gpuScene, SceneUtil::LightManager* distantLights)
        {
            mGpuScene = gpuScene;
            mDistantLights = distantLights;
        }
        osg::ref_ptr<osg::Node> getChunk(float size, const osg::Vec2f& center, unsigned char lod, unsigned int lodFlags,
            bool activeGrid, const osg::Vec3f& viewPoint, bool compile) override;

        /// Create a chunk and report any GPU geometry that requires an uncached retry.
        osg::ref_ptr<osg::Node> createChunk(float size, const osg::Vec2f& center, bool activeGrid,
            const osg::Vec3f& viewPoint, bool compile, unsigned char lod, bool* gpuFailed = nullptr,
            std::vector<PagedRefKey>* pagedRefs = nullptr);

        unsigned int getNodeMask() override;

        /// @return true if view needs rebuild
        bool enableObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell, bool enabled);

        /// @return true if view needs rebuild
        bool blacklistObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell);

        void clear();

        /// Must be called after clear() before rendering starts.
        /// @return true if view needs rebuild
        bool unlockCache();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        /// Append the cell-qualified references represented by active-grid paging chunks.
        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<PagedRefKey>& out);

    private:
        /// Return a shared GPU extraction template so callers retain replaced material-array state while building.
        std::shared_ptr<const GpuTemplate> getGpuTemplate(
            const osg::Node& node, osg::Node::NodeMask mask, bool activeGrid);

        /// Force the next lookup for a source model to extract a fresh GPU template.
        void dropGpuTemplate(const osg::Node& node, bool activeGrid);

        /// Remove cached GPU templates whose source model has expired.
        void sweepGpuTemplates();

        Resource::SceneManager* mSceneManager;
        GpuScene* mGpuScene;
        SceneUtil::LightManager* mDistantLights;
        std::mutex mGpuTemplateMutex;
        std::map<std::pair<const osg::Node*, bool>,
            std::pair<osg::observer_ptr<const osg::Node>, std::shared_ptr<const GpuTemplate>>>
            mGpuTemplates;
        double mNextTemplateSweep = 0;
        bool mActiveGrid;
        bool mDebugBatches;
        float mMergeFactor;
        float mMinSize;
        float mMinSizeMergeFactor;
        float mMinSizeCostMultiplier;

        std::mutex mRefTrackerMutex;
        struct RefTracker
        {
            std::set<PagedRefKey> mDisabled;
            std::set<PagedRefKey> mBlacklist;
            bool operator==(const RefTracker& other) const
            {
                return mDisabled == other.mDisabled && mBlacklist == other.mBlacklist;
            }
        };
        RefTracker mRefTracker;
        RefTracker mRefTrackerNew;
        bool mRefTrackerLocked;

        std::mutex mIncompleteMutex;
        std::map<ChunkId, std::vector<PagedRefKey>> mIncompleteRefnums;

        const RefTracker& getRefTracker() const { return mRefTracker; }
        RefTracker& getWritableRefTracker() { return mRefTrackerLocked ? mRefTrackerNew : mRefTracker; }

        std::mutex mSizeCacheMutex;
        typedef std::map<PagedRefKey, float> SizeCache;
        SizeCache mSizeCache;

        std::mutex mLODNameCacheMutex;
        typedef std::pair<std::string, unsigned char> LODNameCacheKey; // Key: mesh name, lod level
        using LODNameCache = std::map<LODNameCacheKey, VFS::Path::Normalized>; // Cache: key, mesh name to use
        LODNameCache mLODNameCache;
    };

    /// Associates paged geometry with its cell-qualified source reference for renderer queries.
    class RefnumMarker : public osg::Object
    {
    public:
        RefnumMarker()
            : mNumVertices(0)
        {
        }
        /// Copy the source identity and geometry span into a cloned marker.
        RefnumMarker(const RefnumMarker& copy, osg::CopyOp co)
            : mRef(copy.mRef)
            , mNumVertices(copy.mNumVertices)
        {
        }
        META_Object(MWRender, RefnumMarker)

        PagedRefKey mRef;
        unsigned int mNumVertices;
    };
}

#endif
