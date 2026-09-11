#ifndef OPENMW_MWRENDER_GROUNDCOVER_H
#define OPENMW_MWRENDER_GROUNDCOVER_H

#include <components/esm3/loadcell.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/vfs/pathutil.hpp>

namespace MWWorld
{
    class ESMStore;
    class GroundcoverStore;
}
namespace osg
{
    class Program;
}

namespace MWRender
{
    class GroundcoverDistance;
    typedef std::tuple<osg::Vec2f, float> GroundcoverChunkId; // Center, Size
    class Groundcover : public Resource::GenericResourceManager<GroundcoverChunkId>,
                        public Terrain::QuadTreeWorld::ChunkManager
    {
    public:
        /// Create immutable instance resources with a shared live distance for all cached chunks.
        Groundcover(Resource::SceneManager* sceneManager, float density, float viewDistance,
            const MWWorld::GroundcoverStore& store);
        ~Groundcover();

        /// Update existing chunks' visibility and shading after render threads and preload work have stopped.
        void updateDistance(float distance);

        osg::ref_ptr<osg::Node> getChunk(float size, const osg::Vec2f& center, unsigned char lod, unsigned int lodFlags,
            bool activeGrid, const osg::Vec3f& viewPoint, bool compile) override;

        unsigned int getNodeMask() override;

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        struct GroundcoverEntry
        {
            ESM::Position mPos;
            float mScale;

            GroundcoverEntry(const ESM::CellRef& ref)
                : mPos(ref.mPos)
                , mScale(ref.mScale)
            {
            }
        };

    private:
        using InstanceMap = std::map<VFS::Path::Normalized, std::vector<GroundcoverEntry>, std::less<>>;

        Resource::SceneManager* mSceneManager;
        float mDensity;
        osg::ref_ptr<GroundcoverDistance> mDistance;
        osg::ref_ptr<osg::StateSet> mStateset;
        osg::ref_ptr<osg::Program> mProgramTemplate;
        const MWWorld::GroundcoverStore& mGroundcoverStore;

        /// Build both LOD representations and attach live distance state outside the material-sharing cache.
        osg::ref_ptr<osg::Node> createChunk(InstanceMap& instances, const osg::Vec2f& center);
        void collectInstances(InstanceMap& instances, float size, const osg::Vec2f& center);
    };
}

#endif
