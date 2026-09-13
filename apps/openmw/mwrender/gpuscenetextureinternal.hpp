#ifndef OPENMW_MWRENDER_GPUSCENETEXTUREINTERNAL_H
#define OPENMW_MWRENDER_GPUSCENETEXTUREINTERNAL_H

namespace MWRender
{
    struct GpuScene::Textures
    {
        using StorageKey = MWRender::StorageKey;
        using SamplerKey = MWRender::SamplerKey;

        /// A fence guards pending layers.
        enum class LayerState : std::uint8_t
        {
            Used,
            Pending,
            Staged,
            Free,
        };

        struct Layer
        {
            osg::ref_ptr<osg::Image> mImage;
            osg::ref_ptr<osg::Texture2D> mSource;
            std::uint32_t mUsers = 0;
            std::uint64_t mRevision = 0;
            LayerState mState = LayerState::Used;
        };

        struct Pool;

        /// Binds the pool name the renderer owns.
        class ArrayTexture : public osg::Texture2DArray
        {
        public:
            explicit ArrayTexture(Pool& pool)
                : mPool(pool)
            {
            }

            void apply(osg::State& state) const override;
            int compare(const osg::StateAttribute& sa) const override;

        protected:
            ~ArrayTexture() override = default;

        private:
            Pool& mPool;
        };

        struct PoolGl
        {
            GLuint mName = 0;
            std::uint32_t mCapacity = 0;
            std::vector<std::uint64_t> mRevisions;
            std::vector<unsigned int> mModified;
        };

        struct Pool
        {
            StorageKey mKey;
            osg::ref_ptr<ArrayTexture> mTexture;
            std::vector<Layer> mLayers;
            std::map<const osg::Image*, std::uint32_t> mImages;
            std::map<const osg::Texture2D*, std::uint32_t> mSources;
            std::map<unsigned int, PoolGl> mContexts;
            Id mId = sInvalid;
            std::uint32_t mCapacity = 0;
            std::uint32_t mUsers = 0;
            /// Bumped when the pool loses every user.
            std::uint64_t mLife = 0;
            bool mFailed = false;
            /// One upload warning per pool.
            bool mWarned = false;
            mutable std::mutex mMutex;
        };

        struct NameTicket
        {
            GLuint mName = 0;
            std::size_t mBytes = 0;
        };

        struct LayerTicket
        {
            Id mPool = sInvalid;
            std::uint32_t mLayer = 0;
            std::uint64_t mLife = 0;
        };

        using CopyImageFn = void(GL_APIENTRY*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint,
            GLint, GLint, GLsizei, GLsizei, GLsizei);

        /// Budget record; tiers appear only under pressure.
        struct Proxy
        {
            struct Tier
            {
                osg::ref_ptr<osg::Image> mImage;
                osg::ref_ptr<osg::Texture2D> mTexture;
                std::size_t mBytes = 0;
            };

            osg::ref_ptr<osg::Image> mImage;
            /// Tier zero; the scene owns it.
            osg::ref_ptr<osg::Texture2D> mSource;
            osg::ref_ptr<osg::Texture2D> mTemplate;
            osg::ref_ptr<osg::Texture2D> mTexture;
            StorageKey mKey;
            std::vector<Tier> mTiers;
            std::size_t mBytes = 0;
            std::uint32_t mUsers = 0;
            unsigned int mLastVisible = 0;
            unsigned int mCurrent = 0;
            unsigned int mDesired = 0;
            unsigned int mFloor = 0;
            bool mPrepared = false;
            bool mRetiring = false;
            std::vector<Id> mMaterials;
        };

        struct Reservation
        {
            Id mPool = sInvalid;
            std::uint32_t mLayer = 0;
        };

        /// Builds proxy tiers on a worker thread.
        class TierWork : public SceneUtil::WorkItem
        {
        public:
            void doWork() override { buildTiers(*this); }

            Id mProxy = sInvalid;
            osg::ref_ptr<osg::Image> mImage;
            osg::ref_ptr<osg::Texture2D> mTemplate;
            StorageKey mKey;
            unsigned int mFloor = 0;
            std::vector<Proxy::Tier> mTiers;

