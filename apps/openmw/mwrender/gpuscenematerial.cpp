#include "gpusceneinternal.hpp"

#include <SDL_opengl_glext.h>

namespace MWRender
{
    GpuScene::Id GpuScene::Textures::track(osg::Texture& texture, osg::ref_ptr<osg::Texture>& out)
    {
        out = &texture;
        auto* source = dynamic_cast<osg::Texture2D*>(&texture);
        StorageKey key;
        // Textures without storage stay outside the budget.
        if (source == nullptr || !storage(*source, key))
            return sInvalid;
        const auto entryKey = std::make_pair(source->getImage(), key);
        const auto found = mProxyIds.find(entryKey);
        if (found != mProxyIds.end())
        {
            const Proxy& entry = mProxies[found->second];
            // Only a demoted entry replaces the source.
            if (entry.mCurrent != 0)
                out = entry.mTexture;
            return found->second;
        }
        Proxy proxy;
        proxy.mImage = source->getImage();
        proxy.mSource = source;
        proxy.mKey = key;
        proxy.mBytes = key.mBytes;
        while (proxy.mFloor + 1 < key.mLevels
            && std::max(key.mWidth >> (proxy.mFloor + 1), key.mHeight >> (proxy.mFloor + 1)) >= 64)
            ++proxy.mFloor;
        Id id;
        if (!mFreeProxies.empty())
        {
            id = mFreeProxies.back();
            mFreeProxies.pop_back();
            mProxies[id] = std::move(proxy);
        }
        else
        {
            id = static_cast<Id>(mProxies.size());
            mProxies.push_back(std::move(proxy));
        }
        mProxyIds.emplace(entryKey, id);
        mProxyBytes += key.mBytes;
        return id;
    }

