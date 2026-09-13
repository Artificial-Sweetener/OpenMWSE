#include "groundcover.hpp"

#include "groundcoverbatching.hpp"
#include "groundcoverdistance.hpp"
#include "groundcoverinstances.hpp"
#include "groundcoverlod.hpp"
#include "groundcovernearfar.hpp"

#include <limits>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/Program>
#include <osg/VertexAttribDivisor>

#include <components/debug/debuglog.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/misc/convert.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/quadtreenode.hpp>

#include "../mwworld/groundcoverstore.hpp"

#include "gpuscene.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        /// Build the world transform consumed by the GPU scene for one groundcover instance.
        osg::Matrixf computeGpuInstanceMatrix(const Groundcover::GroundcoverEntry& entry)
        {
            return osg::Matrixf::scale(entry.mScale, entry.mScale, entry.mScale)
                * osg::Matrixf(Misc::Convert::makeOsgQuat(entry.mPos)) * osg::Matrixf::translate(entry.mPos.asVec3());
        }

        class InstancingVisitor : public osg::NodeVisitor
        {
        public:
            /// Configure cloned model geometry for one chunk's immutable instance collection.
            InstancingVisitor(const GroundcoverInstanceBatch& instances, GroundcoverBatch& drawBatch)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mInstances(instances)
                , mDrawBatch(drawBatch)
            {
            }

            void apply(osg::Group& group) override
            {
                for (unsigned int i = 0; i < group.getNumChildren();)
                {
                    if (group.getChild(i)->asDrawable() && !group.getChild(i)->asGeometry())
                        group.removeChild(i);
                    else
                        ++i;
                }
                traverse(group);
            }

            void apply(osg::Geometry& geom) override
            {
                for (unsigned int i = 0; i < geom.getNumPrimitiveSets(); ++i)
                {
                    geom.getPrimitiveSet(i)->setNumInstances(static_cast<int>(mInstances.mTransforms->size()));
                }

                osg::BoundingBox originalBox = geom.getBoundingBox();
                geom.setInitialBound(mInstances.mBounds);

                // Display lists do not support instancing in OSG 3.4
                geom.setUseDisplayList(false);
                geom.setUseVertexBufferObjects(true);

                geom.setVertexAttribArray(6, mInstances.mTransforms.get(), osg::Array::BIND_PER_VERTEX);
                geom.setVertexAttribArray(7, mInstances.mRotations.get(), osg::Array::BIND_PER_VERTEX);

                geom.addCullCallback(
                    new GroundcoverNearFarCullCallback(mInstances.mTransforms, mInstances.mRotations, originalBox));
                mDrawBatch.configure(geom);
            }

        private:
            const GroundcoverInstanceBatch& mInstances;
            GroundcoverBatch& mDrawBatch;
        };

        class DensityCalculator
        {
        public:
            DensityCalculator(float density)
                : mDensity(density)
            {
            }

            bool isInstanceEnabled()
            {
                if (mDensity >= 1.f)
                    return true;

                mCurrentGroundcover += mDensity;
                if (mCurrentGroundcover < 1.f)
                    return false;

                mCurrentGroundcover -= 1.f;

                return true;
            }
            void reset() { mCurrentGroundcover = 0.f; }

        private:
            float mCurrentGroundcover = 0.f;
            float mDensity = 0.f;
        };

        class ViewDistanceCallback : public SceneUtil::NodeCallback<ViewDistanceCallback>
        {
        public:
            /// Retain the current world-unit distance for this chunk's coarse culling boundary.
            ViewDistanceCallback(GroundcoverDistance* distance, const osg::BoundingBox& box)
                : mDistance(distance)
                , mBox(box)
            {
            }
            /// Traverse visible chunks using the same limit as batch culling and shader fading.
            void operator()(osg::Node* node, osg::NodeVisitor* nv)
            {
                if (mDistance->get() > 0 && Terrain::distance(mBox, nv->getEyePoint()) <= mDistance->get())
                    traverse(node, nv);
            }

        private:
            osg::ref_ptr<GroundcoverDistance> mDistance;
            osg::BoundingBox mBox;
        };

        inline bool isInChunkBorders(ESM::CellRef& ref, osg::Vec2f& minBound, osg::Vec2f& maxBound)
        {
            osg::Vec2f size = maxBound - minBound;
            if (size.x() >= 1 && size.y() >= 1)
                return true;

            osg::Vec3f pos = ref.mPos.asVec3();
            osg::Vec3f cellPos = pos / ESM::Land::REAL_SIZE;
            if ((minBound.x() > std::floor(minBound.x()) && cellPos.x() < minBound.x())
                || (minBound.y() > std::floor(minBound.y()) && cellPos.y() < minBound.y())
                || (maxBound.x() < std::ceil(maxBound.x()) && cellPos.x() >= maxBound.x())
                || (maxBound.y() < std::ceil(maxBound.y()) && cellPos.y() >= maxBound.y()))
                return false;

            return true;
        }
    }

    osg::ref_ptr<osg::Node> Groundcover::getChunk(float size, const osg::Vec2f& center, unsigned char lod,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {
        if (lod > getMaxLodLevel())
            return nullptr;
        GroundcoverChunkId id = std::make_tuple(center, size);
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id);
        if (obj)
            return static_cast<osg::Node*>(obj.get());
        else
        {
            InstanceMap instances;
            collectInstances(instances, size, center);
            osg::ref_ptr<osg::Node> node = createChunk(instances, center);
            mCache->addEntryToObjectCache(id, node.get());
            return node;
        }
    }

    Groundcover::Groundcover(Resource::SceneManager* sceneManager, float density, float viewDistance,
        const MWWorld::GroundcoverStore& store, GpuScene* gpuScene)
        : GenericResourceManager<GroundcoverChunkId>(nullptr, Settings::cells().mCacheExpiryDelay)
        , mSceneManager(sceneManager)
        , mDensity(density)
        , mDistance(new GroundcoverDistance(viewDistance))
        , mStateset(new osg::StateSet)
        , mGroundcoverStore(store)
        , mGpuScene(gpuScene)
    {
        setViewDistance(viewDistance);
        // MGE uses default alpha settings for groundcover, so we can not rely on alpha properties
        // Force a unified alpha handling instead of data from meshes
        osg::ref_ptr<osg::AlphaFunc> alpha = new osg::AlphaFunc(osg::AlphaFunc::GEQUAL, 128.f / 255.f);
        mStateset->setAttributeAndModes(alpha.get(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        mStateset->setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        mStateset->setRenderBinDetails(0, "RenderBin", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
        mStateset->setAttribute(new osg::VertexAttribDivisor(6, 1));
        mStateset->setAttribute(new osg::VertexAttribDivisor(7, 1));

        mProgramTemplate = mSceneManager->getShaderManager().getProgramTemplate()
            ? Shader::ShaderManager::cloneProgram(mSceneManager->getShaderManager().getProgramTemplate())
            : osg::ref_ptr<osg::Program>(new osg::Program);
        mProgramTemplate->addBindAttribLocation("aOffset", 6);
        mProgramTemplate->addBindAttribLocation("aRotation", 7);
        if (mGpuScene != nullptr)
            mGpuScene->setGroundcoverDistances(mDistance->getFarLodDistance(), mDistance->get());
    }

    Groundcover::~Groundcover() = default;

    void Groundcover::updateDistance(float distance)
    {
        mDistance->set(distance);
        if (mGpuScene != nullptr)
            mGpuScene->setGroundcoverDistances(mDistance->getFarLodDistance(), mDistance->get());
    }

    void Groundcover::setGpuScene(GpuScene* gpuScene)
    {
        mGpuScene = gpuScene;
        if (mGpuScene != nullptr)
            mGpuScene->setGroundcoverDistances(mDistance->getFarLodDistance(), mDistance->get());
    }

    void Groundcover::collectInstances(InstanceMap& instances, float size, const osg::Vec2f& center)
    {
        if (mDensity <= 0.f)
            return;

        osg::Vec2f minBound = (center - osg::Vec2f(size / 2.f, size / 2.f));
        osg::Vec2f maxBound = (center + osg::Vec2f(size / 2.f, size / 2.f));
        DensityCalculator calculator(mDensity);
        ESM::ReadersCache readers;
        osg::Vec2i startCell = osg::Vec2i(static_cast<int>(std::floor(center.x() - size / 2.f)),
            static_cast<int>(std::floor(center.y() - size / 2.f)));
        for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
        {
            for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
            {
                ESM::Cell cell;
                mGroundcoverStore.initCell(cell, cellX, cellY);
                if (cell.mContextList.empty())
                    continue;

                calculator.reset();
                std::map<ESM::RefNum, ESM::CellRef> refs;
                for (size_t i = 0; i < cell.mContextList.size(); ++i)
                {
                    const std::size_t index = static_cast<std::size_t>(cell.mContextList[i].index);
                    const ESM::ReadersCache::BusyItem reader = readers.get(index);
                    cell.restore(*reader, i);
                    ESM::CellRef ref;
                    bool deleted = false;
                    while (cell.getNextRef(*reader, ref, deleted))
                    {
                        if (!deleted && refs.find(ref.mRefNum) == refs.end() && !calculator.isInstanceEnabled())
                            deleted = true;
                        if (!deleted && !isInChunkBorders(ref, minBound, maxBound))
                            deleted = true;

                        if (deleted)
                        {
                            refs.erase(ref.mRefNum);
                            continue;
                        }
                        refs[ref.mRefNum] = std::move(ref);
                    }
                }

                for (auto& [refNum, cellRef] : refs)
                {
                    const VFS::Path::NormalizedView model = mGroundcoverStore.getGroundcoverModel(cellRef.mRefID);
                    if (model.empty())
                        continue;
                    auto it = instances.find(model);
                    if (it == instances.end())
                        it = instances.emplace_hint(it, VFS::Path::Normalized(model), std::vector<GroundcoverEntry>());
                    it->second.emplace_back(std::move(cellRef));
                }
            }
        }
    }

    void Groundcover::clearCache()
    {
        GenericResourceManager<GroundcoverChunkId>::clearCache();
        std::lock_guard<std::mutex> lock(mGpuTemplateMutex);
        mGpuTemplates.clear();
    }

    Groundcover::GpuModel Groundcover::lookupGpuModel(const osg::Node& temp)
    {
        std::lock_guard<std::mutex> lock(mGpuTemplateMutex);
        const auto found = mGpuTemplates.find(&temp);
        if (found != mGpuTemplates.end())
        {
            osg::ref_ptr<const osg::Node> alive;
            // Retired materials void the cached ids.
            if (found->second.mNode.lock(alive) && alive == &temp
                && (found->second.mTemplate == nullptr || mGpuScene->isTemplateLive(*found->second.mTemplate)))
                return found->second;
            mGpuTemplates.erase(found);
        }
        GpuModel model;
        model.mNode = &temp;
        osg::ref_ptr<osg::Node> node
            = static_cast<osg::Node*>(temp.clone(osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
                | osg::CopyOp::DEEP_COPY_USERDATA | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES));
        constexpr float farTriangleRatio = 0.25f;
        osg::ref_ptr<osg::Node> farNode = makeGroundcoverFarLod(*node, farTriangleRatio);
        osg::ref_ptr<osg::LOD> lod = new osg::LOD;
        const float lodDistance = mDistance->getFarLodDistance();
        lod->addChild(node, 0.f, lodDistance);
        lod->addChild(farNode, lodDistance, std::numeric_limits<float>::max());
        model.mRoot = new osg::Group;
        model.mRoot->setStateSet(mStateset);
        model.mRoot->addChild(lod);
        mSceneManager->recreateShaders(model.mRoot, "groundcover", mProgramTemplate);
        mSceneManager->shareState(model.mRoot);
        // GPU buckets are extracted before the chunk enters the terrain graph, so capture the live fade uniform here.
        osg::ref_ptr<osg::StateSet> distanceState = new osg::StateSet(*model.mRoot->getOrCreateStateSet());
        mDistance->attachFadeUniform(*distanceState);
        distanceState->setDataVariance(osg::Object::STATIC);
        model.mRoot->setStateSet(distanceState);
        model.mTemplate = mGpuScene->extract(*model.mRoot, ~0u, false);
        mGpuTemplates.emplace(&temp, model);
        return model;
    }

    osg::ref_ptr<osg::Node> Groundcover::createChunk(InstanceMap& instances, const osg::Vec2f& center)
    {
        osg::ref_ptr<osg::Group> group = new osg::Group;
        osg::Vec3f worldCenter = osg::Vec3f(center.x(), center.y(), 0) * ESM::Land::REAL_SIZE;
        std::vector<GpuScene::Instance> gpuInstances;
        struct GpuSource
        {
            osg::ref_ptr<osg::Group> mRoot;
            osg::ref_ptr<const osg::Node> mModel;
            const std::vector<GroundcoverEntry>* mEntries = nullptr;
        };
        std::vector<GpuSource> gpuRoots;
        const auto appendModel
            = [&](const GpuTemplate& gpuTemplate, const std::vector<GroundcoverEntry>& modelEntries) {
                  for (const GroundcoverEntry& entry : modelEntries)
                  {
                      const osg::Matrixf matrix(computeGpuInstanceMatrix(entry));
                      mGpuScene->appendGroundcoverInstances(gpuTemplate, matrix, entry.mScale, gpuInstances);
                  }
              };
        const auto addInstancedCopy = [&](const osg::Node& source, const std::vector<GroundcoverEntry>& entries) {
            osg::ref_ptr<osg::Node> node = static_cast<osg::Node*>(source.clone(osg::CopyOp::DEEP_COPY_NODES
                | osg::CopyOp::DEEP_COPY_DRAWABLES | osg::CopyOp::DEEP_COPY_USERDATA | osg::CopyOp::DEEP_COPY_ARRAYS
                | osg::CopyOp::DEEP_COPY_PRIMITIVES));

            constexpr float clusterSizeInCells = 4.f;
            const GroundcoverInstanceLayout layout
                = makeGroundcoverInstanceLayout(entries, ESM::Land::REAL_SIZE * clusterSizeInCells);
            const osg::BoundingSphere modelBounds = node->getBound();
            const float modelExtent = modelBounds.valid() ? modelBounds.center().length() + modelBounds.radius() : 0.f;
            GroundcoverInstanceBatch batch = makeGroundcoverInstanceBatch(entries, layout, worldCenter, modelExtent);
            constexpr float farTriangleRatio = 0.25f;
            osg::ref_ptr<osg::Node> farNode = makeGroundcoverFarLod(*node, farTriangleRatio);

            osg::ref_ptr<GroundcoverBatch> detailedBatch
                = new GroundcoverBatch(batch.mClusters, batch.mTransforms, modelExtent, mDistance);
            InstancingVisitor detailedVisitor(batch, *detailedBatch);
            node->accept(detailedVisitor);
            detailedBatch->attach(*node);

            osg::ref_ptr<GroundcoverBatch> farBatch
                = new GroundcoverBatch(batch.mClusters, batch.mTransforms, modelExtent, mDistance);
            InstancingVisitor farVisitor(batch, *farBatch);
            farNode->accept(farVisitor);
            farBatch->attach(*farNode);

            osg::ref_ptr<GroundcoverLod> lod = new GroundcoverLod(mDistance);
            lod->addChild(node);
            lod->addChild(farNode);
            group->addChild(lod);
        };
        for (const auto& [model, entries] : instances)
        {
            const osg::Node* temp = mSceneManager->getTemplate(model);
            // Keep link to original mesh to keep it in cache
            group->getOrCreateUserDataContainer()->addUserObject(new Resource::TemplateRef(temp));

            if (mGpuScene != nullptr && mGpuScene->isEnabled())
            {
                const GpuModel gpuModel = lookupGpuModel(*temp);
                if (gpuModel.mTemplate != nullptr && !gpuModel.mTemplate->mLegacy)
                {
                    appendModel(*gpuModel.mTemplate, entries);
                    gpuRoots.push_back(GpuSource{ gpuModel.mRoot, temp, &entries });
                    continue;
                }
            }
            addInstancedCopy(*temp, entries);
        }

        osg::BoundingSphere gpuBound;
        if (!gpuInstances.empty())
        {
            const auto buildChunk = [&] {
                osg::BoundingSphere localBound;
                for (const GpuScene::Instance& instance : gpuInstances)
                    localBound.expandBy(
                        osg::BoundingSphere(instance.mBounds.center() - worldCenter, instance.mBounds.radius()));
                osg::ref_ptr<osg::Node> chunk = mGpuScene->createChunk(std::move(gpuInstances), localBound,
                    Settings::groundcover().mPointLighting, GpuScene::ChunkKind::Groundcover);
                if (chunk != nullptr)
                    gpuBound = localBound;
                return chunk;
            };
            osg::ref_ptr<osg::Node> gpuChunk = buildChunk();
            if (gpuChunk == nullptr)
            {
                // A material or mesh retired during the build.
                gpuInstances.clear();
                std::vector<GpuSource> live;
                for (const GpuSource& source : gpuRoots)
                {
                    const std::unique_ptr<const GpuTemplate> fresh = mGpuScene->extract(*source.mRoot, ~0u, false);
                    if (fresh != nullptr && !fresh->mLegacy)
                    {
                        appendModel(*fresh, *source.mEntries);
                        live.push_back(source);
                    }
                    else
                        addInstancedCopy(*source.mModel, *source.mEntries);
                }
                gpuRoots = std::move(live);
                if (!gpuInstances.empty())
                    gpuChunk = buildChunk();
                if (gpuChunk == nullptr && !gpuRoots.empty())
                {
                    Log(Debug::Warning) << "Groundcover GPU chunk failed; drawing plants directly.";
                    for (const GpuSource& source : gpuRoots)
                        addInstancedCopy(*source.mModel, *source.mEntries);
                }
            }
            if (gpuChunk != nullptr)
                group->addChild(gpuChunk);
        }

        osg::ComputeBoundsVisitor cbv;
        group->accept(cbv);
        osg::BoundingBox box = cbv.getBoundingBox();
        if (gpuBound.valid())
            box.expandBy(gpuBound);
        group->addCullCallback(new ViewDistanceCallback(mDistance, box));

        group->setStateSet(mStateset);
        group->setNodeMask(Mask_Groundcover);
        if (Settings::groundcover().mPointLighting)
            group->addCullCallback(new SceneUtil::LightListCallback);
        mSceneManager->recreateShaders(group, "groundcover", mProgramTemplate);
        mSceneManager->shareState(group);
        // Keep the editable uniform out of the shared-state cache's value-based ordering.
        osg::ref_ptr<osg::StateSet> distanceState = new osg::StateSet(*group->getStateSet());
        mDistance->attachFadeUniform(*distanceState);
        distanceState->setDataVariance(osg::Object::STATIC);
        group->setStateSet(distanceState);
        group->getBound();
        return group;
    }

    unsigned int Groundcover::getNodeMask()
    {
        return Mask_Groundcover;
    }

    void Groundcover::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Groundcover Chunk", frameNumber, mCache->getStats(), *stats);
    }
}