        protected:
            ~TierWork() override = default;
        };

        Textures(int budget, int maxLayers)
            : mBudgetSetting(budget)
            , mMaxLayers(std::max(maxLayers, 0))
        {
            constexpr std::size_t maxArrays = 1024ull << 20;
            if (budget > 0)
                mBudgetBytes = static_cast<std::size_t>(budget) << 20;
            mArrayLimit = budget > 0 ? mBudgetBytes : maxArrays;
        }

        void resolve(osg::State& state)
        {
            if (mResolved)
                return;
            mResolved = true;
            std::string_view source;
            std::size_t mib = 0;
            if (mBudgetSetting > 0)
            {
                mib = static_cast<std::size_t>(mBudgetSetting);
                source = "setting";
            }
            else if (mBudgetSetting < 0)
            {
                GLint memory[4]{};
                if (osg::isGLExtensionSupported(state.getContextID(), "GL_NVX_gpu_memory_info"))
                {
                    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, memory);
                    source = "NVX";
                }
                else if (osg::isGLExtensionSupported(state.getContextID(), "GL_ATI_meminfo"))
                {
                    glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, memory);
                    source = "ATI";
                }
                if (memory[0] > 0)
                    mib = static_cast<std::size_t>(memory[0]) * 3 / 4 / 1024;
                if (mib == 0)
                {
                    // Unknown drivers still need eviction pressure.
                    mib = 1024;
                    source = "fallback";
                }
            }
            mBudgetBytes = mib << 20;
            if (mBudgetBytes != 0)
                mArrayLimit = mBudgetBytes;
            if (mBudgetBytes == 0)
                Log(Debug::Info) << "GPU texture budget is unlimited.";
            else
                Log(Debug::Info) << "GPU texture budget: " << source << ", " << mib << " MiB.";
        }

        osg::ref_ptr<osg::Sampler> sampler(const osg::Texture& texture)
        {
            const SamplerKey key = samplerKey(texture);
            const auto found = mSamplers.find(key);
            if (found != mSamplers.end())
                return found->second;
            osg::ref_ptr<GpuSampler> result = new GpuSampler;
            result->setWrap(osg::Texture::WRAP_S, texture.getWrap(osg::Texture::WRAP_S));
            result->setWrap(osg::Texture::WRAP_T, texture.getWrap(osg::Texture::WRAP_T));
            result->setWrap(osg::Texture::WRAP_R, texture.getWrap(osg::Texture::WRAP_R));
            result->setFilter(osg::Texture::MIN_FILTER, texture.getFilter(osg::Texture::MIN_FILTER));
            result->setFilter(osg::Texture::MAG_FILTER, texture.getFilter(osg::Texture::MAG_FILTER));
            result->setMaxAnisotropy(texture.getMaxAnisotropy());
            result->setMinLOD(texture.getMinLOD());
            result->setMaxLOD(texture.getMaxLOD());
            result->setLODBias(texture.getLODBias());
            result->setBorderColor(texture.getBorderColor());
            result->setShadowCompareFunc(texture.getShadowCompareFunc());
            result->setShadowTextureMode(
                texture.getShadowComparison() ? texture.getShadowTextureMode() : osg::Texture::NONE);
            mSamplers.emplace(key, result);
            return result;
        }

        bool makeArray(PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline);
        /// Track textures while retaining ordinary per-bucket texture bindings.
        void makeLegacy(PendingMaterial& material, const MaterialSource& source, osg::StateSet& pipeline);
        void makeBindless(PendingMaterial& material);
        /// Return whether a material map currently uses a reduced-resolution proxy.
        bool demoted(const PendingMaterial& material, unsigned int map) const
        {
            const Id proxy = material.mProxies[map];
            return proxy < mProxies.size() && mProxies[proxy].mCurrent != 0;
        }

        /// Return whether any material map currently uses a reduced-resolution proxy.
        bool demoted(const PendingMaterial& material) const
        {
            for (unsigned int map = 0; map < sMapCount; ++map)
                if (demoted(material, map))
                    return true;
            return false;
        }
        void useProxies(PendingMaterial& material);
        void prepareTiers();
        osg::ref_ptr<TierWork> pickTiers();
        void backProxy(Proxy& proxy);
        static void buildTiers(TierWork& work);
        void attachTiers(TierWork& work);
        void manage(GpuScene& scene, unsigned int frame);
        void switchTier(GpuScene& scene, Id proxy, unsigned int tier);
        /// Records budget bytes without building a proxy.
        Id track(osg::Texture& texture, osg::ref_ptr<osg::Texture>& out);
        Reservation reserve(osg::Texture2D& texture, const char** reason = nullptr);
        void rollback(const std::vector<Reservation>& reservations);
        void release(const PendingMaterial& material, osg::State* state = nullptr);
        /// A failed pool, kept layers, abandoned bytes.
        struct PoolFailure
        {
            Id mPool;
            std::uint32_t mKept;
            std::size_t mBytes;
        };

        bool syncPool(osg::State& state, Pool& pool, std::vector<PoolFailure>& failures);
        bool allocate(osg::State& state, Pool& pool, PoolGl& gl, std::vector<PoolFailure>& failures);
        bool failPool(osg::State& state, Pool& pool, GLuint name, std::uint32_t kept, const char* message,
            std::vector<PoolFailure>& failures);
        void uploadLayer(osg::State& state, const Pool& pool, const Layer& layer, std::uint32_t index) const;
        void uploadPool(osg::State& state, const Pool& pool, PoolGl& gl, std::vector<std::uint32_t>& uploaded) const;
        void dropMaterials(GpuScene& scene, Id pool, std::uint32_t kept);
        void release(const PendingRetire& retired, osg::State* state = nullptr)
        {
            if (retired.mBytes != 0)
            {
                std::set<osg::Texture*> textures;
                for (const osg::ref_ptr<osg::Texture>& texture : retired.mMaterial.mTextures)
                    if (texture != nullptr && textures.insert(texture.get()).second)
                        releaseTexture(*texture, state);
                mPendingBytes -= std::min(mPendingBytes, retired.mBytes);
            }
            if (retired.mProxy < mProxies.size())
            {
                Proxy& proxy = mProxies[retired.mProxy];
                proxy.mRetiring = false;
                if (proxy.mImage == nullptr)
                    mFreeProxies.push_back(retired.mProxy);
            }
            release(retired.mMaterial, state);
        }

        static void releaseTexture(osg::Texture& texture, osg::State* state)
        {
            osg::Texture::TextureObject* object
                = state != nullptr ? texture.getTextureObject(state->getContextID()) : nullptr;
            const osg::ref_ptr<osg::TextureObjectSet> set = object != nullptr ? object->_set : nullptr;
            texture.releaseGLObjects(state);
            // Bindless texture objects cannot be reused.
            if (set != nullptr)
                set->flushAllDeletedTextureObjects();
        }
        void releaseGLObjects(osg::State* state)
        {
            for (const auto& [key, sampler] : mSamplers)
                sampler->releaseGLObjects(state);
            for (Proxy& proxy : mProxies)
                for (std::size_t tier = 1; tier < proxy.mTiers.size(); ++tier)
                    proxy.mTiers[tier].mTexture->releaseGLObjects(state);
            std::vector<NameTicket> names;
            {
                std::lock_guard<std::mutex> lock(mStageMutex);
                if (state == nullptr)
                {
                    mStagedNames.clear();
                    mStagedLayers.clear();
                    mNameBytes.store(0, std::memory_order_relaxed);
                }
                else
                {
                    const auto found = mStagedNames.find(state->getContextID());
                    if (found != mStagedNames.end())
                    {
                        names.swap(found->second);
                        mStagedNames.erase(found);
                    }
                }
            }
            for (const std::unique_ptr<Pool>& pool : mPools)
            {
                if (pool == nullptr)
                    continue;
                std::lock_guard<std::mutex> lock(pool->mMutex);
                if (state == nullptr)
                {
                    pool->mContexts.clear();
                    continue;
                }
                const auto found = pool->mContexts.find(state->getContextID());
                if (found == pool->mContexts.end())
                    continue;
                names.push_back({ found->second.mName, 0 });
                pool->mContexts.erase(found);
            }
            if (state == nullptr)
                mSyncedArrays.clear();
            else
            {
                deleteNames(names);
                mSyncedArrays.erase(state->getContextID());
            }
        }
        /// Uploads arrays without the scene lock.
        void sync(GpuScene& scene, osg::State& state)
        {
            std::vector<Pool*> pools;
            std::uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(scene.mMutex);
                generation = mArrayGeneration;
                if (mSyncedArrays[state.getContextID()] == generation)
                    return;
                pools.reserve(mPools.size());
                for (const std::unique_ptr<Pool>& pool : mPools)
                    if (pool != nullptr)
                        pools.push_back(pool.get());
            }
            bool bound = false;
            std::vector<PoolFailure> failures;
            for (Pool* pool : pools)
                bound = syncPool(state, *pool, failures) || bound;
            {
                std::lock_guard<std::mutex> lock(scene.mMutex);
                mSyncedArrays[state.getContextID()] = generation;
                for (const PoolFailure& failure : failures)
                {
                    mArrayBytes -= std::min(failure.mBytes, mArrayBytes);
                    dropMaterials(scene, failure.mPool, failure.mKept);
                }
            }
            if (!bound)
                return;
            // Raw binds invalidate OSG texture tracking.
            state.haveAppliedTextureAttribute(sArrayUnit, osg::StateAttribute::TEXTURE);
            const unsigned int units = static_cast<unsigned int>(state.getTextureAttributeMapList().size());
            for (unsigned int unit = 0; unit < units; ++unit)
                state.haveAppliedTextureAttribute(unit, osg::StateAttribute::TEXTURE);
        }
        void stageName(unsigned int context, GLuint name, std::size_t bytes)
        {
            if (name == 0)
                return;
            std::lock_guard<std::mutex> lock(mStageMutex);
            mStagedNames[context].push_back({ name, bytes });
            mNameBytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        void stageLayer(const Pool& pool, std::uint32_t layer)
        {
            std::lock_guard<std::mutex> lock(mStageMutex);
            mStagedLayers.push_back({ pool.mId, layer, pool.mLife });
        }
        void takeStaged(unsigned int context, std::vector<NameTicket>& names, std::vector<LayerTicket>& layers)
        {
            std::lock_guard<std::mutex> lock(mStageMutex);
            const auto found = mStagedNames.find(context);
            if (found != mStagedNames.end())
            {
                names.insert(names.end(), found->second.begin(), found->second.end());
                mStagedNames.erase(found);
            }
            layers.insert(layers.end(), mStagedLayers.begin(), mStagedLayers.end());
            mStagedLayers.clear();
        }
        /// Deletes names with the context current.
        void deleteNames(std::vector<NameTicket>& names)
        {
            std::size_t bytes = 0;
            std::vector<GLuint> ids;
            ids.reserve(names.size());
            for (const NameTicket& ticket : names)
            {
                bytes += ticket.mBytes;
                if (ticket.mName != 0)
                    ids.push_back(ticket.mName);
            }
            if (!ids.empty())
                glDeleteTextures(static_cast<GLsizei>(ids.size()), ids.data());
            mNameBytes.fetch_sub(
                std::min(bytes, mNameBytes.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            names.clear();
        }
        void freeLayers(std::vector<LayerTicket>& layers)
        {
            for (const LayerTicket& ticket : layers)
            {
                if (ticket.mPool >= mPools.size() || mPools[ticket.mPool] == nullptr)
                    continue;
                Pool& pool = *mPools[ticket.mPool];
                std::lock_guard<std::mutex> lock(pool.mMutex);
                if (pool.mLife != ticket.mLife || ticket.mLayer >= pool.mLayers.size())
                    continue;
                Layer& layer = pool.mLayers[ticket.mLayer];
                if (layer.mState == LayerState::Staged)
                    layer.mState = LayerState::Free;
            }
            layers.clear();
        }
        std::size_t pendingBytes() const { return mPendingBytes + mNameBytes.load(std::memory_order_relaxed); }
        /// Budget left after the geometry buffers.
        std::size_t budget() const
        {
            const std::size_t reserved = mReservedBytes.load(std::memory_order_relaxed);
            return mBudgetBytes == 0 ? 0 : std::max(mBudgetBytes / 4, mBudgetBytes - std::min(reserved, mBudgetBytes));
        }
        void warnTexture(const osg::Texture* texture, std::string_view reason, const char* cause)
        {
            const osg::Image* image = texture != nullptr ? texture->getImage(0) : nullptr;
            std::string name;
            if (image != nullptr && !image->getFileName().empty())
                name = image->getFileName();
            else if (texture != nullptr && !texture->getName().empty())
                name = texture->getName();
            else if (image != nullptr)
                name = std::string("generated ") + std::to_string(image->s()) + "x" + std::to_string(image->t());
            else if (texture != nullptr)
                name = std::string(texture->className()) + " " + std::to_string(texture->getTextureWidth()) + "x"
                    + std::to_string(texture->getTextureHeight());
            Log(Debug::Verbose) << "GPU texture fallback: " << (name.empty() ? "<unnamed>" : name) << " (" << cause
                                << ")";
            if (!mWarnings.emplace(texture, reason).second)
                return;
            std::size_t& count = mWarningCounts[std::string(reason)];
            ++count;
            if (count <= 8)
                Log(Debug::Warning) << "GPU texture uses material draws: " << (name.empty() ? "<unnamed>" : name)
                                    << " (" << cause << ")";
            else if (count == 9)
                Log(Debug::Warning) << "More GPU textures use material draws.";
        }

        int mBudgetSetting = -1;
        int mMaxLayers = 0;
        std::size_t mArrayLimit = 0;
        std::size_t mArrayBytes = 0;
        std::size_t mProxyBytes = 0;
        std::size_t mPendingBytes = 0;
        std::atomic<std::size_t> mNameBytes{ 0 };
        std::size_t mBudgetBytes = 0;
        /// Buffer capacities the draw thread holds.
        std::atomic<std::size_t> mReservedBytes{ 0 };
        std::uint64_t mMipChanges = 0;
        bool mResolved = false;
        std::map<SamplerKey, osg::ref_ptr<osg::Sampler>> mSamplers;
        std::map<StorageKey, Id> mPoolIds;
        std::vector<std::unique_ptr<Pool>> mPools;
        std::uint64_t mArrayGeneration = 1;
        std::map<unsigned int, std::uint64_t> mSyncedArrays;
        std::map<std::pair<const osg::Image*, StorageKey>, Id> mProxyIds;
        std::vector<Proxy> mProxies;
        std::vector<Id> mFreeProxies;
        SceneUtil::WorkQueue* mWorkQueue = nullptr;
        osg::ref_ptr<TierWork> mTierWork;
        std::mutex mStageMutex;
        std::map<unsigned int, std::vector<NameTicket>> mStagedNames;
        std::vector<LayerTicket> mStagedLayers;
        mutable CopyImageFn mCopyImage = nullptr;
        std::set<std::pair<const osg::Texture*, std::string>> mWarnings;
        std::map<std::string, std::size_t> mWarningCounts;
    };

    struct GpuScene::MaterialSource
    {
        std::vector<const osg::StateSet*> mPath;
        const osg::StateSet::RefAttributePair* mMaterial = nullptr;
        std::array<const osg::StateSet::RefUniformPair*, sMapCount> mMaps{};
        std::array<const osg::StateSet::RefAttributePair*, sMapCount> mTextures{};
        std::array<int, sMapCount> mUnits{};
        bool mMapsInitialized = false;
        const osg::StateSet::RefUniformPair* mEnvMapColor = nullptr;
        const osg::StateSet::RefUniformPair* mAlphaRef = nullptr;
        const osg::StateSet::RefUniformPair* mLumaBias = nullptr;
        const osg::StateSet::RefUniformPair* mBumpMatrix = nullptr;
        std::array<const osg::StateSet::RefUniformPair*, 8> mTexMats{};
    };
}

#endif