    void GpuScene::Textures::makeBindless(PendingMaterial& material)
    {
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            material.mProxies[i] = sInvalid;
            material.mArrays[i] = sInvalid;
            if (material.mTextures[i] == nullptr)
                continue;
            material.mSamplers[i] = sampler(*material.mTextures[i]);
            osg::ref_ptr<osg::Texture> texture;
            material.mProxies[i] = track(*material.mTextures[i], texture);
            material.mTextures[i] = texture;
        }
        useProxies(material);
    }

    void GpuScene::Textures::useProxies(PendingMaterial& material)
    {
        for (const Id proxy : material.mProxies)
        {
            if (proxy == sInvalid)
                continue;
            Proxy& entry = mProxies[proxy];
            // Maps sharing an entry count one user.
            if (std::find(entry.mMaterials.begin(), entry.mMaterials.end(), material.mIndex) != entry.mMaterials.end())
                continue;
            ++entry.mUsers;
            entry.mMaterials.push_back(material.mIndex);
        }
    }

    void GpuScene::Textures::makeLegacy(
        PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline)
    {
        for (unsigned int map = 0; map < sMapCount; ++map)
        {
            material.mProxies[map] = sInvalid;
            material.mArrays[map] = sInvalid;
            if (material.mTextures[map] == nullptr)
                continue;
            material.mSamplers[map] = sampler(*material.mTextures[map]);
            osg::ref_ptr<osg::Texture> texture;
            material.mProxies[map] = track(*material.mTextures[map], texture);
            const int unit = source.mUnits[map];
            // The merged state already binds an unchanged source texture.
            if (texture != material.mTextures[map] && unit >= 0)
            {
                pipeline.setTextureAttribute(static_cast<unsigned int>(unit), texture);
                pipeline.setTextureAttribute(static_cast<unsigned int>(unit), material.mSamplers[map]);
            }
            material.mTextures[map] = texture;
        }
        useProxies(material);
    }

    bool GpuScene::Textures::makeArray(PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline)
    {
        std::vector<Reservation> reservations;
        std::map<int, std::tuple<Id, std::uint32_t, osg::Sampler*>> units;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            material.mProxies[i] = sInvalid;
            material.mArrays[i] = sInvalid;
            if (material.mTextures[i] == nullptr)
                continue;
            auto* texture = dynamic_cast<osg::Texture2D*>(material.mTextures[i].get());
            const char* cause = texture == nullptr ? "not a 2D texture" : "";
            const Reservation reservation = texture == nullptr ? Reservation() : reserve(*texture, &cause);
            if (reservation.mPool == sInvalid)
            {
                rollback(reservations);
                warnTexture(material.mTextures[i].get(), "array", cause);
                return false;
            }
            reservations.push_back(reservation);
            material.mArrays[i] = reservation.mPool;
            material.mLayers[i] = reservation.mLayer;
            material.mRecord.mMaps[i] = static_cast<std::uint64_t>(reservation.mLayer) + 1;
            material.mSamplers[i] = sampler(*texture);
            // Shared units resolve to one texture.
            units.emplace(
                source.mUnits[i], std::make_tuple(reservation.mPool, reservation.mLayer, material.mSamplers[i].get()));
        }
        for (const auto& [unit, binding] : units)
        {
            if (unit < 0)
                continue;
            const Id pool = std::get<0>(binding);
            pipeline.setTextureAttribute(static_cast<unsigned int>(unit), mPools[pool]->mTexture);
            pipeline.setTextureAttribute(static_cast<unsigned int>(unit), std::get<2>(binding));
            pipeline.removeTextureMode(static_cast<unsigned int>(unit), GL_TEXTURE_2D);
        }
        return true;
    }

    void GpuScene::Textures::release(const PendingMaterial& material, osg::State* state)
    {
        std::vector<Reservation> reservations;
        for (unsigned int i = 0; i < sMapCount; ++i)
            if (material.mArrays[i] != sInvalid)
                reservations.push_back({ material.mArrays[i], material.mLayers[i] });
        rollback(reservations);
        for (const Id proxy : material.mProxies)
        {
            if (proxy == sInvalid || proxy >= mProxies.size())
                continue;
            Proxy& entry = mProxies[proxy];
            const auto found = std::find(entry.mMaterials.begin(), entry.mMaterials.end(), material.mIndex);
            if (found == entry.mMaterials.end())
                continue;
            entry.mMaterials.erase(found);
            if (entry.mUsers == 0 || --entry.mUsers != 0)
                continue;
            mProxyIds.erase(std::make_pair(entry.mImage.get(), entry.mKey));
            mProxyBytes -= entry.mBytes;
            // Tier zero belongs to the scene.
            for (std::size_t tier = 1; tier < entry.mTiers.size(); ++tier)
                entry.mTiers[tier].mTexture->releaseGLObjects(state);
            entry.mTiers.clear();
            entry.mTexture = nullptr;
            entry.mTemplate = nullptr;
            entry.mSource = nullptr;
            entry.mImage = nullptr;
            // A retire ticket still names the slot.
            if (!entry.mRetiring)
                mFreeProxies.push_back(proxy);
        }
    }

    /// Collects finished tiers and starts one build.

    std::unique_ptr<GpuScene::MaterialSource> GpuScene::makeSource(
        const std::vector<const osg::StateSet*>& statePath, const osg::Program& program) const
    {
        auto source = std::make_unique<MaterialSource>();
        source->mPath = statePath;
        const auto uniform = [&](const char* name) {
            return resolvePair<osg::StateSet::RefUniformPair>(
                statePath, [&](const osg::StateSet& stateSet) { return stateSet.getUniformPair(name); });
        };
        source->mMaterial = resolvePair<osg::StateSet::RefAttributePair>(statePath,
            [](const osg::StateSet& stateSet) { return stateSet.getAttributePair(osg::StateAttribute::MATERIAL); });
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            source->mMaps[i] = uniform(Shader::sMaterialMapNames[i]);
            source->mUnits[i] = mShaderManager.getSamplerUnit(program, Shader::sMaterialMapNames[i]).value_or(-1);
        }
        source->mEnvMapColor = uniform("envMapColor");
        source->mAlphaRef = uniform("alphaRef");
        source->mLumaBias = uniform("envMapLumaBias");
        source->mBumpMatrix = uniform("bumpMapMatrix");
        for (unsigned int i = 0; i < source->mTexMats.size(); ++i)
            source->mTexMats[i] = uniform(sTexMatNames[i]);
        return source;
    }

    GpuScene::PendingMaterial GpuScene::makeMaterial(MaterialSource& source, Id index)
    {
        PendingMaterial material;
        material.mIndex = index;
        material.mProxies.fill(sInvalid);
        material.mArrays.fill(sInvalid);
        material.mLayers.fill(0);
        GpuMaterial& record = material.mRecord;
        std::memset(&record, 0, sizeof(record));
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            const int unit = uniformValue(source.mMaps[i], source.mUnits[i]);
            if (!source.mMapsInitialized || source.mUnits[i] != unit)
            {
                source.mUnits[i] = unit;
                source.mTextures[i] = unit < 0
                    ? nullptr
                    : resolvePair<osg::StateSet::RefAttributePair>(source.mPath, [&](const osg::StateSet& stateSet) {
                          return stateSet.getTextureAttributePair(unit, osg::StateAttribute::TEXTURE);
                      });
            }
            if (source.mTextures[i] != nullptr)
                material.mTextures[i] = dynamic_cast<osg::Texture*>(source.mTextures[i]->first.get());
            material.mSources[i] = material.mTextures[i].get();
        }
        source.mMapsInitialized = true;
        const SceneUtil::Material* sceneMaterial = source.mMaterial == nullptr
            ? nullptr
            : dynamic_cast<const SceneUtil::Material*>(source.mMaterial->first.get());
        const SceneUtil::Material& mat = sceneMaterial != nullptr ? *sceneMaterial : *mDefaultMaterial;
        put(record.mDiffuse, mat.getDiffuse());
        put(record.mAmbient, mat.getAmbient());
        put(record.mSpecular, mat.getSpecular());
        put(record.mEmission, mat.getEmission());
        put(record.mEnvMapColor, uniformValue(source.mEnvMapColor, osg::Vec4f(1, 1, 1, 1)));
        record.mParams[0] = mat.getShininess();
        record.mParams[1] = mat.getEmissiveMultiplier();
        record.mParams[2] = mat.getSpecularStrength();
        record.mParams[3] = uniformValue(source.mAlphaRef, 0.f);
        const osg::Vec2f lumaBias = uniformValue(source.mLumaBias, osg::Vec2f(0, 0));
        record.mLumaBias[0] = lumaBias.x();
        record.mLumaBias[1] = lumaBias.y();
        const osg::Matrix2 bumpMatrix = uniformValue(source.mBumpMatrix, osg::Matrix2(1, 0, 0, 1));
        for (int i = 0; i < 4; ++i)
            record.mBumpMatrix[i] = bumpMatrix[i];
        record.mModes[0] = static_cast<std::int32_t>(mat.getVertexColorMode());
        // Texture unit per map, four bits each.
        std::uint32_t unitsLow = 0;
        std::uint32_t unitsHigh = 0;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            const std::uint32_t unit = source.mUnits[i] > 0 ? static_cast<std::uint32_t>(source.mUnits[i]) & 15u : 0u;
            if (i < 8)
                unitsLow |= unit << (i * 4);
            else
                unitsHigh |= unit << ((i - 8) * 4);
        }
        record.mModes[1] = static_cast<std::int32_t>(unitsLow);
        record.mModes[2] = static_cast<std::int32_t>(unitsHigh);
        for (unsigned int unit = 0; unit < 8; ++unit)
        {
            const osg::Matrixf texMat = uniformValue(source.mTexMats[unit], osg::Matrixf::identity());
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 4; ++col)
                    record.mTexMatRows[unit * 2 + row][col] = texMat(col, row);
        }
        return material;
    }

    void GpuScene::refreshMaterialLocked(Id material, unsigned int frame, bool samplers)
    {
        if (material >= mMaterials.size() || mMaterialFrames[material] == frame)
            return;
        mMaterialFrames[material] = frame;
        const Shader::MaterialMode mode = mMaterials[material].mMode;
        if (mMaterialSources[material] == nullptr)
            return;
        if (mode == Shader::MaterialMode::Array)
        {
            // Array refresh is not transactional yet.
            if (samplers)
                rebindSamplersLocked(material);
            PendingMaterial pending = makeMaterial(*mMaterialSources[material], material);
            PendingMaterial& current = mMaterials[material];
            if (pending.mSources != current.mSources)
                return;
            std::copy(std::begin(current.mRecord.mMaps), std::end(current.mRecord.mMaps), pending.mRecord.mMaps);
            if (std::memcmp(&pending.mRecord, &current.mRecord, sizeof(GpuMaterial)) != 0)
            {
                current.mRecord = pending.mRecord;
                mPendingMaterials.push_back(current);
            }
            return;
        }
        const bool bindless = mode == Shader::MaterialMode::Bindless;
        if (samplers && !bindless)
            rebindSamplersLocked(material);
        const bool forced = samplers && bindless;
        PendingMaterial pending = makeMaterial(*mMaterialSources[material], material);
        pending.mMode = mode;
        const PendingMaterial& current = mMaterials[material];
        const auto changed = [&] {
            if (bindless && std::memcmp(&pending.mRecord, &current.mRecord, sizeof(GpuMaterial)) != 0)
                return true;
            for (unsigned int i = 0; i < sMapCount; ++i)
                if (pending.mTextures[i].get() != current.mTextures[i].get())
                    return true;
            return false;
        };
        if (!forced && !mTextures->demoted(current) && !changed())
            return;
        if (bindless)
            mTextures->makeBindless(pending);
        else
        {
            const osg::ref_ptr<osg::StateSet> scratch = new osg::StateSet;
            mTextures->makeLegacy(pending, *mMaterialSources[material], *scratch);
        }
        if (!forced && !changed())
            return;
        if (!bindless)
            rebindMapsLocked(material, pending);
        PendingMaterial retired = current;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            if (retired.mProxies[i] == pending.mProxies[i])
                retired.mProxies[i] = sInvalid;
            if (retired.mArrays[i] == pending.mArrays[i] && retired.mLayers[i] == pending.mLayers[i])
                retired.mArrays[i] = sInvalid;
        }
        mPendingRetired.push_back(PendingRetire{ std::move(retired), false });
        mMaterials[material] = pending;
        if (bindless)
            mPendingMaterials.push_back(std::move(pending));
    }

    void GpuScene::rebindMapsLocked(Id material, const PendingMaterial& pending)
    {
        const auto surface = mSurfaces.find(mMaterialPaths[material]);
        if (surface == mSurfaces.end() || surface->second.mBucket >= mBuckets.size()
            || !mBuckets[surface->second.mBucket].mLive)
            return;
        const MaterialSource& source = *mMaterialSources[material];
        const PendingMaterial& current = mMaterials[material];
        for (unsigned int map = 0; map < sMapCount; ++map)
        {
            const int unit = source.mUnits[map];
            if (unit < 0 || pending.mTextures[map] == nullptr || pending.mTextures[map] == current.mTextures[map])
                continue;
            const osg::ref_ptr<osg::Sampler> replacement
                = mTextures->demoted(pending, map) ? pending.mSamplers[map] : nullptr;
            mBucketEdits.push_back(BucketEdit{
                surface->second.mBucket, static_cast<unsigned int>(unit), pending.mTextures[map], replacement });
        }
    }

    /// Rebinds bucket samplers after a filter change.
    void GpuScene::rebindSamplersLocked(Id material)
    {
        const auto surface = mSurfaces.find(mMaterialPaths[material]);
        if (surface == mSurfaces.end() || surface->second.mBucket >= mBuckets.size())
            return;
        const Id bucket = surface->second.mBucket;
        PendingMaterial& current = mMaterials[material];
        const MaterialSource& source = *mMaterialSources[material];
        const bool legacy = current.mMode == Shader::MaterialMode::Legacy;
        for (unsigned int map = 0; map < sMapCount; ++map)
        {
            const int unit = source.mUnits[map];
            if (current.mTextures[map] == nullptr || unit < 0)
                continue;
            const bool reduced = mTextures->demoted(current, map);
            if (legacy && !reduced)
                continue;
            const osg::Texture* origin = current.mTextures[map].get();
            if (reduced && source.mTextures[map] != nullptr)
                origin = dynamic_cast<const osg::Texture*>(source.mTextures[map]->first.get());
            if (origin == nullptr)
                continue;
            const osg::ref_ptr<osg::Sampler> replacement = mTextures->sampler(*origin);
            if (replacement == current.mSamplers[map])
                continue;
            current.mSamplers[map] = replacement;
            // Materials sharing a unit share its sampler.
            const auto queued = [&](const BucketEdit& edit) {
                return edit.mBucket == bucket && edit.mUnit == static_cast<unsigned int>(unit);
            };
            if (std::none_of(mBucketEdits.begin(), mBucketEdits.end(), queued))
                mBucketEdits.push_back(BucketEdit{ bucket, static_cast<unsigned int>(unit), nullptr, replacement });
        }
    }

    void GpuScene::refreshSamplers()
    {
        if (!isSupported())
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        for (const Id material : mActiveMaterials)
        {
            mMaterialFrames[material] = std::numeric_limits<unsigned int>::max();
            refreshMaterialLocked(material, mLifetimeFrame, true);
        }
    }

    void GpuScene::addMaterialLocked(Id material)
    {
        if (material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published)
            return;
        ++mMaterialUsers[material];
        mMaterialStates[material] = MaterialState::Active;
    }

    void GpuScene::dropMaterialLocked(Id material)
    {
        if (material < mMaterialUsers.size() && mMaterialUsers[material] != 0)
            --mMaterialUsers[material];
    }

    void GpuScene::markMaterialLocked(Id material, unsigned int frame)
    {
        if (material >= mMaterialStates.size() || mMaterialStates[material] == MaterialState::Published)
            return;
        if (mMaterialLast[material] == frame)
            return;
        mMaterialLast[material] = std::max(mMaterialLast[material], frame);
        mMaterialStates[material] = MaterialState::Active;
    }

    void GpuScene::retireMaterialsLocked(unsigned int frame)
    {
        constexpr unsigned int grace = 3;
        // Publishing erases the slot from the list.
        for (std::size_t i = mActiveMaterials.size(); i-- > 0;)
        {
            const Id material = mActiveMaterials[i];
            MaterialState& state = mMaterialStates[material];
            if (mMaterialUsers[material] != 0 || frame <= mMaterialLast[material] + grace)
            {
                state = MaterialState::Active;
                continue;
            }
            if (state == MaterialState::Active)
            {
                state = MaterialState::Queued;
                continue;
            }
            publishMaterialLocked(material);
        }
        std::erase_if(mFailedSurfaces, [&](const auto& entry) { return frame > entry.second + grace; });
    }

    /// Retires the slot; its id never returns.
    void GpuScene::publishMaterialLocked(Id material)
    {
        const auto surface = mSurfaces.find(mMaterialPaths[material]);
        if (surface != mSurfaces.end())
        {
            const Id bucket = surface->second.mBucket;
            mSurfaces.erase(surface);
            if (bucket < mBuckets.size() && mBuckets[bucket].mMaterials != 0 && --mBuckets[bucket].mMaterials == 0
                && mBuckets[bucket].mMode == Shader::MaterialMode::Legacy)
            {
                mBucketEdits.push_back(BucketEdit{ bucket, 0, nullptr, nullptr });
            }
        }
        std::erase(mActiveMaterials, material);
        std::erase_if(mPendingMaterials, [&](const PendingMaterial& pending) { return pending.mIndex == material; });
        mPendingRetired.push_back(PendingRetire{ std::move(mMaterials[material]), true });
        mMaterials[material] = PendingMaterial{};
        mMaterials[material].mIndex = material;
        mMaterialSources[material].reset();
        mMaterialPaths[material].clear();
        mMaterialStates[material] = MaterialState::Published;
    }
}
