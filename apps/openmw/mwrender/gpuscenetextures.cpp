#include "gpusceneinternal.hpp"

#include <SDL_opengl_glext.h>

namespace MWRender
{
    void GpuSampler::compileGLObjects(osg::State& state) const
    {
        const unsigned int context = state.getContextID();
        if (!_PCdirtyflags[context])
            return;
        const osg::GLExtensions& ext = *state.get<osg::GLExtensions>();
        GLuint& sampler = _PCsampler[context];
        if (sampler == 0)
            ext.glGenSamplers(1, &sampler);

        // OSG's Windows wrap-R enum is incorrect.
        ext.glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, _wrap_s);
        ext.glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, _wrap_t);
        ext.glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, _wrap_r);
        ext.glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, _min_filter);
        ext.glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, _mag_filter);
        osg::Vec4f border(_borderColor);
        ext.glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border.ptr());
        ext.glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE,
            _shadow_texture_mode == osg::Texture::NONE ? GL_NONE : GL_COMPARE_R_TO_TEXTURE);
        ext.glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, _shadow_compare_func);
        if (ext.isTextureFilterAnisotropicSupported)
            ext.glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, _maxAnisotropy);
        if (_maxlod >= _minlod)
        {
            ext.glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, _minlod);
            ext.glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, _maxlod);
        }
        ext.glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, _lodbias);
        _PCdirtyflags[context] = false;
    }

    void GpuScene::Textures::ArrayTexture::apply(osg::State& state) const
    {
        GLuint name = 0;
        {
            std::lock_guard<std::mutex> lock(mPool.mMutex);
            const auto found = mPool.mContexts.find(state.getContextID());
            if (found != mPool.mContexts.end())
                name = found->second.mName;
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    }

    int GpuScene::Textures::ArrayTexture::compare(const osg::StateAttribute& sa) const
    {
        COMPARE_StateAttribute_Types(ArrayTexture, sa);
        if (&mPool < &rhs.mPool)
            return -1;
        if (&mPool > &rhs.mPool)
            return 1;
        return 0;
    }

    void GpuScene::Textures::uploadLayer(
        osg::State& state, const Pool& pool, const Layer& layer, std::uint32_t index) const
    {
        const osg::Image* image = layer.mImage.get();
        if (image == nullptr || !image->valid())
            return;
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        const StorageKey& key = pool.mKey;
        state.unbindPixelBufferObject();
        glPixelStorei(GL_UNPACK_ALIGNMENT, image->getPacking());
#if !defined(OSG_GLES1_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, image->getRowLength());
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
#endif
        const bool compressed = osg::Texture::isCompressedInternalFormat(key.mInternal);
        const unsigned int levels = key.mGenerated ? 1 : key.mLevels;
        for (unsigned int level = 0; level < levels; ++level)
        {
            const GLsizei width = std::max(key.mWidth >> level, 1);
            const GLsizei height = std::max(key.mHeight >> level, 1);
            const std::size_t offset = image->isMipmap() ? image->getMipmapOffset(level) : 0;
            const unsigned char* data = image->data() + offset;
            if (compressed)
            {
                const std::size_t size = osg::Image::computeImageSizeInBytes(width, height, 1, key.mFormat, key.mType);
                ext->glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0,
                    static_cast<GLint>(index), width, height, 1, static_cast<GLenum>(key.mInternal),
                    static_cast<GLsizei>(size), data);
            }
            else
                ext->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0, static_cast<GLint>(index),
                    width, height, 1, key.mFormat, key.mType, data);
        }
    }

    void GpuScene::Textures::uploadPool(
        osg::State& state, const Pool& pool, PoolGl& gl, std::vector<std::uint32_t>& uploaded) const
    {
        const std::uint32_t count = static_cast<std::uint32_t>(std::min(pool.mLayers.size(), gl.mRevisions.size()));
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const Layer& layer = pool.mLayers[index];
            if (layer.mImage == nullptr)
            {
                if (layer.mSource == nullptr || gl.mRevisions[index] == layer.mRevision)
                    continue;
                const osg::Texture::TextureObject* object = layer.mSource->getTextureObject(state.getContextID());
                if (object == nullptr)
                    continue;
                if (mCopyImage == nullptr)
                    osg::setGLExtensionFuncPtr(mCopyImage, "glCopyImageSubData");
                if (mCopyImage == nullptr)
                    continue;
                mCopyImage(object->id(), GL_TEXTURE_2D, 0, 0, 0, 0, gl.mName, GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                    static_cast<GLint>(index), pool.mKey.mWidth, pool.mKey.mHeight, 1);
                gl.mRevisions[index] = layer.mRevision;
                continue;
            }
            const unsigned int modified = layer.mImage->getModifiedCount();
            if (gl.mRevisions[index] == layer.mRevision && gl.mModified[index] == modified)
                continue;
            uploadLayer(state, pool, layer, index);
            gl.mRevisions[index] = layer.mRevision;
            gl.mModified[index] = modified;
            uploaded.push_back(index);
        }
    }

    /// The caller drops the affected materials.
    bool GpuScene::Textures::failPool(osg::State& state, Pool& pool, GLuint name, std::uint32_t kept,
        const char* message, std::vector<PoolFailure>& failures)
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        glDeleteTextures(1, &name);
        pool.mFailed = true;
        std::size_t bytes = 0;
        if (kept < pool.mCapacity)
        {
            // The caller removes the abandoned bytes.
            bytes = static_cast<std::size_t>(pool.mCapacity - kept) * pool.mKey.mBytes;
            pool.mCapacity = kept;
        }
        Log(Debug::Warning) << message;
        failures.push_back(PoolFailure{ pool.mId, kept, bytes });
        return false;
    }

    /// Publishes the replacement only when complete.
    bool GpuScene::Textures::allocate(osg::State& state, Pool& pool, PoolGl& gl, std::vector<PoolFailure>& failures)
    {
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        const StorageKey& key = pool.mKey;
        PoolGl next;
        next.mCapacity = pool.mCapacity;
        glGenTextures(1, &next.mName);
        state.setActiveTextureUnit(sArrayUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, next.mName);
        clearErrors();
        ext->glTexStorage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLsizei>(key.mLevels), key.mInternal, key.mWidth,
            key.mHeight, static_cast<GLsizei>(pool.mCapacity));
        if (glGetError() != GL_NO_ERROR)
            return failPool(state, pool, next.mName, gl.mCapacity, "GPU texture array storage failed.", failures);
        constexpr std::array<int, 4> identity{ GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
        if (key.mSwizzle != identity)
            glTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_RGBA, key.mSwizzle.data());
        next.mRevisions.assign(pool.mCapacity, 0);
        next.mModified.assign(pool.mCapacity, 0);
        if (gl.mName != 0 && gl.mCapacity != 0)
        {
            if (mCopyImage == nullptr)
                osg::setGLExtensionFuncPtr(mCopyImage, "glCopyImageSubData");
            if (mCopyImage != nullptr)
            {
                const GLsizei depth = static_cast<GLsizei>(std::min(gl.mCapacity, pool.mCapacity));
                for (unsigned int level = 0; level < key.mLevels; ++level)
                    mCopyImage(gl.mName, GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0, 0, next.mName,
                        GL_TEXTURE_2D_ARRAY, static_cast<GLint>(level), 0, 0, 0, std::max(key.mWidth >> level, 1),
                        std::max(key.mHeight >> level, 1), depth);
                // Only copied layers keep their revision.
                if (glGetError() == GL_NO_ERROR)
                {
                    const std::size_t count = std::min<std::size_t>(gl.mRevisions.size(), pool.mCapacity);
                    std::copy_n(gl.mRevisions.begin(), count, next.mRevisions.begin());
                    std::copy_n(gl.mModified.begin(), std::min<std::size_t>(gl.mModified.size(), count),
                        next.mModified.begin());
                }
                else
                    clearErrors();
            }
        }
        std::vector<std::uint32_t> uploaded;
        uploadPool(state, pool, next, uploaded);
        if (!uploaded.empty())
        {
            resetUnpack();
            if (key.mGenerated && ext->glGenerateMipmap != nullptr)
                ext->glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        }
        if (glGetError() != GL_NO_ERROR)
            return failPool(state, pool, next.mName, gl.mCapacity, "GPU texture array upload failed.", failures);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        stageName(state.getContextID(), gl.mName, static_cast<std::size_t>(gl.mCapacity) * key.mBytes);
        gl = std::move(next);
        return true;
    }

    bool GpuScene::Textures::syncPool(osg::State& state, Pool& pool, std::vector<PoolFailure>& failures)
    {
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        if (ext->glTexStorage3D == nullptr)
            return false;
        std::lock_guard<std::mutex> lock(pool.mMutex);
        for (std::uint32_t index = 0; index < pool.mLayers.size(); ++index)
        {
            Layer& layer = pool.mLayers[index];
            if (layer.mState != LayerState::Pending)
                continue;
            layer.mState = LayerState::Staged;
            stageLayer(pool, index);
        }
        if (pool.mCapacity == 0 || pool.mLayers.empty())
            return false;
        PoolGl& gl = pool.mContexts[state.getContextID()];
        const bool attempted = !pool.mFailed && (gl.mName == 0 || gl.mCapacity != pool.mCapacity);
        // A grown pool uploads before it publishes.
        if (attempted && allocate(state, pool, gl, failures))
            return true;
        if (gl.mName == 0)
            return attempted;
        state.setActiveTextureUnit(sArrayUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, gl.mName);
        clearErrors();
        std::vector<std::uint32_t> uploaded;
        uploadPool(state, pool, gl, uploaded);
        if (!uploaded.empty())
        {
            resetUnpack();
            if (pool.mKey.mGenerated && ext->glGenerateMipmap != nullptr)
                ext->glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
            if (glGetError() != GL_NO_ERROR)
            {
                pool.mFailed = true;
                failures.push_back({ pool.mId, 0, 0 });
                if (!pool.mWarned)
                {
                    pool.mWarned = true;
                    Log(Debug::Warning) << "GPU texture array upload failed.";
                }
            }
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        state.setActiveTextureUnit(0);
        return true;
    }

    void GpuScene::Textures::dropMaterials(GpuScene& scene, Id pool, std::uint32_t kept)
    {
        std::vector<Id> dropped;
        for (const Id material : scene.mActiveMaterials)
        {
            const PendingMaterial& pending = scene.mMaterials[material];
            bool affected = false;
            for (unsigned int map = 0; map < sMapCount; ++map)
                affected = affected || (pending.mArrays[map] == pool && pending.mLayers[map] >= kept);
            if (affected && scene.mMaterialUsers[material] != 0)
            {
                scene.mSupported = false;
                scene.mDemoted.store(true, std::memory_order_relaxed);
            }
            // Used materials retire when users drop.
            if (affected && scene.mMaterialUsers[material] == 0)
                dropped.push_back(material);
        }
        for (const Id material : dropped)
            scene.publishMaterialLocked(material);
    }

    GpuScene::Textures::Reservation GpuScene::Textures::reserve(osg::Texture2D& texture, const char** reason)
    {
        StorageKey key;
        bool fromSource = false;
        if (!storage(texture, key, reason))
        {
            // GL-resident render targets copy into layers.
            if (texture.getImage() != nullptr || !storageFromTexture(texture, key))
                return {};
            fromSource = true;
        }
        const osg::Image* image = texture.getImage();
        Id poolId = sInvalid;
        const auto poolFound = mPoolIds.find(key);
        if (poolFound != mPoolIds.end())
            poolId = poolFound->second;
        else
        {
            poolId = static_cast<Id>(mPools.size());
            auto pool = std::make_unique<Pool>();
            pool->mKey = key;
            pool->mId = poolId;
            pool->mTexture = new ArrayTexture(*pool);
            mPoolIds.emplace(key, poolId);
            mPools.push_back(std::move(pool));
        }
        Pool& pool = *mPools[poolId];
        std::lock_guard<std::mutex> lock(pool.mMutex);
        if (pool.mFailed)
        {
            if (reason != nullptr)
                *reason = "pool allocation failed";
            return {};
        }
        const auto imageFound = fromSource ? pool.mImages.end() : pool.mImages.find(image);
        if (imageFound != pool.mImages.end())
        {
            Layer& layer = pool.mLayers[imageFound->second];
            ++layer.mUsers;
            ++pool.mUsers;
            return { poolId, imageFound->second };
        }
        if (fromSource)
        {
            const auto sourceFound = pool.mSources.find(&texture);
            if (sourceFound != pool.mSources.end())
            {
                Layer& layer = pool.mLayers[sourceFound->second];
                ++layer.mUsers;
                ++pool.mUsers;
                return { poolId, sourceFound->second };
            }
        }

        auto layerIt = std::find_if(pool.mLayers.begin(), pool.mLayers.end(),
            [](const Layer& layer) { return layer.mState == LayerState::Free; });
        if (layerIt == pool.mLayers.end())
        {
            const std::size_t oldBytes = static_cast<std::size_t>(pool.mCapacity) * key.mBytes;
            const std::size_t otherBytes = mArrayBytes - oldBytes;
            std::size_t headroom = mArrayLimit - std::min(otherBytes, mArrayLimit);
            if (mBudgetBytes != 0)
            {
                const std::size_t used = otherBytes + mProxyBytes + pendingBytes();
                headroom = std::min(headroom, mBudgetBytes - std::min(used, mBudgetBytes));
            }
            const std::uint32_t affordable = static_cast<std::uint32_t>(
                std::min<std::size_t>(headroom / key.mBytes, std::numeric_limits<std::uint32_t>::max()));
            const std::uint32_t maxCapacity = std::min(static_cast<std::uint32_t>(mMaxLayers), affordable);
            std::uint32_t capacity
                = pool.mCapacity == 0 ? std::min(2u, maxCapacity) : std::min(pool.mCapacity * 2, maxCapacity);
            if (capacity < 2 || capacity <= pool.mCapacity)
            {
                if (reason != nullptr)
                    *reason = "layer budget exhausted";
                return {};
            }
            const std::size_t bytes = static_cast<std::size_t>(capacity) * key.mBytes;
            const std::uint32_t oldCapacity = pool.mCapacity;
            pool.mCapacity = capacity;
            pool.mLayers.resize(capacity);
            for (std::uint32_t layer = oldCapacity; layer < capacity; ++layer)
                pool.mLayers[layer].mState = LayerState::Free;
            mArrayBytes = mArrayBytes - oldBytes + bytes;
            layerIt = pool.mLayers.begin() + oldCapacity;
        }
        const std::uint32_t layer = static_cast<std::uint32_t>(std::distance(pool.mLayers.begin(), layerIt));
        layerIt->mUsers = 1;
        ++layerIt->mRevision;
        layerIt->mState = LayerState::Used;
        if (fromSource)
        {
            layerIt->mSource = &texture;
            pool.mSources.emplace(&texture, layer);
        }
        else
        {
            layerIt->mImage = texture.getImage();
            pool.mImages.emplace(image, layer);
        }
        ++pool.mUsers;
        ++mArrayGeneration;
        return { poolId, layer };
    }

    void GpuScene::Textures::rollback(const std::vector<Reservation>& reservations)
    {
        for (const Reservation& reservation : reservations)
        {
            if (reservation.mPool == sInvalid || reservation.mPool >= mPools.size()
                || mPools[reservation.mPool] == nullptr)
                continue;
            Pool& pool = *mPools[reservation.mPool];
            std::lock_guard<std::mutex> lock(pool.mMutex);
            Layer& layer = pool.mLayers[reservation.mLayer];
            --pool.mUsers;
            if (--layer.mUsers != 0)
                continue;
            pool.mImages.erase(layer.mImage.get());
            if (layer.mSource != nullptr)
                pool.mSources.erase(layer.mSource.get());
            layer.mSource = nullptr;
            layer.mImage = nullptr;
            // The fence frees the layer for reuse.
            layer.mState = LayerState::Pending;
            ++mArrayGeneration;
            if (pool.mUsers != 0)
                continue;
            mArrayBytes -= static_cast<std::size_t>(pool.mCapacity) * pool.mKey.mBytes;
            for (const auto& [context, gl] : pool.mContexts)
                stageName(context, gl.mName, static_cast<std::size_t>(gl.mCapacity) * pool.mKey.mBytes);
            pool.mContexts.clear();
            pool.mCapacity = 0;
            pool.mLayers.clear();
            pool.mImages.clear();
            pool.mFailed = false;
            pool.mWarned = false;
            ++pool.mLife;
        }
    }
}
