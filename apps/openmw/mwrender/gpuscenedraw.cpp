#include "gpusceneinternal.hpp"

namespace MWRender
{
    void GpuScene::draw(osg::RenderInfo& renderInfo, Id dispatcherId)
    {
        osg::State& state = *renderInfo.getState();
        if (state.getFrameStamp() == nullptr || !isEnabled() || !checkLimits())
            return;
        const unsigned int frame = state.getFrameStamp()->getFrameNumber();
        const osg::Camera* camera = renderInfo.getCurrentCamera();
        const bool prepass = dispatcherId == sPrepass;
        Gl& gl = *mGl;
        struct EndQuery
        {
            osg::GLExtensions* mExt;
            ~EndQuery()
            {
                if (mExt != nullptr)
                    mExt->glEndQuery(GL_PRIMITIVES_GENERATED);
            }
        } query{ gl.beginStats(frame, *mStats) ? gl.mExt : nullptr };
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTextures->resolve(state);
        }
        gl.advance(*this, state);
        mTextureHandles.store(gl.mHandles.size(), std::memory_order_relaxed);

        std::vector<GpuView> views;
        std::array<ViewRecord*, sMaxGroupViews> dispatched;
        std::size_t dispatchedCount = 0;
        std::uint32_t viewBase = 0;
        std::uint32_t viewIndex = 0;
        std::vector<std::uint32_t> chunkData;
        PendingWork work;
        std::uint32_t instanceExtent = 0;
        std::uint32_t receiverBase = 0;
        bool needPrepare = false;
        std::vector<DynamicInstance> dynamic;
        std::vector<SkinUpdate> skins;
        std::vector<ReceiverSlot> receivers;
        osg::Matrixf lightProjection;
        osg::ref_ptr<osg::ShaderStorageBufferBinding> lightReceivers;
        const bool classicLighting = mLightManager != nullptr && mLightManager->getClassicLighting();
        std::unique_ptr<RayBatch> rayBatch;
        std::vector<RayRequest> invalidated;
        struct DrawRange
        {
            Id mBucket;
            osg::ref_ptr<osg::StateSet> mStateSet;
            osg::ref_ptr<osg::StateSet> mPrepassState;
            osg::ref_ptr<const osg::Program> mProgram;
            Shader::MaterialMode mMode;
            std::uint32_t mOffset;
            std::uint32_t mCount;
            bool mDelta;
        };
        std::vector<DrawRange> ranges;
        std::vector<osg::StateAttribute::GLMode> deltaModes;
        std::vector<osg::StateAttribute::TypeMemberPair> deltaAttributes;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const auto found = mViews.find(ViewKey(frame, camera));
            if (prepass && camera != mPrepassCamera)
                return;
            if (found == mViews.end() || !found->second.mHasParams || (!prepass && dispatcherId >= mDispatchers.size()))
                return;
            ViewRecord& record = found->second;
            if (prepass)
            {
                lightProjection = record.mLightProjection;
                lightReceivers = record.mLightReceivers;
            }
            if (prepass && !mPendingRays.empty())
            {
                std::vector<RayRequest> requests;
                requests.swap(mPendingRays);
                if (mRayInvalidate)
                {
                    mRayInvalidate = false;
                    invalidated = std::move(requests);
                }
                else
                    rayBatch = makeRayBatchLocked(std::move(requests));
            }
            if (!record.mDispatched)
            {
                // Cull compatible frame views together.
                if (mDispatchFrame != frame)
                {
                    mDispatchFrame = frame;
                    mDispatchedViews = 0;
                    mStaticRanges.clear();
                }
                viewBase = mDispatchedViews;
                chunkData.assign(mChunks.size() * 2, 0);
                const auto add = [&](ViewRecord& pending) {
                    pending.mDispatched = true;
                    pending.mViewIndex = viewBase + static_cast<std::uint32_t>(views.size());
                    const bool activeRanges = pending.mShadow ? mShadowActiveRanges : mRegularActiveRanges;
                    if (activeRanges)
                        dispatched[dispatchedCount++] = &pending;
                    const std::uint32_t bit = 1u << views.size();
                    for (const auto& [slot, generation] : pending.mChunks)
                    {
                        if (slot >= mChunks.size() || mChunks[slot].mGeneration != generation)
                            continue;
                        const std::size_t offset = static_cast<std::size_t>(slot) * 2;
                        chunkData[offset] = generation;
                        chunkData[offset + 1] |= bit;
                    }
                    views.push_back(makeView(pending));
                };
                add(record);
                for (auto& [key, pending] : mViews)
                    if (key.first == frame && !pending.mDispatched && pending.mHasParams
                        && pending.mOcclusion == record.mOcclusion && views.size() < sMaxGroupViews)
                        add(pending);
                mDispatchedViews += static_cast<std::uint32_t>(views.size());
                takePendingWork(work);
                instanceExtent = mInstances.mExtent;
                for (std::size_t i = 0; i < dispatchedCount; ++i)
                {
                    ViewRecord* pending = dispatched[i];
                    pending->mStaticRangeOffset = static_cast<Id>(mStaticRanges.size());
                    pending->mStaticRangeCount = static_cast<Id>(mBuckets.size());
                    mStaticRanges.resize(mStaticRanges.size() + mBuckets.size(), { sInvalid, 0 });
                    for (const auto& [slot, generation] : pending->mChunks)
                    {
                        if (slot >= mChunks.size() || mChunks[slot].mGeneration != generation)
                            continue;
                        for (const auto& entry : mChunks[slot].mBatchCounts)
                        {
                            const Batch& batch = mBatches[entry.first];
                            if (batch.mCommand == sInvalid)
                                continue;
                            auto& range = mStaticRanges[pending->mStaticRangeOffset + batch.mBucket];
                            range.first = std::min(range.first, batch.mCommand);
                            range.second = std::max(range.second, batch.mCommand + 1);
                        }
                    }
                }
            }
            viewIndex = record.mViewIndex;
            if (!record.mPrepared)
            {
                record.mPrepared = true;
                needPrepare = true;
                dynamic = std::move(record.mDynamic);
                skins = std::move(record.mSkins);
                receivers = std::move(record.mReceivers);
                receiverBase = std::max(mReceiverSlots.mExtent, 1u);
            }
            const bool trimStatic = record.mShadow ? mShadowActiveRanges : mRegularActiveRanges;
            if (prepass)
            {
                for (Id bucketId = 0; bucketId < mBuckets.size(); ++bucketId)
                {
                    const Bucket& bucket = mBuckets[bucketId];
                    const osg::StateSet& bucketState = *bucket.mStateSet;
                    if (!prepassBucket(bucketState) || bucket.mCommandCount == 0)
                        continue;
                    osg::ref_ptr<osg::StateSet>& prepassState = gl.mPrepassStates[bucket.mStateSet.get()];
                    if (prepassState == nullptr)
                    {
                        osg::ref_ptr<osg::StateSet> created = makePrepassState(bucketState);
                        const auto same
                            = std::find_if(gl.mPrepassStates.begin(), gl.mPrepassStates.end(), [&](const auto& item) {
                                  return item.second != nullptr && item.second->compare(*created, true) == 0;
                              });
                        prepassState = same != gl.mPrepassStates.end() ? same->second : created;
                    }
                    if (!ranges.empty() && ranges.back().mPrepassState == prepassState
                        && ranges.back().mMode == bucket.mMode
                        && ranges.back().mOffset + ranges.back().mCount == bucket.mCommandOffset)
                        ranges.back().mCount += bucket.mCommandCount;
                    else
                        ranges.push_back(DrawRange{ bucketId, bucket.mStateSet, prepassState, bucket.mProgram,
                            bucket.mMode, bucket.mCommandOffset, bucket.mCommandCount, bucket.mDelta });
                }
            }
            else
            {
                const bool skipTerrainExtras = mTerrainShadowRange && record.mShadow;
                for (const Id bucketId : mDispatchers[dispatcherId].mBuckets)
                {
                    const Bucket& bucket = mBuckets[bucketId];
                    if (skipTerrainExtras && bucket.mTerrainExtraOnly)
                        continue;
                    std::uint32_t offset = bucket.mCommandOffset;
                    std::uint32_t count = bucket.mCommandCount;
                    if (trimStatic && bucketId < record.mStaticRangeCount)
                    {
                        const auto& range = mStaticRanges[record.mStaticRangeOffset + bucketId];
                        offset = range.first == sInvalid ? bucket.mCommandOffset : range.first;
                        count = range.first == sInvalid ? 0 : range.second - range.first;
                    }
                    if (count == 0)
                    {
                        const bool dynamicEmpty = needPrepare
                            ? dynamic.empty()
                            : bucketId >= gl.mDynamicRanges.size() || gl.mDynamicRanges[bucketId].second == 0;
                        if (dynamicEmpty)
                            continue;
                    }
                    ranges.push_back(DrawRange{ bucketId, bucket.mStateSet, nullptr, bucket.mProgram, bucket.mMode,
                        offset, count, bucket.mDelta });
                }
            }
            deltaModes = mDeltaModes;
            deltaAttributes = mDeltaAttributes;
        }

        if (!views.empty())
            upload(state, work);
        if (!invalidated.empty())
        {
            RayBatch failed;
            failed.mRequests = std::move(invalidated);
            finishRay(mRayState, failed, nullptr, false);
        }
        if (rayBatch != nullptr)
        {
            bool valid = rayBatch->mValid && mRaySupported.load(std::memory_order_relaxed)
                && mSkinSupported.load(std::memory_order_relaxed);
            if (valid && rayBatch->mTasks.empty())
                finishRay(mRayState, *rayBatch, nullptr, true);
            else if (!valid || state.getGraphicsContext() == nullptr
                || !runRay(state, *state.getGraphicsContext(), rayBatch))
                finishRay(mRayState, *rayBatch, nullptr, false);
        }
        if (needPrepare)
        {
            if (!prepareDynamic(state, dynamic, skins, receiverBase, receivers)
                || (classicLighting && !prepareReceivers(viewIndex, receivers)))
            {
                mSupported = false;
                mDemoted.store(true, std::memory_order_relaxed);
                return;
            }
        }
        CullPass pass = CullPass::Full;
        osg::ref_ptr<const osg::Texture> depthSource;
        const unsigned int frameId = frame % 2;
        if (prepass && mOcclusionEnabled && !views.empty() && mPostProcessor != nullptr)
        {
            depthSource = mPostProcessor->getPrimaryFbo(frameId)
                              ->getAttachment(osg::Camera::PACKED_DEPTH_STENCIL_BUFFER)
                              .getTexture();
            if (depthSource != nullptr
                && (depthSource->getTextureTarget() == GL_TEXTURE_2D
                    || depthSource->getTextureTarget() == GL_TEXTURE_2D_MULTISAMPLE))
            {
                pass = CullPass::Seed;
            }
            else
                depthSource = nullptr;
        }
        if (!views.empty() && !dispatch(state, views, viewBase, instanceExtent, chunkData, pass, false))
        {
            Log(Debug::Error) << "GPU scene dispatch failed";
            mSupported = false;
        }
        if (!mSupported)
            return;
        if (prepass && pass != CullPass::Seed)
            return;
        const std::size_t commandBase = static_cast<std::size_t>(viewIndex) * gl.mCommandCount;

        osg::GLExtensions& ext = *gl.mExt;
        ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, gl.mMaterialBuffer);
        if (prepass)
        {
            const auto resolve = [&](bool ready) {
                if (!dispatch(state, views, viewBase, instanceExtent, chunkData, CullPass::Resolve, ready))
                {
                    Log(Debug::Error) << "GPU scene dispatch failed";
                    mSupported = false;
                }
            };
            const osg::ref_ptr<osg::Program> base = mShaderManager.getProgram("gpuscene_prepass", {}, nullptr, {},
                Shader::ProgramCapability::Instanced | Shader::ProgramCapability::ArrayMaterial);
            if (base == nullptr)
            {
                resolve(false);
                return;
            }
            const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            for (const DrawRange& range : ranges)
            {
                osg::ref_ptr<osg::Program>& program = gl.mPrepassPrograms[range.mMode];
                if (program == nullptr)
                    program = mShaderManager.getInstancedProgram(*base, range.mMode);
                if (program == nullptr)
                    continue;
                osg::ref_ptr<osg::StateSet>& programState = gl.mPrepassStateSets[range.mMode];
                if (programState == nullptr)
                {
                    programState = new osg::StateSet;
                    programState->setAttributeAndModes(program,
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                    programState->setAttribute(
                        mPrepassDrawable->getStateSet()->getAttribute(osg::StateAttribute::POLYGONMODE),
                        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                }
                state.pushStateSet(programState);
                state.pushStateSet(range.mPrepassState);
                state.apply();
                if (range.mCount != 0 && gl.mVao != 0)
                {
                    ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
                    state.bindVertexArrayObject(gl.mVao);
                    gl.draw(
                        state, "prepass", range.mMode, gl.mCommandBuffer, commandBase + range.mOffset, range.mCount);
                }
                state.popStateSet();
                state.popStateSet();
            }
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
            state.bindVertexArrayObject(previousVao);
            ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            state.apply();
            const bool ready = depthSource != nullptr && buildPyramid(state, *depthSource);
            resolve(ready);
            if (ready && lightReceivers)
                cullLights(state, viewIndex, lightProjection, *lightReceivers);
            return;
        }
        if (classicLighting)
        {
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, gl.mReceiverBuffers.at(viewIndex).mBuffer);
        }
        // Stack values applied before the first range.
        std::vector<bool> baseModes(deltaModes.size());
        std::vector<bool> lockedModes(deltaModes.size());
        std::vector<const osg::StateAttribute*> baseAttributes(deltaAttributes.size());
        std::vector<bool> lockedAttributes(deltaAttributes.size());
        for (std::size_t i = 0; i < deltaModes.size(); ++i)
        {
            baseModes[i] = state.getLastAppliedMode(deltaModes[i]);
            const auto found = state.getModeMap().find(deltaModes[i]);
            lockedModes[i] = found != state.getModeMap().end() && !found->second.valueVec.empty()
                && (found->second.valueVec.back() & osg::StateAttribute::OVERRIDE) != 0;
        }
        for (std::size_t i = 0; i < deltaAttributes.size(); ++i)
        {
            baseAttributes[i] = state.getLastAppliedAttribute(deltaAttributes[i].first, deltaAttributes[i].second);
            const auto found = state.getAttributeMap().find(deltaAttributes[i]);
            lockedAttributes[i] = found != state.getAttributeMap().end() && !found->second.attributeVec.empty()
                && (found->second.attributeVec.back().second & osg::StateAttribute::OVERRIDE) != 0;
        }
        const auto castingBase = [&](const osg::StateSet& bucket, bool& useAlpha, bool& alphaBlend) {
            const SceneUtil::ShadowsBin::CastingState casting = SceneUtil::ShadowsBin::castingState(bucket);
            alphaBlend = casting.mAlphaBlend;
            useAlpha = casting.mUseAlpha;
            return mCastingProgramList[casting.mFunction - GL_NEVER].get();
        };
        bool first = true;
        bool override = false;
        bool shadowPass = false;
        const osg::Program* currentBase = nullptr;
        Shader::MaterialMode currentMode = Shader::MaterialMode::Legacy;
        bool currentUseAlpha = false;
        bool currentAlphaBlend = false;
        osg::ref_ptr<const osg::Program> overrideProgram;
        for (const DrawRange& range : ranges)
        {
            const std::pair<std::uint32_t, std::uint32_t> dynamicRange = range.mBucket < gl.mDynamicRanges.size()
                ? gl.mDynamicRanges[range.mBucket]
                : std::pair<std::uint32_t, std::uint32_t>();
            if (range.mCount == 0 && dynamicRange.second == 0)
                continue;

            bool canDraw = true;
            osg::ref_ptr<osg::StateSet> casting;
            bool delta = range.mDelta && !first;
            if (delta && shadowPass)
            {
                bool useAlpha = false;
                bool alphaBlend = false;
                const osg::Program* base = castingBase(*range.mStateSet, useAlpha, alphaBlend);
                delta = base == currentBase && range.mMode == currentMode && useAlpha == currentUseAlpha
                    && alphaBlend == currentAlphaBlend;
            }
            if (delta)
            {
                const osg::StateSet& stateSet = *range.mStateSet;
                for (std::size_t i = 0; i < deltaModes.size(); ++i)
                {
                    const auto found = stateSet.getModeList().find(deltaModes[i]);
                    state.applyMode(deltaModes[i],
                        found != stateSet.getModeList().end() && !lockedModes[i]
                            ? (found->second & osg::StateAttribute::ON) != 0
                            : baseModes[i]);
                }
                for (std::size_t i = 0; i < deltaAttributes.size(); ++i)
                {
                    const auto found = stateSet.getAttributeList().find(deltaAttributes[i]);
                    const osg::StateAttribute* attribute
                        = found != stateSet.getAttributeList().end() && !lockedAttributes[i] ? found->second.first.get()
                                                                                             : baseAttributes[i];
                    if (attribute == nullptr)
                        attribute
                            = state.getGlobalDefaultAttribute(deltaAttributes[i].first, deltaAttributes[i].second);
                    if (attribute != nullptr)
                        state.applyAttribute(attribute);
                }
                const osg::Program* program = override ? overrideProgram.get() : range.mProgram.get();
                canDraw = program != nullptr;
                if (canDraw && state.applyAttribute(program))
                {
                    const osg::Program::PerContextProgram* pcp = state.getLastAppliedProgramObject();
                    canDraw = pcp != nullptr;
                    for (const auto& [name, stack] : state.getUniformMap())
                        if (canDraw && !stack.uniformVec.empty())
                            pcp->apply(*stack.uniformVec.back().first);
                }
            }
            else
            {
                state.pushStateSet(range.mStateSet);
                state.apply();
                const osg::Program::PerContextProgram* appliedPcp = state.getLastAppliedProgramObject();
                const osg::Program* applied = appliedPcp != nullptr ? appliedPcp->getProgram() : nullptr;
                canDraw = applied != nullptr;
                override = applied != nullptr && applied != range.mProgram;
                overrideProgram = nullptr;
                if (override)
                {
                    shadowPass = std::find(mCastingProgramList.begin(), mCastingProgramList.end(), applied)
                        != mCastingProgramList.end();
                    bool useAlpha = false;
                    bool alphaBlend = false;
                    const osg::Program* base
                        = shadowPass ? castingBase(*range.mStateSet, useAlpha, alphaBlend) : applied;
                    currentBase = base;
                    currentMode = range.mMode;
                    currentUseAlpha = useAlpha;
                    currentAlphaBlend = alphaBlend;
                    const auto cacheKey = std::make_pair(base, range.mMode);
                    const auto cached = gl.mCastingPrograms.find(cacheKey);
                    osg::ref_ptr<osg::Program> instanced = cached != gl.mCastingPrograms.end()
                        ? cached->second
                        : gl.mCastingPrograms.emplace(cacheKey, mShaderManager.getInstancedProgram(*base, range.mMode))
                              .first->second;
                    if (instanced == nullptr)
                        canDraw = false;
                    else if (instanced != applied)
                    {
                        osg::ref_ptr<osg::StateSet>& cachedCasting
                            = gl.mCastingStateSets[{ base, useAlpha, alphaBlend, range.mMode }];
                        if (cachedCasting == nullptr)
                        {
                            cachedCasting = new osg::StateSet;
                            cachedCasting->setAttributeAndModes(instanced,
                                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
                                    | osg::StateAttribute::PROTECTED);
                            if (shadowPass)
                            {
                                cachedCasting->addUniform(new osg::Uniform("useDiffuseMapForShadowAlpha", useAlpha),
                                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                                cachedCasting->addUniform(new osg::Uniform("alphaTestShadows", alphaBlend),
                                    osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                            }
                        }
                        casting = cachedCasting;
                        state.pushStateSet(casting);
                        state.apply();
                    }
                    if (canDraw)
                        overrideProgram = instanced;
                }
                first = false;
            }

            const GLuint previousVao = state.getCurrentVertexArrayState() != nullptr
                ? state.getCurrentVertexArrayState()->getVertexArrayObject()
                : 0;
            if (canDraw && range.mCount != 0 && gl.mVao != 0)
            {
                ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
                state.bindVertexArrayObject(gl.mVao);
                gl.draw(state, shadowPass ? "shadow" : "main", range.mMode, gl.mCommandBuffer,
                    commandBase + range.mOffset, range.mCount);
            }
            if (canDraw && dynamicRange.second != 0 && gl.mDynamicVao != 0)
            {
                ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mDynamicInstanceBuffer);
                state.bindVertexArrayObject(gl.mDynamicVao);
                gl.draw(state, shadowPass ? "shadow-dynamic" : "main-dynamic", range.mMode, gl.mDynamicCommandBuffer,
                    dynamicRange.first, dynamicRange.second);
            }
            ext.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, gl.mInstanceBuffer);
            state.bindVertexArrayObject(previousVao);

            if (casting != nullptr)
                state.popStateSet();
            if (!delta)
                state.popStateSet();
        }
        ext.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        if (!first)
            state.apply();
    }
}
