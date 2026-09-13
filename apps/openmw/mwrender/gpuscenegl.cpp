#include "gpusceneinternal.hpp"

namespace MWRender
{
    void GpuScene::Gl::deleteVaos()
    {
        GLuint vaos[] = { mVao, mDynamicVao };
        mExt->glDeleteVertexArrays(static_cast<GLsizei>(std::size(vaos)), vaos);
        mVao = 0;
        mDynamicVao = 0;
    }

    void GpuScene::Gl::beginUpload()
    {
        if (!mFlushUploads)
            return;
        if (mUploadCount == 0)
            glFlush();
        mUploadCount = (mUploadCount + 1) % 64;
    }

    bool GpuScene::Gl::grow(
        GLuint& buffer, std::size_t& capacity, std::size_t required, GLenum usage, std::size_t limit)
    {
        if (buffer != 0 && required <= capacity)
            return true;
        if (limit != 0 && required > limit)
            return false;
        std::size_t newCapacity = std::max<std::size_t>(capacity, 1 << 16);
        while (newCapacity < required)
            newCapacity += std::max<std::size_t>(newCapacity / 2, 1 << 16);
        if (limit != 0)
            newCapacity = std::min(newCapacity, limit);
        beginUpload();
        GLuint newBuffer = 0;
        mExt->glGenBuffers(1, &newBuffer);
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, newBuffer);
        for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        {
        }
        mExt->glBufferData(GL_COPY_WRITE_BUFFER, newCapacity, nullptr, usage);
        if (glGetError() != GL_NO_ERROR)
        {
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            mExt->glDeleteBuffers(1, &newBuffer);
            return false;
        }
        if (buffer != 0)
        {
            mExt->glBindBuffer(GL_COPY_READ_BUFFER, buffer);
            mCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, capacity);
            mExt->glBindBuffer(GL_COPY_READ_BUFFER, 0);
            mExt->glDeleteBuffers(1, &buffer);
        }
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        buffer = newBuffer;
        capacity = newCapacity;
        deleteVaos();
        return true;
    }

    void GpuScene::Gl::stream(GLuint& buffer, const void* data, std::size_t size, GLenum usage)
    {
        beginUpload();
        if (buffer == 0)
            mExt->glGenBuffers(1, &buffer);
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        mExt->glBufferData(GL_COPY_WRITE_BUFFER, std::max<std::size_t>(size, 4), data, usage);
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    void GpuScene::Gl::upload(GLuint buffer, std::size_t offset, const void* data, std::size_t size)
    {
        beginUpload();
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        mExt->glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, data);
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    void GpuScene::Gl::createVao(osg::State& state, bool dynamic)
    {
        GLuint& vao = dynamic ? mDynamicVao : mVao;
        if (vao != 0)
            return;
        mExt->glGenVertexArrays(1, &vao);
        const GLuint previous = state.getCurrentVertexArrayState() != nullptr
            ? state.getCurrentVertexArrayState()->getVertexArrayObject()
            : 0;
        state.bindVertexArrayObject(vao);
        mExt->glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
        const GLsizei stride = sizeof(GpuVertex);
        glVertexPointer(3, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mPosition)));
        glEnableClientState(GL_VERTEX_ARRAY);
        glNormalPointer(GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mNormal)));
        glEnableClientState(GL_NORMAL_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mColor)));
        glEnableClientState(GL_COLOR_ARRAY);
        for (unsigned int unit = 0; unit < 7; ++unit)
        {
            state.setClientActiveTextureUnit(unit);
            glTexCoordPointer(2, GL_FLOAT, stride,
                reinterpret_cast<const void*>(offsetof(GpuVertex, mUv) + unit * sizeof(float) * 2));
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        state.setClientActiveTextureUnit(7);
        glTexCoordPointer(4, GL_FLOAT, stride, reinterpret_cast<const void*>(offsetof(GpuVertex, mTexCoord7)));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        state.setClientActiveTextureUnit(0);
        mExt->glEnableVertexAttribArray(Shader::ShaderManager::sVertexMaterialAttribute);
        mExt->glVertexAttribIPointer(Shader::ShaderManager::sVertexMaterialAttribute, 1, GL_UNSIGNED_INT, stride,
            reinterpret_cast<const void*>(offsetof(GpuVertex, mMaterial)));
        mExt->glBindBuffer(GL_ARRAY_BUFFER, dynamic ? mDynamicVisibleBuffer : mVisibleBuffer);
        mExt->glVertexAttribIPointer(
            Shader::ShaderManager::sInstanceSlotAttribute, 1, GL_UNSIGNED_INT, sizeof(std::uint32_t), nullptr);
        mExt->glVertexAttribDivisor(Shader::ShaderManager::sInstanceSlotAttribute, 1);
        mExt->glEnableVertexAttribArray(Shader::ShaderManager::sInstanceSlotAttribute);
        mExt->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuffer);
        state.bindVertexArrayObject(previous);
        osg::VertexArrayState* vas = state.getCurrentVertexArrayState();
        const osg::GLBufferObject* vbo = vas != nullptr ? vas->getCurrentVertexBufferObject() : nullptr;
        mExt->glBindBuffer(GL_ARRAY_BUFFER, vbo != nullptr ? vbo->getGLObjectID() : 0);
    }

    GpuScene::Gl::HandleEntry& GpuScene::Gl::acquire(osg::State& state, const HandleKey& key)
    {
        auto [found, inserted] = mHandles.try_emplace(key);
        HandleEntry& entry = found->second;
        if (!inserted)
            return entry;
        beginUpload();
        osg::Texture& texture = *const_cast<osg::Texture*>(key.first.get());
        osg::Sampler& sampler = *const_cast<osg::Sampler*>(key.second.get());
        osg::Texture::TextureObject* object = texture.getTextureObject(state.getContextID());
        if (object == nullptr)
        {
            state.applyTextureAttribute(0, &texture);
            object = texture.getTextureObject(state.getContextID());
        }
        if (object != nullptr)
        {
            state.setActiveTextureUnit(0);
            glBindTexture(texture.getTextureTarget(), object->id());
            state.haveAppliedTextureAttribute(0, osg::StateAttribute::TEXTURE);
            GLint width = 0;
            glGetTexLevelParameteriv(texture.getTextureTarget(), 0, GL_TEXTURE_WIDTH, &width);
            if (width != 0)
            {
                sampler.compileGLObjects(state);
                const auto* managed = dynamic_cast<const GpuSampler*>(&sampler);
                const GLuint samplerObject = managed != nullptr ? managed->object(state.getContextID()) : 0;
                if (samplerObject != 0)
                {
                    entry.mHandle = mExt->glGetTextureSamplerHandle(object->id(), samplerObject);
                    if (entry.mHandle != 0)
                        mExt->glMakeTextureHandleResident(entry.mHandle);
                }
            }
        }
        if (entry.mHandle == 0)
        {
            const osg::Image* image = texture.getImage(0);
            Log(Debug::Warning) << "GPU bindless texture unavailable: "
                                << (image != nullptr ? image->getFileName() : texture.getName())
                                << " texture=" << (object != nullptr ? object->id() : 0);
        }
        return entry;
    }

    bool GpuScene::Gl::contains(const std::array<std::optional<HandleKey>, sMapCount>& keys, const HandleKey& key)
    {
        return std::any_of(keys.begin(), keys.end(),
            [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == key; });
    }

    void GpuScene::Gl::dropHandle(const HandleKey& key)
    {
        const auto found = mHandles.find(key);
        if (found == mHandles.end() || found->second.mUsers == 0)
            return;
        // Unused handles remain resident until textures leave.
        --found->second.mUsers;
    }

    void GpuScene::Gl::retireTextureHandles(const osg::Texture* texture)
    {
        for (auto& [key, entry] : mHandles)
        {
            if (key.first.get() != texture || entry.mUsers != 0)
                continue;
            ++entry.mGeneration;
            mPendingHandles.push_back({ key, entry.mGeneration, mTicketFrame });
        }
    }

    void GpuScene::Gl::setMaterial(osg::State& state, PendingMaterial& material)
    {
        if (material.mIndex >= mMaterialHandles.size())
            mMaterialHandles.resize(material.mIndex + 1);
        auto& previous = mMaterialHandles[material.mIndex];
        std::array<std::optional<HandleKey>, sMapCount> next;
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            if (material.mTextures[i] == nullptr || material.mSamplers[i] == nullptr)
            {
                material.mRecord.mMaps[i] = 0;
                continue;
            }
            HandleKey key(material.mTextures[i], material.mSamplers[i]);
            HandleEntry& entry = acquire(state, key);
            material.mRecord.mMaps[i] = entry.mHandle;
            next[i] = key;
            const bool duplicate = std::any_of(next.begin(), next.begin() + i,
                [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == key; });
            if (!duplicate && !contains(previous, key))
                ++entry.mUsers;
        }
        for (unsigned int i = 0; i < sMapCount; ++i)
        {
            if (!previous[i].has_value() || contains(next, *previous[i]))
                continue;
            const bool duplicate = std::any_of(previous.begin(), previous.begin() + i,
                [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == *previous[i]; });
            if (!duplicate)
                dropHandle(*previous[i]);
        }
        previous = std::move(next);
    }

    void GpuScene::Gl::retireMaterial(PendingRetire&& retired)
    {
        const Id material = retired.mMaterial.mIndex;
        if (retired.mFinal && material < mMaterialHandles.size())
        {
            auto& keys = mMaterialHandles[material];
            for (unsigned int i = 0; i < sMapCount; ++i)
            {
                if (!keys[i].has_value())
                    continue;
                const bool duplicate = std::any_of(keys.begin(), keys.begin() + i,
                    [&](const std::optional<HandleKey>& item) { return item.has_value() && *item == *keys[i]; });
                if (!duplicate)
                    dropHandle(*keys[i]);
            }
            keys = {};
        }
        // Handles retire with storage, not materials.
        if (retired.mBytes != 0)
            for (const osg::ref_ptr<osg::Texture>& texture : retired.mMaterial.mTextures)
                if (texture != nullptr)
                    retireTextureHandles(texture.get());
        mPendingRetired.push_back(std::move(retired));
    }

    void GpuScene::Gl::pollRetired(GpuScene& scene, osg::State& state)
    {
        std::vector<PendingRetire> released;
        std::vector<Textures::NameTicket> names;
        std::vector<Textures::LayerTicket> layers;
        for (auto batch = mRetired.begin(); batch != mRetired.end();)
        {
            const unsigned int frame = state.getFrameStamp() != nullptr ? state.getFrameStamp()->getFrameNumber() : 0;
            // Queued frames may still sample these textures.
            if (std::any_of(batch->mHandles.begin(), batch->mHandles.end(),
                    [frame](const HandleTicket& ticket) { return frame - ticket.mFrame < 2; }))
            {
                ++batch;
                continue;
            }
            const GLenum wait = mExt->glClientWaitSync(batch->mFence, 0, 0);
            if (wait == GL_TIMEOUT_EXPIRED || wait == GL_WAIT_FAILED)
            {
                ++batch;
                continue;
            }
            mExt->glDeleteSync(batch->mFence);
            for (const HandleTicket& ticket : batch->mHandles)
            {
                const auto found = mHandles.find(ticket.mKey);
                if (found == mHandles.end() || found->second.mUsers != 0
                    || found->second.mGeneration != ticket.mGeneration)
                    continue;
                if (found->second.mHandle != 0)
                    mExt->glMakeTextureHandleNonResident(found->second.mHandle);
                mHandles.erase(found);
            }
            released.insert(released.end(), std::make_move_iterator(batch->mMaterials.begin()),
                std::make_move_iterator(batch->mMaterials.end()));
            names.insert(names.end(), batch->mNames.begin(), batch->mNames.end());
            layers.insert(layers.end(), batch->mLayers.begin(), batch->mLayers.end());
            batch = mRetired.erase(batch);
        }
        if (!names.empty())
            scene.mTextures->deleteNames(names);
        if (!released.empty() || !layers.empty())
        {
            std::lock_guard<std::mutex> lock(scene.mMutex);
            for (const PendingRetire& retired : released)
                scene.mTextures->release(retired, &state);
            scene.mTextures->freeLayers(layers);
        }
    }

    void GpuScene::Gl::advance(GpuScene& scene, osg::State& state)
    {
        const osg::observer_ptr<osg::GraphicsThread> thread = state.getGraphicsContext()->getGraphicsThread();
        if (mDrawThread != thread)
        {
            if (!mHandles.empty())
                glFinish();
            for (const auto& [key, entry] : mHandles)
            {
                if (entry.mHandle == 0)
                    continue;
                mExt->glMakeTextureHandleNonResident(entry.mHandle);
                mExt->glMakeTextureHandleResident(entry.mHandle);
            }
            mDrawThread = thread;
        }
        if (state.getFrameStamp() != nullptr)
            mTicketFrame = state.getFrameStamp()->getFrameNumber();
        scene.mTextures->takeStaged(state.getContextID(), mPendingNames, mPendingLayers);
        pollRetired(scene, state);
    }

    void GpuScene::Gl::fenceRetired()
    {
        if (mPendingHandles.empty() && mPendingRetired.empty() && mPendingNames.empty() && mPendingLayers.empty())
            return;
        GLsync fence = mExt->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence == nullptr)
            return;
        FenceBatch batch;
        batch.mFence = fence;
        batch.mHandles.swap(mPendingHandles);
        batch.mMaterials.swap(mPendingRetired);
        batch.mNames.swap(mPendingNames);
        batch.mLayers.swap(mPendingLayers);
        mRetired.push_back(std::move(batch));
    }

    void GpuScene::Gl::cleanup(GpuScene& scene, osg::State& state)
    {
        for (const auto& [key, entry] : mHandles)
            if (entry.mHandle != 0)
                mExt->glMakeTextureHandleNonResident(entry.mHandle);
        for (const auto& [key, entry] : mHandles)
            Textures::releaseTexture(*const_cast<osg::Texture*>(key.first.get()), &state);
        mHandles.clear();
        for (FenceBatch& batch : mRetired)
            if (batch.mFence != nullptr)
                mExt->glDeleteSync(batch.mFence);
        std::lock_guard<std::mutex> lock(scene.mMutex);
        for (const PendingRetire& retired : mPendingRetired)
            scene.mTextures->release(retired, &state);
        for (const FenceBatch& batch : mRetired)
            for (const PendingRetire& retired : batch.mMaterials)
                scene.mTextures->release(retired, &state);
        scene.mTextures->takeStaged(state.getContextID(), mPendingNames, mPendingLayers);
        for (const FenceBatch& batch : mRetired)
        {
            mPendingNames.insert(mPendingNames.end(), batch.mNames.begin(), batch.mNames.end());
            mPendingLayers.insert(mPendingLayers.end(), batch.mLayers.begin(), batch.mLayers.end());
        }
        scene.mTextures->deleteNames(mPendingNames);
        scene.mTextures->freeLayers(mPendingLayers);
        mPendingRetired.clear();
        mPendingHandles.clear();
        mRetired.clear();
        mMaterialHandles.clear();
    }

    bool GpuScene::Gl::resizeViews(std::uint32_t views)
    {
        if (views <= mViewCapacity)
            return true;
        const std::size_t capacity = std::max(views, 8u);
        if (capacity * mCommandCount > mStorageLimit / sizeof(GpuCommand)
            || capacity * mSlotCount > mStorageLimit / sizeof(std::uint32_t)
            || capacity * mBatchCount > mStorageLimit / sizeof(std::uint32_t))
        {
            Log(Debug::Error) << "GPU view buffers exceed storage limit";
            return false;
        }
        mViewCapacity = static_cast<std::uint32_t>(capacity);
        const auto reallocate = [&](GLuint& buffer, std::size_t& allocated, std::size_t bytes) {
            bytes = std::max<std::size_t>(bytes, 4);
            GLuint fresh = 0;
            mExt->glGenBuffers(1, &fresh);
            mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, fresh);
            mExt->glBufferData(GL_COPY_WRITE_BUFFER, bytes, nullptr, GL_DYNAMIC_COPY);
            if (buffer != 0)
            {
                // Earlier view groups keep their regions.
                mExt->glBindBuffer(GL_COPY_READ_BUFFER, buffer);
                mCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                    static_cast<GLsizeiptr>(std::min(allocated, bytes)));
                mExt->glBindBuffer(GL_COPY_READ_BUFFER, 0);
                mExt->glDeleteBuffers(1, &buffer);
            }
            buffer = fresh;
            allocated = bytes;
        };
        reallocate(mVisibleBuffer, mVisibleBytes,
            static_cast<std::size_t>(mViewCapacity) * mSlotCount * sizeof(std::uint32_t));
        reallocate(
            mCountBuffer, mCountBytes, static_cast<std::size_t>(mViewCapacity) * mBatchCount * sizeof(std::uint32_t));
        reallocate(mCommandBuffer, mCommandBytes,
            static_cast<std::size_t>(mViewCapacity) * mCommandCount * sizeof(GpuCommand));
        mExt->glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        // Only the static array records these buffers.
        if (mVao != 0)
        {
            mExt->glDeleteVertexArrays(1, &mVao);
            mVao = 0;
        }
        return true;
    }

    void GpuScene::Gl::zero(GLuint buffer)
    {
        const std::uint32_t zero = 0;
        mExt->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        mClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        mExt->glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

}
