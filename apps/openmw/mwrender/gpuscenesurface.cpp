#include "gpusceneinternal.hpp"

namespace MWRender
{
    std::size_t GpuScene::PathHash::operator()(const SurfacePath& path) const
    {
        std::size_t value = path.size();
        for (const osg::ref_ptr<const osg::StateSet>& stateSet : path)
            value ^= std::hash<const osg::StateSet*>{}(stateSet.get()) + 0x9e3779b9 + (value << 6) + (value >> 2);
        return value;
    }

    std::size_t GpuScene::PathHash::operator()(const std::vector<const osg::StateSet*>& path) const
    {
        std::size_t value = path.size();
        for (const osg::StateSet* stateSet : path)
            value ^= std::hash<const osg::StateSet*>{}(stateSet) + 0x9e3779b9 + (value << 6) + (value >> 2);
        return value;
    }

    bool GpuScene::PathEqual::operator()(const SurfacePath& left, const SurfacePath& right) const
    {
        return left == right;
    }

    bool GpuScene::PathEqual::operator()(const SurfacePath& left, const std::vector<const osg::StateSet*>& right) const
    {
        return left.size() == right.size()
            && std::equal(left.begin(), left.end(), right.begin(),
                [](const osg::ref_ptr<const osg::StateSet>& l, const osg::StateSet* r) { return l.get() == r; });
    }

    bool GpuScene::PathEqual::operator()(const std::vector<const osg::StateSet*>& left, const SurfacePath& right) const
    {
        return operator()(right, left);
    }

    bool GpuScene::bucketEdited(Id bucket) const
    {
        return std::any_of(
            mBucketEdits.begin(), mBucketEdits.end(), [&](const BucketEdit& edit) { return edit.mBucket == bucket; });
    }

    /// Runs on the graphics thread.
    void GpuScene::applyBucketEdits()
    {
        for (const BucketEdit& edit : mBucketEdits)
        {
            if (edit.mBucket >= mBuckets.size())
                continue;
            osg::StateSet& bucket = *mBuckets[edit.mBucket].mStateSet;
            if (edit.mTexture != nullptr)
                bucket.setTextureAttribute(edit.mUnit, edit.mTexture);
            if (edit.mSampler != nullptr)
            {
                bucket.setTextureAttribute(edit.mUnit, edit.mSampler);
                for (bool full : { false, true })
                {
                    const auto actor = mActorBuckets.find({ edit.mBucket, full });
                    if (actor != mActorBuckets.end() && actor->second != edit.mBucket)
                    {
                        osg::StateSet& actorState = *mBuckets[actor->second].mStateSet;
                        actorState.setTextureAttribute(edit.mUnit, edit.mSampler);
                        mGl->mPrepassStates.erase(&actorState);
                    }
                }
            }
            if (edit.mTexture == nullptr && edit.mSampler == nullptr)
            {
                for (unsigned int unit = 0; unit < bucket.getNumTextureAttributeLists(); ++unit)
                {
                    bucket.removeTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                    bucket.removeTextureAttribute(unit, osg::StateAttribute::SAMPLER);
                }
                bucket.removeAttribute(osg::StateAttribute::MATERIAL);
            }
            // The next prepass rebuilds the clone.
            mGl->mPrepassStates.erase(&bucket);
            ++mAppliedEdits;
        }
        mBucketEdits.clear();
    }

