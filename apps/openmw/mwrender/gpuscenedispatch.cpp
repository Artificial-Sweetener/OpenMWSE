#include "gpusceneinternal.hpp"

namespace MWRender
{
    bool GpuScene::prepareDynamic(osg::State& state, std::vector<DynamicInstance>& dynamic,
        const std::vector<SkinUpdate>& skins, std::uint32_t receiverBase, std::vector<ReceiverSlot>& receivers)
    {
        Gl& gl = *mGl;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mUpdateFrame != frame)
            {
                mUpdateFrame = frame;
                mUpdatedMeshes.clear();
            }
        }
        if (mSkinSupported.load(std::memory_order_relaxed) && !applySkin(state, skins))
        {
            Log(Debug::Error) << "GPU skin shader failed";
            mSkinSupported.store(false, std::memory_order_relaxed);
            mRaySupported.store(false, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            gl.mDynamicRanges.assign(mBuckets.size(), { 0, 0 });
        }
        if (dynamic.empty())
            return true;
        if (dynamic.size() > sInvalid - receiverBase)
        {
            Log(Debug::Warning) << "GPU light slots exceed index limits";
            return false;
        }

        std::vector<std::array<Id, 3>> order;
        order.reserve(dynamic.size());
        for (Id i = 0; i < dynamic.size(); ++i)
            order.push_back({ dynamic[i].mBucket, dynamic[i].mMesh, i });
        std::sort(order.begin(), order.end());

        struct MeshUpload
        {
            osg::ref_ptr<const osg::Geometry> mGeometry;
            std::uint32_t mVertexOffset;
            std::uint32_t mVertexCount;
            std::uint32_t mIndexCount;
        };
        std::vector<MeshUpload> meshUploads;
        std::vector<GpuInstance> instances;
        std::vector<GpuCommand> commands;
        instances.reserve(dynamic.size());

        std::size_t first = 0;
        while (first < order.size())
        {
            const DynamicInstance& head = dynamic[order[first][2]];
            std::size_t last = first + 1;
            while (last < order.size() && order[last][0] == head.mBucket && order[last][1] == head.mMesh)
                ++last;

            Mesh mesh;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (head.mMesh >= mMeshes.size() || !mMeshes[head.mMesh].mGeometry.valid()
                    || mMeshes[head.mMesh].mFailed)
                {
                    first = last;
                    continue;
                }
                mesh = mMeshes[head.mMesh];
            }

            const std::uint32_t baseInstance = static_cast<std::uint32_t>(instances.size());
            for (std::size_t i = first; i < last; ++i)
            {
                const DynamicInstance& instance = dynamic[order[i][2]];
                GpuInstance record{};
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col)
                        record.mRows[row][col] = instance.mWorld(col, row);
                record.mMaterial[1] = receiverBase + static_cast<std::uint32_t>(instances.size());
                record.mMaterial[0] = instance.mMaterial;
                instances.push_back(record);
                if (instance.mReceiver != 0)
                    receivers.push_back({ record.mMaterial[1], instance.mReceiver });
                if (instance.mUpdateMesh)
                {
                    // Skip uploads for unchanged vertex data.
                    const osg::Array* vertices = instance.mGeometry->getVertexArray();
                    const unsigned int version = vertices != nullptr ? vertices->getModifiedCount() : 0;
                    std::lock_guard<std::mutex> versionLock(mMutex);
                    if (mUpdatedMeshes.insert(instance.mGeometry.get()).second)
                    {
                        const auto [it, inserted] = mMeshVersions.try_emplace(instance.mGeometry.get(), version);
                        if (inserted || it->second != version)
                        {
                            it->second = version;
                            meshUploads.push_back(MeshUpload{ instance.mGeometry,
                                static_cast<std::uint32_t>(mesh.mBaseVertex), mesh.mVertexCount, mesh.mIndexCount });
                        }
                    }
                }
            }

            const std::uint32_t bucket = head.mBucket;
            if (bucket < gl.mDynamicRanges.size())
            {
                if (gl.mDynamicRanges[bucket].second == 0)
                    gl.mDynamicRanges[bucket].first = static_cast<std::uint32_t>(commands.size());
                ++gl.mDynamicRanges[bucket].second;
            }
            const GpuCommand command{ mesh.mIndexCount, static_cast<std::uint32_t>(instances.size()) - baseInstance,
                mesh.mFirstIndex, mesh.mBaseVertex, baseInstance };
            commands.push_back(command);
            first = last;
        }

        if (instances.empty())
            return true;
        if (commands.size() > gl.mStorageLimit / sizeof(GpuCommand)
            || instances.size() > gl.mStorageLimit / sizeof(GpuInstance))
        {
            Log(Debug::Error) << "GPU dynamic buffers exceed storage limit";
            return false;
        }
        std::vector<std::uint32_t> visible(instances.size());
        std::iota(visible.begin(), visible.end(), 0);

        gl.stream(gl.mDynamicInstanceBuffer, instances.data(), instances.size() * sizeof(GpuInstance));
        gl.stream(gl.mDynamicVisibleBuffer, visible.data(), visible.size() * sizeof(std::uint32_t));
        gl.stream(gl.mDynamicCommandBuffer, commands.data(), commands.size() * sizeof(GpuCommand));

        for (const MeshUpload& pending : meshUploads)
        {
            std::vector<std::uint8_t> vertices;
            std::vector<std::uint32_t> indices;
            if (!packGeometry(*pending.mGeometry, vertices, indices)
                || vertices.size() != pending.mVertexCount * sizeof(GpuVertex) || indices.size() != pending.mIndexCount)
                continue;
            gl.upload(gl.mVertexBuffer, pending.mVertexOffset * sizeof(GpuVertex), vertices.data(), vertices.size());
        }
        gl.createVao(state, true);
        return true;
    }

    bool GpuScene::prepareReceivers(std::uint32_t view, const std::vector<ReceiverSlot>& receivers)
    {
        Gl& gl = *mGl;
        // Unmapped slots receive only sunlight.
        std::size_t slots = 1;
        for (const ReceiverSlot& receiver : receivers)
            slots = std::max(slots, static_cast<std::size_t>(receiver.mSlot) + 1);
        if (slots > gl.mStorageLimit / sizeof(std::uint32_t))
        {
            Log(Debug::Warning) << "GPU light slots exceed storage limits";
            return false;
        }
        std::vector<std::uint32_t> indices(slots, 0);
        for (const ReceiverSlot& receiver : receivers)
            indices[receiver.mSlot] = receiver.mReceiver;
        if (gl.mReceiverBuffers.size() <= view)
            gl.mReceiverBuffers.resize(view + 1);
        auto& buffer = gl.mReceiverBuffers[view];
        gl.stream(buffer.mBuffer, indices.data(), indices.size() * sizeof(std::uint32_t));
        buffer.mSlots = static_cast<std::uint32_t>(slots);
        return true;
    }

    void GpuScene::cullLights(osg::State& state, std::uint32_t view, const osg::Matrixf& projection,
        osg::ShaderStorageBufferBinding& receivers)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;
        if (gl.mLightCullProgram == nullptr)
        {
            gl.mLightCullProgram = new osg::Program;
            gl.mLightCullProgram->addShader(mShaderManager.getShader("gpuscene_lightcull.comp"));
        }
        state.applyAttribute(gl.mLightCullProgram);
        auto* pcp = gl.mLightCullProgram->getPCP(state);
        if (!pcp->isLinked() || !state.setActiveTextureUnit(30))
            return;
        glBindTexture(GL_TEXTURE_2D, gl.mPyramid);
        state.setActiveTextureUnit(0);
        state.haveAppliedTextureAttribute(30, osg::StateAttribute::TEXTURE);
        ext.glUniform1i(pcp->getUniformLocation("pyramidLevels"), gl.mPyramidLevels);
        ext.glUniformMatrix4fv(pcp->getUniformLocation("projection"), 1, GL_FALSE, projection.ptr());
        state.applyAttribute(&receivers);
        const auto& buffer = gl.mReceiverBuffers.at(view);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, buffer.mBuffer);
        constexpr std::uint32_t maxSlots = 65535 * 64;
        for (std::uint32_t first = 0; first < buffer.mSlots; first += maxSlots)
        {
            ext.glUniform1ui(pcp->getUniformLocation("slotBase"), first);
            ext.glDispatchCompute((std::min(buffer.mSlots - first, maxSlots) + 63) / 64, 1, 1);
        }
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    GpuScene::GpuView GpuScene::makeView(const ViewRecord& record) const
    {
        GpuView view{};
        for (std::size_t i = 0; i < record.mPlanes.size(); ++i)
            view.mPlanes[i] = record.mPlanes[i];
        view.mPixelSizeVector = record.mPixelSizeVector;
        view.mViewPoint = osg::Vec4f(record.mViewPoint, record.mLodScale);
        view.mParams.x() = record.mSmallFeaturePixelSize;
        view.mParams.y() = mGroundcoverDetailedDistance;
        view.mParams.z() = mGroundcoverViewDistance;
        view.mMeta[0] = static_cast<std::uint32_t>(record.mPlanes.size());
        view.mMeta[1] = record.mOcclusion ? 1u : 0u;
        view.mMeta[2] = mNightDayMode.load(std::memory_order_relaxed);
        view.mMeta[3] = mTerrainShadowOnce && record.mShadow ? 1u : 0u;
        for (int column = 0; column < 4; ++column)
        {
            view.mViewProj[column] = osg::Vec4f(record.mViewProj(column, 0), record.mViewProj(column, 1),
                record.mViewProj(column, 2), record.mViewProj(column, 3));
        }
        return view;
    }

    bool GpuScene::buildPyramid(osg::State& state, const osg::Texture& source)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;
        const bool multisample = source.getTextureTarget() == GL_TEXTURE_2D_MULTISAMPLE;
        if (source.getTextureTarget() != GL_TEXTURE_2D && !multisample)
            return false;
        osg::Texture::TextureObject* object = source.getTextureObject(state.getContextID());
        const int sourceWidth = source.getTextureWidth();
        const int sourceHeight = source.getTextureHeight();
        const int width = std::max(sourceWidth / 2, 1);
        const int height = std::max(sourceHeight / 2, 1);
        if (object == nullptr || sourceWidth <= 1 || sourceHeight <= 1 || ext.glTexStorage2D == nullptr
            || ext.glBindImageTexture == nullptr || ext.glMemoryBarrier == nullptr)
            return false;
        if (gl.mDownsampleProgram == nullptr)
        {
            gl.mDownsampleProgram = new osg::Program;
            gl.mDownsampleProgram->addShader(
                mShaderManager.getShader("gpuscene_downsample.comp", { { "multisample", multisample ? "1" : "0" } }));
            gl.mFirstPass = new osg::Uniform("firstPass", 0);
            gl.mSourceDepthUniform = new osg::Uniform("sourceDepth", 0);
        }
        if (gl.mPyramid == 0 || gl.mPyramidWidth != width || gl.mPyramidHeight != height)
        {
            if (!state.setActiveTextureUnit(30))
                return false;
            if (gl.mPyramid != 0)
                glDeleteTextures(1, &gl.mPyramid);
            glGenTextures(1, &gl.mPyramid);
            gl.mPyramidWidth = width;
            gl.mPyramidHeight = height;
            gl.mPyramidLevels = std::min(8, 1 + static_cast<int>(std::floor(std::log2(std::max(width, height)))));
            glBindTexture(GL_TEXTURE_2D, gl.mPyramid);
            ext.glTexStorage2D(GL_TEXTURE_2D, gl.mPyramidLevels, GL_R32F, width, height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            state.setActiveTextureUnit(0);
        }
        ext.glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        state.applyAttribute(gl.mDownsampleProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mDownsampleProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        if (!state.setActiveTextureUnit(29))
            return false;
        source.apply(state);
        if (gl.mSourceSamplesUniform == nullptr)
        {
            GLint samples = 1;
            if (multisample)
                glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_SAMPLES, &samples);
            gl.mSourceSamplesUniform = new osg::Uniform("sourceSamples", samples);
        }
        state.setActiveTextureUnit(0);
        pcp->apply(*gl.mSourceSamplesUniform);
        gl.mSourceDepthUniform->set(29);
        int levelWidth = gl.mPyramidWidth;
        int levelHeight = gl.mPyramidHeight;
        for (int level = 0; level < gl.mPyramidLevels; ++level)
        {
            gl.mFirstPass->set(level == 0 ? 1 : 0);
            pcp->apply(*gl.mFirstPass);
            pcp->apply(*gl.mSourceDepthUniform);
            if (level > 0)
                ext.glBindImageTexture(0, gl.mPyramid, level - 1, GL_FALSE, 0, GL_READ_ONLY_ARB, GL_R32F);
            ext.glBindImageTexture(1, gl.mPyramid, level, GL_FALSE, 0, GL_WRITE_ONLY_ARB, GL_R32F);
            ext.glDispatchCompute((levelWidth + 15) / 16, (levelHeight + 15) / 16, 1);
            ext.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            levelWidth = std::max(levelWidth / 2, 1);
            levelHeight = std::max(levelHeight / 2, 1);
        }
        ext.glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        state.haveAppliedTextureAttribute(29, osg::StateAttribute::TEXTURE);
        state.haveAppliedTextureAttribute(30, osg::StateAttribute::TEXTURE);
        return true;
    }

    bool GpuScene::dispatch(osg::State& state, const std::vector<GpuView>& views, std::uint32_t viewBase,
        std::uint32_t instanceExtent, const std::vector<std::uint32_t>& chunkData, CullPass pass, bool occlusionReady)
    {
        Gl& gl = *mGl;
        osg::GLExtensions& ext = *gl.mExt;

        if (gl.mCullProgram == nullptr)
        {
            gl.mCullProgram = new osg::Program;
            gl.mCullProgram->addShader(mShaderManager.getShader("gpuscene_cull.comp"));
            gl.mCommandProgram = new osg::Program;
            gl.mCommandProgram->addShader(mShaderManager.getShader("gpuscene_commands.comp"));
            gl.mInstanceCount = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "instanceCount");
            gl.mChunkCount = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "chunkCount");
            gl.mViewBase = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "viewBase");
            gl.mViewCount = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "viewCount");
            gl.mSlotCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "slotCount");
            gl.mBatchCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "batchCount");
            gl.mCommandCountUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "commandCount");
            gl.mOccluderPixelUniform = new osg::Uniform("occluderPixelSize", sOccluderPixelSize);
            gl.mCullPassUniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "cullPass");
            gl.mOcclusionReadyUniform = new osg::Uniform("occlusionReady", 0);
            gl.mPyramidLevelsUniform = new osg::Uniform("pyramidLevels", 0);
        }

        if (gl.mVertexBuffer == 0 || gl.mBatchCount == 0)
            return true;
        if (pass != CullPass::Resolve)
        {
            if (!gl.resizeViews(viewBase + static_cast<std::uint32_t>(views.size())))
                return false;
            gl.createVao(state);
            gl.stream(gl.mChunkBuffer, chunkData.data(), chunkData.size() * sizeof(std::uint32_t));
            gl.stream(gl.mViewBuffer, views.data(), views.size() * sizeof(GpuView));
            gl.zero(gl.mCountBuffer);
        }
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, gl.mBatchBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, gl.mChunkBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, gl.mCountBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, gl.mVisibleBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 13, gl.mCommandBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, gl.mViewBuffer);
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, gl.mOcclusionBuffer);

        ext.glMemoryBarrier(
            GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        state.applyAttribute(gl.mCullProgram.get());
        osg::Program::PerContextProgram* pcp = gl.mCullProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        gl.mInstanceCount->set(instanceExtent);
        gl.mChunkCount->set(static_cast<unsigned int>(chunkData.size() / 2));
        gl.mViewBase->set(viewBase);
        gl.mViewCount->set(static_cast<unsigned int>(views.size()));
        gl.mSlotCountUniform->set(gl.mSlotCount);
        gl.mBatchCountUniform->set(gl.mBatchCount);
        gl.mCommandCountUniform->set(gl.mCommandCount);
        gl.mCullPassUniform->set(static_cast<unsigned int>(pass));
        gl.mOcclusionReadyUniform->set(occlusionReady ? 1 : 0);
        for (const osg::Uniform* uniform : { gl.mInstanceCount.get(), gl.mChunkCount.get(), gl.mViewBase.get(),
                 gl.mViewCount.get(), gl.mSlotCountUniform.get(), gl.mBatchCountUniform.get(),
                 gl.mOccluderPixelUniform.get(), gl.mCullPassUniform.get(), gl.mOcclusionReadyUniform.get() })
            pcp->apply(*uniform);
        if (!state.setActiveTextureUnit(30))
            return false;
        glBindTexture(GL_TEXTURE_2D, gl.mPyramid);
        state.setActiveTextureUnit(0);
        state.haveAppliedTextureAttribute(30, osg::StateAttribute::TEXTURE);
        if (occlusionReady)
        {
            gl.mPyramidLevelsUniform->set(gl.mPyramidLevels);
            pcp->apply(*gl.mPyramidLevelsUniform);
        }
        if (instanceExtent != 0)
            ext.glDispatchCompute((instanceExtent + sWorkGroupSize - 1) / sWorkGroupSize, 1, 1);
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        state.applyAttribute(gl.mCommandProgram.get());
        pcp = gl.mCommandProgram->getPCP(state);
        if (pcp == nullptr || !pcp->isLinked())
            return false;
        for (const osg::Uniform* uniform : { gl.mViewBase.get(), gl.mViewCount.get(), gl.mSlotCountUniform.get(),
                 gl.mBatchCountUniform.get(), gl.mCommandCountUniform.get() })
            pcp->apply(*uniform);
        ext.glDispatchCompute(
            (gl.mBatchCount + sWorkGroupSize - 1) / sWorkGroupSize, static_cast<GLuint>(views.size()), 1);
        ext.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
            | GL_BUFFER_UPDATE_BARRIER_BIT);
        return true;
    }

    bool GpuScene::Gl::beginStats(unsigned int frame, osg::Stats& stats)
    {
        if (mStatsFrame != frame)
        {
            mStatsFrame = frame;
            for (DrawStats& pending : mDrawStats)
            {
                if (pending.mUsed == 0)
                    continue;
                GLint available = GL_FALSE;
                mExt->glGetQueryObjectiv(pending.mQueries[pending.mUsed - 1], GL_QUERY_RESULT_AVAILABLE, &available);
                if (!available)
                    continue;
                GLuint64 triangles = 0;
                for (std::size_t i = 0; i < pending.mUsed; ++i)
                {
                    GLuint64 count = 0;
                    mExt->glGetQueryObjectui64v(pending.mQueries[i], GL_QUERY_RESULT, &count);
                    triangles += count;
                }
                stats.setAttribute(pending.mFrame, "GPU triangles", static_cast<double>(triangles));
                pending.mUsed = 0;
            }
            DrawStats& current = mDrawStats[frame % mDrawStats.size()];
            mCollectStats = stats.collectStats("gpu_scene") && current.mUsed == 0;
            if (mCollectStats)
                current.mFrame = frame;
            if (stats.collectStats("gpu_scene"))
                stats.setAttribute(frame, "GPU scene active", 1);
        }
        if (!mCollectStats)
            return false;
        DrawStats& current = mDrawStats[frame % mDrawStats.size()];
        if (current.mUsed == current.mQueries.size())
        {
            GLuint query;
            mExt->glGenQueries(1, &query);
            current.mQueries.push_back(query);
        }
        mExt->glBeginQuery(GL_PRIMITIVES_GENERATED, current.mQueries[current.mUsed++]);
        return true;
    }

    void GpuScene::Gl::draw(
        osg::State& state, const char* pass, Shader::MaterialMode mode, GLuint buffer, std::size_t first, GLsizei count)
    {
        // GPU-scene buffers are raw GL objects, so invalidate any OSG-managed indirect binding before replacing it.
        state.unbindDrawIndirectBufferObject();
        mExt->glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffer);
        const osg::Program::PerContextProgram* pcp = state.getLastAppliedProgramObject();
        if (mTraceDraws)
        {
            GLint vao = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            Log(Debug::Info) << "GPU draw: context=" << state.getContextID()
                             << " frame=" << state.getFrameStamp()->getFrameNumber() << " pass=" << pass
                             << " material=" << static_cast<int>(mode)
                             << " program=" << (pcp != nullptr ? pcp->getHandle() : 0) << " vao=" << vao
                             << " indirect=" << buffer << " offset=" << first * sizeof(GpuCommand) << " count=" << count
                             << " stride=" << sizeof(GpuCommand);
            Log(Debug::Info) << "GPU buffers: vertex=" << mVertexBuffer << '/' << mVertexCapacity
                             << " index=" << mIndexBuffer << '/' << mIndexCapacity << " instance=" << mInstanceBuffer
                             << '/' << mInstanceCapacity << " dynamicInstance=" << mDynamicInstanceBuffer
                             << " material=" << mMaterialBuffer << '/' << mMaterialCapacity
                             << " commands=" << mCommandBuffer << '/' << mCommandBytes;
            if (pcp != nullptr)
            {
                const osg::Program* program = pcp->getProgram();
                for (unsigned int i = 0; i < program->getNumShaders(); ++i)
                    Log(Debug::Info) << "GPU program " << pcp->getHandle()
                                     << " shader=" << program->getShader(i)->getName();
            }
        }
        mExt->glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(first * sizeof(GpuCommand)), count, sizeof(GpuCommand));
        mUploadCount = 0;
        if (mTraceDraws)
            glFinish();
        if (mTraceDraws)
        {
            if (pcp != nullptr)
            {
                std::string log;
                pcp->getInfoLog(log);
                if (!log.empty())
                    Log(Debug::Warning) << "GPU program " << pcp->getHandle() << " driver log: " << log;
            }
            if (mGetResetStatus != nullptr)
            {
                const GLenum status = mGetResetStatus();
                if (status != GL_NO_ERROR)
                    Log(Debug::Error) << "GPU reset status=" << status;
            }
            if (mTraceDraws)
                Log(Debug::Info) << "GPU draw finished: program=" << (pcp != nullptr ? pcp->getHandle() : 0);
        }
    }
}
