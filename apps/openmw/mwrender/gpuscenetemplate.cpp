#include "gpusceneinternal.hpp"

#include <components/sceneutil/deformationstats.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace MWRender
{
    osg::ref_ptr<osg::Node> GpuScene::createTerrainChunk(
        Terrain::TerrainDrawable& drawable, const osg::Vec3f& worldOffset, bool lit)
    {
        if (!isEnabled() || drawable.getPasses().empty())
            return nullptr;
        const Id mesh = getMesh(drawable);
        if (mesh == sInvalid)
            return nullptr;
        std::vector<Instance> instances;
        const Terrain::TerrainDrawable::PassVector& passes = drawable.getPasses();
        for (std::size_t passIndex = 0; passIndex < passes.size(); ++passIndex)
        {
            const osg::ref_ptr<osg::StateSet>& pass = passes[passIndex];
            std::vector<const osg::StateSet*> path;
            if (drawable.getStateSet() != nullptr)
                path.push_back(drawable.getStateSet());
            path.push_back(pass);
            const Surface surface = getSurface(path, static_cast<int>(passIndex) + 1);
            if (surface.mBucket == sInvalid)
                return nullptr;
            Instance instance;
            instance.mWorld = osg::Matrixf::translate(worldOffset);
            const osg::BoundingBox& box = drawable.getBoundingBox();
            instance.mBounds = osg::BoundingSphere(box.center() + worldOffset, box.radius());
            instance.mBox = box;
            instance.mSmallFeatureCulling = true;
            instance.mTerrainExtra = passIndex != 0;
            instance.mOpaqueShadow = mTerrainShadowOnce && passIndex == 0;
            instance.mGeometry = &drawable;
            instance.mBucket = surface.mBucket;
            instance.mMaterial = surface.mMaterial;
            instances.push_back(instance);
        }
        lit = lit || (mLightManager != nullptr && mLightManager->getDistantLightsEnabled());
        osg::ref_ptr<osg::Node> chunk = createChunk(std::move(instances), drawable.getBound(), lit, ChunkKind::Terrain);
        if (auto* gpuChunk = dynamic_cast<GpuChunkNode*>(chunk.get()))
            gpuChunk->setTerrainDrawable(&drawable);
        if (chunk && lit)
            chunk->addCullCallback(new SceneUtil::LightListCallback);
        return chunk;
    }

    void GpuScene::setCastingPrograms(const std::vector<osg::ref_ptr<osg::Program>>& programs)
    {
        mCastingProgramList = programs;
        for (const osg::ref_ptr<osg::Program>& program : programs)
        {
            if (osg::ref_ptr<osg::Program> instanced = mShaderManager.getInstancedProgram(*program, mMaterialMode))
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (std::find(mProgramsToLink.begin(), mProgramsToLink.end(), instanced) == mProgramsToLink.end())
                    mProgramsToLink.push_back(std::move(instanced));
            }
        }
    }

    unsigned int GpuScene::getPendingWork() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return static_cast<unsigned int>(mPendingMeshes.size() + mPendingInstances.size() + mPendingSkins.size()
            + mPendingMaterials.size() + mPendingRetired.size() + mProgramsToLink.size());
    }

    void GpuScene::reportStats(unsigned int frame, osg::Stats& stats) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::array<std::size_t, 4> chunkCounts{};
        std::array<std::size_t, 4> instanceCounts{};
        std::uint64_t groundcoverDetailedInstances = 0;
        std::uint64_t groundcoverReducedInstances = 0;
        std::uint64_t groundcoverDetailedTriangles = 0;
        std::uint64_t groundcoverReducedTriangles = 0;
        for (const Chunk& chunk : mChunks)
        {
            if (chunk.mGeneration == 0)
                continue;
            const std::size_t kind = static_cast<std::size_t>(chunk.mKind);
            ++chunkCounts[kind];
            instanceCounts[kind] += chunk.mInstanceCount;
            groundcoverDetailedInstances += chunk.mGroundcoverDetailedInstances;
            groundcoverReducedInstances += chunk.mGroundcoverReducedInstances;
            groundcoverDetailedTriangles += chunk.mGroundcoverDetailedTriangles;
            groundcoverReducedTriangles += chunk.mGroundcoverReducedTriangles;
        }
        constexpr std::array<std::string_view, 4> kindNames{ "object", "terrain", "groundcover", "actor" };
        for (std::size_t kind = 0; kind < kindNames.size(); ++kind)
        {
            const std::string prefix = "GPU " + std::string(kindNames[kind]);
            stats.setAttribute(frame, prefix + " chunks", static_cast<double>(chunkCounts[kind]));
            stats.setAttribute(frame, prefix + " instances", static_cast<double>(instanceCounts[kind]));
        }
        stats.setAttribute(
            frame, "GPU groundcover detailed instances", static_cast<double>(groundcoverDetailedInstances));
        stats.setAttribute(
            frame, "GPU groundcover reduced instances", static_cast<double>(groundcoverReducedInstances));
        stats.setAttribute(
            frame, "GPU groundcover detailed triangles", static_cast<double>(groundcoverDetailedTriangles));
        stats.setAttribute(
            frame, "GPU groundcover reduced triangles", static_cast<double>(groundcoverReducedTriangles));
        std::size_t gpuDeformationRigs = 0;
        std::size_t gpuDeformationFallbacks = 0;
        if (isEnabled() && mGpuSkinningEnabled)
        {
            const bool skinSupported = mSkinSupported.load(std::memory_order_relaxed);
            for (const auto& [actor, population] : mActorRigPopulations)
            {
                gpuDeformationRigs += skinSupported ? population.mGpu : 0;
                gpuDeformationFallbacks += skinSupported ? population.mTotal - population.mGpu : population.mTotal;
            }
        }
        stats.setAttribute(
            frame, std::string(SceneUtil::gpuDeformationRigCounter), static_cast<double>(gpuDeformationRigs));
        stats.setAttribute(
            frame, std::string(SceneUtil::gpuDeformationFallbackCounter), static_cast<double>(gpuDeformationFallbacks));
        const std::size_t bytes = mTextures->mArrayBytes + mTextures->mProxyBytes + mTextures->pendingBytes();
        const std::size_t budget = mTextures->budget();
        const std::size_t overage = budget != 0 && bytes > budget ? bytes - budget : 0;
        stats.setAttribute(frame, "GPU texture bytes", static_cast<double>(bytes));
        stats.setAttribute(frame, "GPU texture budget", static_cast<double>(mTextures->mBudgetBytes));
        stats.setAttribute(frame, "GPU texture available", static_cast<double>(budget));
        stats.setAttribute(frame, "GPU mesh bytes", static_cast<double>(mVertices.mUsed * sizeof(GpuVertex)));
        stats.setAttribute(frame, "GPU mesh extent", static_cast<double>(mVertices.mExtent * sizeof(GpuVertex)));
        stats.setAttribute(frame, "GPU mesh budget", static_cast<double>(mMeshBudgetBytes));
        stats.setAttribute(frame, "GPU mesh evictions", static_cast<double>(mMeshEvictions));
        stats.setAttribute(frame, "GPU receiver slots", static_cast<double>(mReceiverSlots.mExtent));
        stats.setAttribute(frame, "GPU array bytes", static_cast<double>(mTextures->mArrayBytes));
        const std::size_t pools = static_cast<std::size_t>(std::count_if(mTextures->mPools.begin(),
            mTextures->mPools.end(), [](const auto& pool) { return pool != nullptr && pool->mCapacity != 0; }));
        stats.setAttribute(frame, "GPU array pools", static_cast<double>(pools));
        stats.setAttribute(frame, "GPU material buckets", static_cast<double>(mBuckets.size()));
        const std::size_t legacy = static_cast<std::size_t>(
            std::count_if(mActiveMaterials.begin(), mActiveMaterials.end(), [&](Id material) {
                return material < mMaterials.size() && mMaterials[material].mMode == Shader::MaterialMode::Legacy;
            }));
        stats.setAttribute(frame, "GPU legacy materials", static_cast<double>(legacy));
        std::size_t commands = 0;
        for (const Bucket& bucket : mBuckets)
            commands += bucket.mCommandCount;
        stats.setAttribute(frame, "GPU draw commands", static_cast<double>(commands));
        stats.setAttribute(frame, "GPU texture handles", static_cast<double>(mTextureHandles.load()));
        stats.setAttribute(frame, "GPU mip changes", static_cast<double>(mTextures->mMipChanges));
        stats.setAttribute(frame, "GPU bucket edits", static_cast<double>(mAppliedEdits));
        stats.setAttribute(frame, "GPU texture overage", static_cast<double>(overage));
    }

    unsigned int GpuScene::switchModes(unsigned int childCount, unsigned int child)
    {
        // Missing modes use child zero.
        unsigned int modes = 0;
        for (unsigned int mode = 0; mode < sModeCount; ++mode)
            if ((childCount > mode ? mode : 0) == child)
                modes |= 1u << mode;
        return modes;
    }

    std::unique_ptr<const GpuTemplate> GpuScene::extract(const osg::Node& node, unsigned int mask, bool bakeBillboards)
    {
        auto result = std::make_unique<GpuTemplate>();
        // Bindless instances have spare flag bits.
        const bool nightDay = mDistantPagingEnabled && mMaterialMode == Shader::MaterialMode::Bindless
            && Settings::game().mDayNightSwitches;
        GpuExtractor extractor(*this, mask, bakeBillboards, nightDay, *result);
        extractor.visit(&node, osg::Matrixf::identity(), nullptr, 0, osg::Vec3f(), 1.f, true);
        if (result->mGeometries.empty())
            return nullptr;
        return result;
    }

    namespace
    {
        /// Accepts chains of UV controllers only.
        bool collectLiveUpdaters(osg::Callback& callback, std::vector<std::uintptr_t>& key,
            std::vector<osg::ref_ptr<osg::Callback>>& updaters)
        {
            const auto accept = [&](const SceneUtil::StateSetUpdater* controller) {
                const auto* uv = dynamic_cast<const NifOsg::UVController*>(controller);
                if (uv == nullptr)
                    return false;
                for (const void* part : uv->getAnimationKey())
                    key.push_back(reinterpret_cast<std::uintptr_t>(part));
                for (const unsigned int unit : uv->getTextureUnits())
                    key.push_back(unit);
                key.push_back(0);
                return true;
            };
            for (osg::Callback* cb = &callback; cb != nullptr; cb = cb->getNestedCallback())
            {
                if (auto* composite = dynamic_cast<SceneUtil::CompositeStateSetUpdater*>(cb))
                {
                    for (std::size_t i = 0; i < composite->getNumControllers(); ++i)
                        if (!accept(composite->getController(i)))
                            return false;
                }
                else if (!accept(dynamic_cast<const SceneUtil::StateSetUpdater*>(cb)))
                    return false;
                updaters.emplace_back(cb);
            }
            return !updaters.empty();
        }
    }

    osg::ref_ptr<const osg::StateSet> GpuScene::liveStateSet(const osg::Callback& callback)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<std::uintptr_t> key;
        std::vector<osg::ref_ptr<osg::Callback>> updaters;
        // Updaters mutate their interpolator caches.
        if (!collectLiveUpdaters(const_cast<osg::Callback&>(callback), key, updaters))
            return nullptr;
        LiveMaterials& entry = mLiveMaterials[key];
        if (entry.mStateSet == nullptr)
        {
            entry.mStateSet = new osg::StateSet;
            entry.mUpdaters = std::move(updaters);
            for (const osg::ref_ptr<osg::Callback>& updater : entry.mUpdaters)
                dynamic_cast<SceneUtil::StateSetUpdater&>(*updater).setDefaults(entry.mStateSet);
        }
        return entry.mStateSet;
    }

    void GpuScene::bindLiveMaterial(const osg::StateSet& stateSet, Id material)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& [key, entry] : mLiveMaterials)
        {
            if (entry.mStateSet != &stateSet)
                continue;
            if (std::find(entry.mMaterials.begin(), entry.mMaterials.end(), material) == entry.mMaterials.end())
                entry.mMaterials.push_back(material);
            return;
        }
    }

    void GpuScene::refreshLiveMaterials(osg::NodeVisitor& nv)
    {
        const unsigned int frame = nv.getFrameStamp()->getFrameNumber();
        std::lock_guard<std::mutex> lock(mMutex);
        if (mLiveFrame == frame)
            return;
        mLiveFrame = frame;
        for (auto it = mLiveMaterials.begin(); it != mLiveMaterials.end();)
        {
            LiveMaterials& entry = it->second;
            std::erase_if(entry.mMaterials, [&](Id material) {
                return material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published;
            });
            if (entry.mMaterials.empty())
            {
                if (entry.mStateSet->referenceCount() == 1)
                    it = mLiveMaterials.erase(it);
                else
                    ++it;
                continue;
            }
            const bool visible = std::any_of(entry.mMaterials.begin(), entry.mMaterials.end(),
                [&](Id material) { return mMaterialUsers[material] != 0 || frame <= mMaterialLast[material] + 3; });
            if (visible)
            {
                for (const osg::ref_ptr<osg::Callback>& updater : entry.mUpdaters)
                    dynamic_cast<SceneUtil::StateSetUpdater&>(*updater).apply(entry.mStateSet, &nv);
                for (const Id material : entry.mMaterials)
                    refreshMaterialLocked(material, frame);
            }
            ++it;
        }
    }

    bool GpuScene::isTemplateLive(const GpuTemplate& tmpl) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const GpuGeometry& geometry : tmpl.mGeometries)
        {
            const Id material = geometry.mSurface.mMaterial;
            if (material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published)
                return false;
        }
        return true;
    }

    void GpuScene::appendInstances(const GpuTemplate& tmpl, const osg::Matrixf& refMatrix, float refScale,
        const osg::LOD::MinMaxPair& refDistances, bool smallFeatureCulling, std::vector<Instance>& out) const
    {
        for (const GpuGeometry& geometry : tmpl.mGeometries)
        {
            Instance instance;
            if (geometry.mLod != nullptr)
            {
                const osg::LOD::RangeList& ranges = geometry.mLod->getRangeList();
                if (empty(intersection(ranges[geometry.mLodChild], refDistances)))
                    continue;
                unsigned int survivors = 0;
                for (const LODRange& range : ranges)
                    if (!empty(intersection(range, refDistances)))
                        ++survivors;
                if (survivors > 1)
                {
                    const float lodScale = geometry.mLodScale * refScale;
                    instance.mLod = true;
                    instance.mLodCenter = geometry.mLodCenter * refMatrix;
                    instance.mLodMin = ranges[geometry.mLodChild].first * lodScale;
                    instance.mLodMax = ranges[geometry.mLodChild].second * lodScale;
                }
            }
            instance.mWorld = geometry.mMatrix * refMatrix;
            const float scale = uniformScale(geometry.mMatrix) * refScale;
            instance.mBounds
                = osg::BoundingSphere(geometry.mBounds.center() * instance.mWorld, geometry.mBounds.radius() * scale);
            instance.mBox = geometry.mBounds;
            instance.mSmallFeatureCulling = smallFeatureCulling;
            instance.mModes = geometry.mModes;
            instance.mGeometry = geometry.mGeometry;
            instance.mBucket = geometry.mSurface.mBucket;
            instance.mMaterial = geometry.mSurface.mMaterial;
            out.push_back(std::move(instance));
        }
    }

    void GpuScene::appendGroundcoverInstances(
        const GpuTemplate& tmpl, const osg::Matrixf& refMatrix, float refScale, std::vector<Instance>& out) const
    {
        const std::size_t first = out.size();
        appendInstances(tmpl, refMatrix, refScale, { 0.f, std::numeric_limits<float>::max() }, false, out);
        for (std::size_t index = first; index < out.size(); ++index)
            out[index].mGroundcoverLod = out[index].mLod;
    }

    void GpuScene::setGroundcoverDistances(float detailedDistance, float viewDistance)
    {
        if (!std::isfinite(detailedDistance) || !std::isfinite(viewDistance) || detailedDistance < 0.f
            || detailedDistance > viewDistance)
        {
            throw std::invalid_argument("GPU groundcover distances must be finite, non-negative, and ordered");
        }
        mGroundcoverDetailedDistance = detailedDistance;
        mGroundcoverViewDistance = viewDistance;
    }
}