    GpuScene::Surface GpuScene::getSurface(
        const std::vector<const osg::StateSet*>& statePath, int order, MaterialUpdate updates)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return getSurfaceLocked(statePath, order, updates);
    }

    GpuScene::Surface GpuScene::getSurfaceLocked(
        const std::vector<const osg::StateSet*>& statePath, int order, MaterialUpdate updates)
    {
        if (!mSupported)
            return {};
        const auto found = mSurfaces.find(statePath);
        if (found != mSurfaces.end())
        {
            if (order <= 1)
                mBuckets[found->second.mBucket].mTerrainExtraOnly = false;
            return found->second;
        }
        Surface surface;
        const auto failed = mFailedSurfaces.find(statePath);
        if (failed != mFailedSurfaces.end())
        {
            failed->second = mLifetimeFrame;
            return surface;
        }
        SurfacePath key(statePath.begin(), statePath.end());

        osg::ref_ptr<osg::StateSet> merged = new osg::StateSet;
        for (const osg::StateSet* stateSet : statePath)
            merged->merge(*stateSet);
        const osg::Program* program
            = dynamic_cast<const osg::Program*>(merged->getAttribute(osg::StateAttribute::PROGRAM));
        if (program == nullptr)
        {
            mFailedSurfaces.emplace(std::move(key), mLifetimeFrame);
            return surface;
        }

        const Id materialId = static_cast<Id>(mMaterials.size());
        std::unique_ptr<MaterialSource> source = makeSource(statePath, *program);
        PendingMaterial material = makeMaterial(*source, materialId);
        Shader::MaterialMode materialMode = mMaterialMode;
        // Controller-driven state retains its source bindings.
        if (updates == MaterialUpdate::State && materialMode == Shader::MaterialMode::Array)
            materialMode = Shader::MaterialMode::Legacy;
        osg::ref_ptr<osg::StateSet> pipeline = new osg::StateSet(*merged, osg::CopyOp::SHALLOW_COPY);
        if (materialMode == Shader::MaterialMode::Bindless)
            mTextures->makeBindless(material);
        else if (materialMode == Shader::MaterialMode::Array && !mTextures->makeArray(material, *source, *pipeline))
        {
            materialMode = Shader::MaterialMode::Legacy;
            pipeline = merged;
            material = makeMaterial(*source, materialId);
        }
        if (materialMode == Shader::MaterialMode::Legacy)
            mTextures->makeLegacy(material, *source, *pipeline);
        material.mMode = materialMode;
        osg::ref_ptr<osg::Program> instanced = mShaderManager.getInstancedProgram(*program, materialMode);
        if (instanced == nullptr && materialMode == Shader::MaterialMode::Array)
        {
            mTextures->release(material);
            materialMode = Shader::MaterialMode::Legacy;
            pipeline = merged;
            material = makeMaterial(*source, materialId);
            mTextures->makeLegacy(material, *source, *pipeline);
            material.mMode = materialMode;
            instanced = mShaderManager.getInstancedProgram(*program, materialMode);
        }
        if (instanced == nullptr || (updates == MaterialUpdate::Record && materialMode == Shader::MaterialMode::Legacy))
        {
            mTextures->release(material);
            mFailedSurfaces.emplace(std::move(key), mLifetimeFrame);
            return surface;
        }

        if (materialMode != Shader::MaterialMode::Legacy)
        {
            std::vector<unsigned int> materialUnits;
            for (unsigned int i = 0; i < sMapCount; ++i)
            {
                const int unit = source->mUnits[i];
                if (unit < 0)
                    continue;
                materialUnits.push_back(static_cast<unsigned int>(unit));
                if (materialMode == Shader::MaterialMode::Bindless)
                    pipeline->removeUniform(Shader::sMaterialMapNames[i]);
            }
            if (materialMode == Shader::MaterialMode::Bindless)
            {
                std::sort(materialUnits.begin(), materialUnits.end());
                materialUnits.erase(std::unique(materialUnits.begin(), materialUnits.end()), materialUnits.end());
                for (const unsigned int unit : materialUnits)
                {
                    pipeline->removeTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                    pipeline->removeTextureMode(unit, GL_TEXTURE_2D);
                }
            }
            pipeline->removeAttribute(osg::StateAttribute::MATERIAL);
            for (const char* name : sMaterialUniforms)
                pipeline->removeUniform(name);
        }
        surface.mMaterial = material.mIndex;
        mMaterials.push_back(material);
        mMaterialSources.push_back(std::move(source));
        mMaterialFrames.push_back(std::numeric_limits<unsigned int>::max());
        mMaterialUsers.push_back(0);
        mMaterialLast.push_back(mLifetimeFrame);
        mMaterialStates.push_back(MaterialState::Active);
        mMaterialPaths.push_back(key);
        mActiveMaterials.push_back(materialId);
        if (materialMode != Shader::MaterialMode::Legacy)
            mPendingMaterials.push_back(std::move(material));
        pipeline->setAttributeAndModes(instanced, osg::StateAttribute::ON);
        if (order != 0)
            pipeline->setRenderBinDetails(order - 1, "RenderBin");

        // Drop empty lists so states compare equal.
        osg::ref_ptr<osg::StateSet> canonical = new osg::StateSet;
        for (const auto& [mode, value] : pipeline->getModeList())
            canonical->setMode(mode, value);
        for (const auto& [typeMember, attrPair] : pipeline->getAttributeList())
            canonical->setAttribute(const_cast<osg::StateAttribute*>(attrPair.first.get()), attrPair.second);
        for (const auto& [name, uniformPair] : pipeline->getUniformList())
            canonical->addUniform(const_cast<osg::Uniform*>(uniformPair.first->asUniform()), uniformPair.second);
        for (unsigned int unit = 0; unit < pipeline->getTextureModeList().size(); ++unit)
            for (const auto& [mode, value] : pipeline->getTextureModeList()[unit])
                canonical->setTextureMode(unit, mode, value);
        for (unsigned int unit = 0; unit < pipeline->getNumTextureAttributeLists(); ++unit)
            for (const auto& [typeMember, attrPair] : pipeline->getTextureAttributeList()[unit])
                canonical->setTextureAttribute(
                    unit, const_cast<osg::StateAttribute*>(attrPair.first.get()), attrPair.second);
        if (pipeline->useRenderBinDetails())
            canonical->setRenderBinDetails(
                pipeline->getBinNumber(), pipeline->getBinName(), pipeline->getRenderBinMode());
        else
            canonical->setRenderingHint(pipeline->getRenderingHint());
        canonical->setNestRenderBins(pipeline->getNestRenderBins());
        pipeline = canonical;

        const bool ownBucket = updates != MaterialUpdate::None && materialMode == Shader::MaterialMode::Legacy;
        surface.mBucket = getBucketLocked(*pipeline, *instanced, order, materialMode, ownBucket);
        ++mBuckets[surface.mBucket].mMaterials;
        mSurfaces.emplace(std::move(key), surface);
        return surface;
    }

    GpuScene::Id GpuScene::getBucketLocked(
        osg::StateSet& pipeline, const osg::Program& program, int order, Shader::MaterialMode mode, bool live)
    {
        Id bucketId = sInvalid;
        for (Id id = 0; !live && id < mBuckets.size(); ++id)
        {
            // A queued edit still changes this state.
            if (bucketEdited(id) || mBuckets[id].mLive)
                continue;
            if (mBuckets[id].mStateSet->compare(pipeline, true) == 0)
            {
                bucketId = id;
                mBuckets[id].mTerrainExtraOnly = mBuckets[id].mTerrainExtraOnly && order > 1;
                break;
            }
        }
        if (bucketId == sInvalid)
        {
            Bucket bucket;
            bucket.mStateSet = &pipeline;
            bucket.mProgram = &program;
            bucket.mMode = mode;
            bucket.mLive = live;
            bucket.mTerrainExtraOnly = order > 1;
            bucket.mDelta = pipeline.getUniformList().empty()
                && std::all_of(pipeline.getTextureModeList().begin(), pipeline.getTextureModeList().end(),
                    [](const osg::StateSet::ModeList& modes) { return modes.empty(); })
                && std::all_of(pipeline.getTextureAttributeList().begin(), pipeline.getTextureAttributeList().end(),
                    [](const osg::StateSet::AttributeList& attributes) { return attributes.empty(); });
            // Every bucket state feeds the reset lists.
            for (const auto& [glMode, value] : pipeline.getModeList())
                if (std::find(mDeltaModes.begin(), mDeltaModes.end(), glMode) == mDeltaModes.end())
                    mDeltaModes.push_back(glMode);
            for (const auto& [typeMember, attribute] : pipeline.getAttributeList())
                if (typeMember.first != osg::StateAttribute::PROGRAM
                    && std::find(mDeltaAttributes.begin(), mDeltaAttributes.end(), typeMember)
                        == mDeltaAttributes.end())
                    mDeltaAttributes.push_back(typeMember);
            if (std::find(mProgramsToLink.begin(), mProgramsToLink.end(), &program) == mProgramsToLink.end())
                mProgramsToLink.push_back(const_cast<osg::Program*>(&program));
            bucketId = static_cast<Id>(mBuckets.size());
            mBuckets.push_back(std::move(bucket));

            BinKey binKey;
            binKey.mHint = pipeline.getRenderingHint();
            binKey.mNumber = pipeline.getBinNumber();
            binKey.mMode = pipeline.getRenderBinMode();
            binKey.mName = pipeline.getBinName();
            binKey.mNested = pipeline.getNestRenderBins();
            auto dispatcher = std::find_if(mDispatchers.begin(), mDispatchers.end(),
                [&](const Dispatcher& candidate) { return candidate.mKey == binKey; });
            if (dispatcher == mDispatchers.end())
            {
                const Id id = static_cast<Id>(mDispatchers.size());
                Dispatcher created;
                created.mKey = binKey;
                created.mStateSet = new osg::StateSet;
                if (pipeline.useRenderBinDetails())
                    created.mStateSet->setRenderBinDetails(
                        pipeline.getBinNumber(), pipeline.getBinName(), pipeline.getRenderBinMode());
                else
                    created.mStateSet->setRenderingHint(pipeline.getRenderingHint());
                created.mStateSet->setNestRenderBins(pipeline.getNestRenderBins());
                created.mDrawable = new DispatchDrawable(*this, id);
                created.mDrawable->setStateSet(created.mStateSet);
                created.mOrdered = order != 0;
                mPendingDrawables.emplace_back(created.mDrawable, created.mOrdered);
                mDispatchers.push_back(std::move(created));
                dispatcher = std::prev(mDispatchers.end());
            }
            dispatcher->mBuckets.push_back(bucketId);
        }
        return bucketId;
    }

    GpuScene::Id GpuScene::getActorBucket(Id bucket, bool full)
    {
        const auto found = mActorBuckets.find({ bucket, full });
        if (found != mActorBuckets.end())
            return found->second;
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet(*mBuckets[bucket].mStateSet, osg::CopyOp::SHALLOW_COPY);
        state->removeAttribute(osg::StateAttribute::FRONTFACE);
        for (unsigned int unit = 0; unit < state->getNumTextureAttributeLists(); ++unit)
            state->removeTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
        auto& attributes = state->getTextureAttributeList();
        while (!attributes.empty() && attributes.back().empty())
            attributes.pop_back();
        osg::ref_ptr<const osg::Program> program = mBuckets[bucket].mProgram;
        if (full)
        {
            program = mShaderManager.getInstancedProgram(*program, mMaterialMode, true);
            if (program == nullptr)
                return sInvalid;
            state->setAttributeAndModes(const_cast<osg::Program*>(program.get()), osg::StateAttribute::ON);
        }
        const Id result = getBucketLocked(*state, *program, 0, mBuckets[bucket].mMode, false);
        mActorBuckets.emplace(std::make_pair(bucket, full), result);
        return result;
    }

    /// Transfers the cache slot's mesh reference.
    void GpuCaptureCallback::adoptMeshLocked(Cache& cache, GpuScene::Id mesh) const
    {
        GpuScene& scene = *mScene.load(std::memory_order_relaxed);
        if (cache.mMesh == mesh)
            return;
        if (cache.mMesh != GpuScene::sInvalid)
            scene.releaseMeshLocked(cache.mMesh);
        if (mesh != GpuScene::sInvalid)
            ++scene.mMeshes[mesh].mUsers;
        cache.mMesh = mesh;
    }

    std::pair<GpuScene::Id, GpuScene::Surface> GpuCaptureCallback::resolveLocked(osg::Geometry& geometry,
        const std::vector<const osg::StateSet*>& path, unsigned int frame, bool liveMaterial) const
    {
        GpuScene& scene = *mScene.load(std::memory_order_relaxed);
        GpuScene::Id mesh = GpuScene::sInvalid;
        Cache* match = nullptr;
        for (Cache& cache : mCache)
        {
            if (cache.mGeometry != &geometry)
                continue;
            if (cache.mMesh < scene.mMeshes.size() && scene.mMeshes[cache.mMesh].mGeometry.get() == &geometry)
                mesh = cache.mMesh;
            if (cache.mPath.size() == path.size()
                && std::equal(cache.mPath.begin(), cache.mPath.end(), path.begin(),
                    [](const osg::ref_ptr<const osg::StateSet>& left, const osg::StateSet* right) {
                        return left.get() == right;
                    }))
                match = &cache;
        }
        if (mesh == GpuScene::sInvalid)
            mesh = scene.getMeshLocked(geometry);
        else
        {
            // Keep the eviction grace window fresh.
            scene.mMeshes[mesh].mLast = scene.mLifetimeFrame;
        }
        if (match != nullptr
            && (match->mSurface.mMaterial >= scene.mMaterialStates.size()
                || scene.mMaterialStates[match->mSurface.mMaterial] == GpuScene::MaterialState::Published))
            match = nullptr;
        if (match != nullptr)
        {
            adoptMeshLocked(*match, mesh);
            if (liveMaterial)
                scene.refreshMaterialLocked(match->mSurface.mMaterial, frame);
            return { mesh, match->mSurface };
        }

        const GpuScene::Surface surface = scene.getSurfaceLocked(
            path, 0, liveMaterial ? GpuScene::MaterialUpdate::State : GpuScene::MaterialUpdate::None);
        if (liveMaterial)
            scene.refreshMaterialLocked(surface.mMaterial, frame);
        Cache& cache = mCache[mNextCache++ % mCache.size()];
        adoptMeshLocked(cache, mesh);
        cache.mGeometry = &geometry;
        cache.mPath.assign(path.begin(), path.end());
        cache.mSurface = surface;
        return { mesh, surface };
    }

    bool GpuCaptureCallback::hasLiveMaterial(const osgUtil::CullVisitor& cv) const
    {
        GpuScene& scene = *mScene.load(std::memory_order_relaxed);
        int live = mLiveMaterial.load(std::memory_order_relaxed);
        if (live >= 0)
            return live != 0;
        live = 0;
        const osg::NodePath& path = cv.getNodePath();
        for (auto it = path.rbegin(); it != path.rend(); ++it)
        {
            const osg::Node* node = *it;
            // Scene root updaters only set uniforms.
            if (node == scene.mLightManager)
                break;
            if (hasUpdater(node->getUpdateCallback()) || hasUpdater(node->getCullCallback()))
            {
                live = 1;
                break;
            }
        }
        mLiveMaterial.store(live, std::memory_order_relaxed);
        return live != 0;
    }
}
