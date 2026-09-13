#include "gpusceneinternal.hpp"

namespace MWRender
{
    GpuScene::GpuScene(Shader::ShaderManager& shaderManager, osg::Stats& stats, const Options& options)
        : mShaderManager(shaderManager)
        , mLightOcclusion(options.mLightOcclusion)
        , mGpuSkinningEnabled(options.mGpuSkinning)
        , mDistantPagingEnabled(options.mDistantPaging)
        , mStats(&stats)
    {
        mTerrainShadowOnce = options.mLowEndIndirectBatching;
        mTerrainShadowRange = options.mLowEndIndirectBatching;
        mShadowActiveRanges = options.mLowEndIndirectBatching;
        if (!SceneUtil::glExtensionsReady())
            return;
        osg::GLExtensions& ext = SceneUtil::getGLExtensions();
        mSupported = static_cast<int>(ext.glslLanguageVersion * 100) >= 430 && ext.isVAOSupported
            && ext.glDispatchCompute != nullptr && ext.glMultiDrawElementsIndirect != nullptr
            && ext.glBindBufferBase != nullptr && ext.glMemoryBarrier != nullptr && ext.glVertexAttribDivisor != nullptr
            && ext.glVertexAttribIPointer != nullptr;
        if (!mSupported)
        {
            Log(Debug::Warning) << "GPU-driven object paging unavailable: OpenGL 4.3 features missing";
            return;
        }
        // Realize-time limits refuse before nodes commit.
        const GLint storageBindings = SceneUtil::getMaxShaderStorageBindings();
        if (storageBindings != 0
            && (storageBindings <= sMaxStorageBinding || SceneUtil::getMaxVertexStorageBlocks() < sVertexStorageBlocks
                || SceneUtil::getFragmentStorageBlocks() < sFragmentStorageBlocks))
        {
            Log(Debug::Warning) << "GPU-driven object paging unavailable: too few storage buffer bindings";
            mSupported = false;
            return;
        }
        const bool bindless = ext.glGetTextureHandle != nullptr && ext.glGetTextureSamplerHandle != nullptr
            && ext.glMakeTextureHandleResident != nullptr && ext.glMakeTextureHandleNonResident != nullptr
            && osg::isGLExtensionSupported(ext.contextID, "GL_ARB_bindless_texture");
        // Instance handles need nonuniform sampler access.
        const bool shader5 = osg::isGLExtensionSupported(ext.contextID, "GL_NV_gpu_shader5");
        // AMD exposes nonuniformEXT for OpenGL bindless samplers.
        const bool qualifier = osg::isGLExtensionSupported(ext.contextID, "GL_EXT_nonuniform_qualifier");
        const char* forcedMode = std::getenv("OPENMW_GPU_MATERIAL_MODE");
        const bool forceArray = forcedMode != nullptr && std::string_view(forcedMode) == "array";
        const bool forceLegacy = forcedMode != nullptr && std::string_view(forcedMode) == "legacy";
        const GLint maxLayers = ext.isTexture2DArraySupported ? ext.maxLayerCount : 0;
        if (options.mPackedTextures && bindless && (shader5 || qualifier) && !forceArray && !forceLegacy)
            mMaterialMode = Shader::MaterialMode::Bindless;
        else if (options.mPackedTextures && !forceLegacy && maxLayers >= 2)
            mMaterialMode = Shader::MaterialMode::Array;
        else
        {
            mMaterialMode = Shader::MaterialMode::Legacy;
            Log(Debug::Info) << "GPU packed textures disabled or unavailable; using material draws.";
        }
        if (options.mPackedTextures && mMaterialMode != Shader::MaterialMode::Bindless && !forceArray && !forceLegacy)
            Log(Debug::Warning) << "GPU bindless requirements unavailable; using fallback.";
        const char* mode = "legacy";
        if (mMaterialMode == Shader::MaterialMode::Bindless)
            mode = shader5 ? "bindless-nv" : "bindless-ext";
        else if (mMaterialMode == Shader::MaterialMode::Array)
            mode = "array";
        Log(Debug::Info) << "GPU materials: mode=" << mode << " bindless=" << bindless << " shader5=" << shader5
                         << " nonuniformEXT=" << qualifier << " vertexBlocks=" << SceneUtil::getMaxVertexStorageBlocks()
                         << " arrayLayers=" << maxLayers;
        mRegularActiveRanges = options.mLowEndIndirectBatching && mMaterialMode != Shader::MaterialMode::Bindless;
        const int textureBudget = options.mStorageBudget ? Settings::terrain().mObjectPagingGpuTextureBudget.get() : 0;
        mTextures = std::make_unique<Textures>(textureBudget, maxLayers);
        mGl = std::make_unique<Gl>();
        mGl->mExt = &ext;
        if (mLightOcclusion)
            Log(Debug::Info) << "GPU receiver occlusion enabled";
        const char* trace = std::getenv("OPENMW_GPU_DRAW_TRACE");
        mGl->mTraceDraws = trace != nullptr && std::string_view(trace) == "1";
        if (mGl->mTraceDraws)
            Log(Debug::Warning) << "GPU draw tracing enabled; rendering will slow down.";
        mRaySupported = ext.glFenceSync != nullptr && ext.glClientWaitSync != nullptr && ext.glDeleteSync != nullptr
            && ext.glMapBufferRange != nullptr && ext.glUnmapBuffer != nullptr;
        mRayState = std::make_shared<RayState>();
        mNode = new GpuSceneNode(this);
        mNode->setName("GpuScene");
        // Nested bins avoid the reserved stage bins.
        mOrderedNode = new osg::Group;
        mOrderedNode->setName("GpuSceneOrdered");
        mOrderedNode->setCullingActive(false);
        mOrderedNode->getOrCreateStateSet()->setRenderBinDetails(RenderBin_Default, "RenderBin");
        mNode->addChild(mOrderedNode);
    }

    GpuScene::~GpuScene()
    {
        if (mRayState != nullptr)
        {
            std::lock_guard<std::mutex> stateLock(mRayState->mMutex);
            mRayState->mAlive = false;
        }
        if (mRetireOperation != nullptr)
            mRetireOperation->detach();
        std::lock_guard<std::mutex> lock(mMutex);
        for (const osg::observer_ptr<GpuCaptureCallback>& callback : mCaptureCallbacks)
        {
            // The unref thread may destroy callbacks concurrently.
            osg::ref_ptr<GpuCaptureCallback> held;
            if (callback.lock(held))
                held->detach();
        }
        restoreRigsLocked();
        for (GpuChunkNode* node : mChunkNodes)
            node->detach();
        for (Dispatcher& dispatcher : mDispatchers)
            static_cast<DispatchDrawable*>(dispatcher.mDrawable.get())->detach();
        if (mPrepassDrawable != nullptr)
            static_cast<DispatchDrawable*>(mPrepassDrawable.get())->detach();
        if (mNode != nullptr)
        {
            mNode->setScene(nullptr);
            while (mNode->getNumParents() > 0)
                mNode->getParent(0)->removeChild(mNode);
        }
    }

    osg::Group* GpuScene::getNode()
    {
        return mNode;
    }

    void GpuScene::setEnabled(bool enabled)
    {
        std::vector<RayRequest> failed;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!enabled)
            {
                restoreRigsLocked();
                failed.swap(mPendingRays);
                mRayInvalidate = false;
            }
            mEnabled = enabled;
            if (enabled)
            {
                for (const auto& [actor, batches] : mActorBatches)
                    for (const auto& batch : batches)
                        for (const ActorPart& part : batch->mParts)
                            part.mDrawable->setNodeMask(Mask_GpuProxy);
                for (const RigEntry& entry : mRigRegistry)
                {
                    if (entry.mRig->getNodeMask() == ~0u)
                        entry.mRig->setNodeMask(Mask_GpuProxy);
                    if (entry.mRig->getNodeMask() == Mask_GpuProxy)
                        entry.mRig->setCpuSkinning(false);
                }
            }
            updateRigCullLocked();
        }
        if (enabled && mPrepassDrawable == nullptr && mNode != nullptr)
        {
            // The prepass exists only for enabled scenes.
            osg::ref_ptr<DispatchDrawable> prepass = new DispatchDrawable(*this, sPrepass);
            osg::ref_ptr<osg::StateSet> prepassState = new osg::StateSet;
            prepassState->setRenderBinDetails(-1000, "RenderBin");
            prepassState->setAttribute(new osg::ColorMask(false, false, false, false),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            prepassState->setAttribute(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
            prepass->setStateSet(prepassState);
            mPrepassDrawable = prepass;
            mNode->addChild(prepass);
        }
        if (!failed.empty())
        {
            RayBatch batch;
            batch.mRequests = std::move(failed);
            finishRay(mRayState, batch, nullptr, false);
        }
    }

    void GpuScene::setLightManager(SceneUtil::LightManager* lightManager)
    {
        mLightManager = lightManager;
    }

    void GpuScene::setDefaultMaterial(const SceneUtil::Material* material)
    {
        mDefaultMaterial = material;
    }

    void GpuScene::setWorkQueue(SceneUtil::WorkQueue* queue)
    {
        if (mTextures == nullptr)
            return;
        std::lock_guard<std::mutex> lock(mMutex);
        mTextures->mWorkQueue = queue;
    }

    void GpuScene::capture(osg::Node& node)
    {
        capture(node, nullptr);
    }

    void GpuScene::captureActor(osg::Node& node, SceneUtil::PositionAttitudeTransform& actor)
    {
        capture(node, &actor);
    }

    void GpuScene::capture(osg::Node& node, SceneUtil::PositionAttitudeTransform* actor)
    {
        if (!isEnabled())
            return;
        SceneUtil::LightListCallback* lightList = nullptr;
        // The actor's light list carries torch ignores.
        for (osg::Callback* callback = node.getCullCallback(); callback != nullptr;
             callback = callback->getNestedCallback())
            if ((lightList = dynamic_cast<SceneUtil::LightListCallback*>(callback)) != nullptr)
                break;
        CaptureVisitor visitor(mNode, actor != nullptr);
        // The sky keeps per-view transforms and state.
        visitor.setTraversalMask(~(Mask_Sky | (actor != nullptr ? Mask_UpdateVisitor : 0u)));
        node.accept(visitor);
        std::lock_guard<std::mutex> lock(mMutex);
        if (mCaptureCallbacks.size() >= mCaptureWatermark)
        {
            std::erase_if(mCaptureCallbacks,
                [](const osg::observer_ptr<GpuCaptureCallback>& callback) { return !callback.valid(); });
            mCaptureWatermark = std::max<std::size_t>(mCaptureCallbacks.size() * 2, 1024);
        }
        std::vector<Id> retired;
        if (actor != nullptr)
            removeActorLocked(*actor, &retired);
        ActorRigPopulation* actorRigs = nullptr;
        if (actor != nullptr && mGpuSkinningEnabled)
        {
            actorRigs = &mActorRigPopulations[actor];
            actorRigs->mTotal = visitor.mRigs.size();
        }
        for (osg::Geometry* geometry : visitor.mGeometries)
        {
            if (geometry->getDrawCallback() != nullptr)
                continue;
            if (auto* existing = dynamic_cast<GpuCaptureCallback*>(geometry->getCullCallback()))
            {
                existing->resetPath();
                continue;
            }
            osg::ref_ptr<GpuCaptureCallback> callback = new GpuCaptureCallback(
                *this, geometry->getCullCallback(), geometry->getDataVariance() == osg::Object::DYNAMIC);
            geometry->setCullCallback(callback);
            mCaptureCallbacks.push_back(callback);
        }
        for (const CaptureVisitor::CapturedRig& captured : visitor.mRigs)
        {
            if (!mGpuSkinningEnabled)
                continue;
            SceneUtil::RigGeometry* rig = captured.mRig;
            if (orderDependentPath(captured.mStates))
                continue;
            osg::Geometry* geometry = rig->getGpuGeometry();
            const osg::ref_ptr<osg::Geometry> source = rig->getSourceGeometry();
            if (geometry == nullptr || source == nullptr)
                continue;
            const Id mesh = getMeshLocked(*geometry, source.get());
            if (mesh == sInvalid || !registerSkinLocked(mesh, *rig))
                continue;
            rig->setGpuSkinning(true);
            if (actorRigs != nullptr)
                ++actorRigs->mGpu;
            if (actor != nullptr && mSkinSupported.load(std::memory_order_relaxed) && !captured.mUpdater
                && rig->getNodeMask() == ~0u)
            {
                const Surface surface = getSurfaceLocked(captured.mStates, 0, MaterialUpdate::None);
                if (surface.mBucket != sInvalid)
                {
                    ++mMeshes[mesh].mUsers;
                    addMaterialLocked(surface.mMaterial);
                    mRigRegistry.push_back(RigEntry{ rig, actor, mesh, surface, lightList, &node });
                    if (isEnabled())
                    {
                        rig->setNodeMask(Mask_GpuProxy);
                        rig->setCpuSkinning(false);
                    }
                    continue;
                }
            }
            if (auto* existing = dynamic_cast<GpuCaptureCallback*>(rig->getOuterCull()))
            {
                existing->resetPath();
                continue;
            }
            osg::ref_ptr<GpuCaptureCallback> callback = new GpuCaptureCallback(*this, rig->getOuterCull(), false);
            rig->setOuterCull(callback);
            mCaptureCallbacks.push_back(callback);
        }
        for (SceneUtil::MorphGeometry* morph : visitor.mMorphs)
        {
            if (auto* existing = dynamic_cast<GpuCaptureCallback*>(morph->getInnerCull()))
            {
                existing->resetPath();
                continue;
            }
            osg::ref_ptr<GpuCaptureCallback> callback = new GpuCaptureCallback(*this, nullptr, true);
            morph->setInnerCull(callback);
            mCaptureCallbacks.push_back(callback);
        }
        if (actor != nullptr && mSkinSupported.load(std::memory_order_relaxed)
            && std::any_of(
                mRigRegistry.begin(), mRigRegistry.end(), [&](const RigEntry& entry) { return entry.mActor == actor; }))
        {
            std::vector<Surface> surfaces;
            bool full = false;
            for (const CaptureVisitor::Part& captured : visitor.mParts)
            {
                const Surface surface = orderDependentPath(captured.mStates)
                    ? Surface{}
                    : getSurfaceLocked(captured.mStates, 0, MaterialUpdate::None);
                surfaces.push_back(surface);
                if (mMaterialMode == Shader::MaterialMode::Bindless && surface.mBucket != sInvalid)
                    for (std::size_t i = 1; i < Shader::sObjectMapCount; ++i)
                        full = full || mMaterials[surface.mMaterial].mTextures[i] != nullptr;
            }
            std::map<Id, std::pair<SceneUtil::GeometryBatch, std::shared_ptr<ActorBatch>>> batches;
            for (std::size_t i = 0; i < visitor.mParts.size(); ++i)
            {
                const CaptureVisitor::Part& captured = visitor.mParts[i];
                const Surface surface = surfaces[i];
                if (surface.mBucket == sInvalid)
                    continue;
                ActorPart part;
                part.mDrawable = captured.mDrawable;
                part.mRig = dynamic_cast<SceneUtil::RigGeometry*>(captured.mDrawable);
                if (mMaterialMode == Shader::MaterialMode::Array && part.mRig == nullptr)
                    continue;
                part.mMorph = dynamic_cast<SceneUtil::MorphGeometry*>(captured.mDrawable);
                if (part.mRig != nullptr)
                {
                    const auto found = std::find_if(mRigRegistry.begin(), mRigRegistry.end(),
                        [&](const RigEntry& entry) { return entry.mActor == actor && entry.mRig == part.mRig; });
                    if (found == mRigRegistry.end())
                        continue;
                    part.mGeometry = part.mRig->getSourceGeometry();
                    part.mMesh = found->mMesh;
                }
                else
                {
                    part.mGeometry = part.mMorph != nullptr ? part.mMorph->getSourceGeometry().get()
                                                            : captured.mDrawable->asGeometry();
                    if (part.mGeometry == nullptr
                        || (part.mMorph == nullptr && part.mGeometry->getDataVariance() == osg::Object::DYNAMIC))
                        continue;
                }
                const Id bucket = getActorBucket(surface.mBucket, full);
                if (bucket == sInvalid)
                    continue;
                const auto* winding = dynamic_cast<const osg::FrontFace*>(
                    mBuckets[surface.mBucket].mStateSet->getAttribute(osg::StateAttribute::FRONTFACE));
                auto& [geometry, batch] = batches[bucket];
                if (batch == nullptr)
                {
                    batch = std::make_shared<ActorBatch>();
                    batch->mBucket = bucket;
                }
                part.mVertex = geometry.mGeometry->getVertexArray()->getNumElements();
                if (!geometry.add(*part.mGeometry, surface.mMaterial,
                        winding != nullptr && winding->getMode() == osg::FrontFace::CLOCKWISE))
                    continue;
                part.mMaterial = surface.mMaterial;
                if (part.mRig != nullptr)
                {
                    part.mPalette = batch->mMatrixCount;
                    part.mMatrixCount = mSkinMeshes.at(part.mMesh).mBoneCount + 1;
                    batch->mMatrixCount += part.mMatrixCount;
                }
                if (part.mRig == nullptr)
                    for (const auto& attachment : captured.mTransforms)
                    {
                        const auto found = std::find_if(batch->mTransforms.begin(), batch->mTransforms.end(),
                            [&](const ActorBatch::Transform& transform) {
                                return transform.mNode == attachment && transform.mParent == part.mTransform;
                            });
                        if (found == batch->mTransforms.end())
                        {
                            batch->mTransforms.push_back({ attachment, part.mTransform });
                            part.mTransform = static_cast<Id>(batch->mTransforms.size() - 1);
                        }
                        else
                            part.mTransform = static_cast<Id>(found - batch->mTransforms.begin());
                    }
                if (part.mRig == nullptr && part.mMorph == nullptr && part.mDrawable->getNumParents() > 1)
                {
                    osg::ref_ptr<osg::Geometry> local = new osg::Geometry(*part.mGeometry, osg::CopyOp::SHALLOW_COPY);
                    captured.mParent->replaceChild(part.mDrawable, local);
                    part.mDrawable = local;
                }
                batch->mParts.push_back(std::move(part));
            }
            for (auto& [bucket, assembled] : batches)
            {
                auto& [geometry, batch] = assembled;
                if (batch->mParts.size() < 2)
                    continue;
                const std::uint32_t rigid = batch->mMatrixCount;
                batch->mMatrixCount += static_cast<std::uint32_t>(batch->mTransforms.size() + 1);
                if (batch->mMatrixCount > 0x1000000u)
                    continue;
                auto& skin = *static_cast<osg::Vec2uiArray*>(geometry.mGeometry->getVertexAttribArray(8));
                for (ActorPart& part : batch->mParts)
                {
                    if (part.mRig == nullptr)
                        part.mPalette = rigid
                            + (part.mTransform != sInvalid ? part.mTransform
                                                           : static_cast<Id>(batch->mTransforms.size()));
                    unsigned int flags = part.mGeometry->getNormalArray() != nullptr ? 1u : 0u;
                    if (dynamic_cast<const osg::Vec4Array*>(part.mGeometry->getTexCoordArray(7)) != nullptr)
                        flags |= 2u;
                    part.mPalette |= flags << 24;
                    const auto count = part.mGeometry->getVertexArray()->getNumElements();
                    for (unsigned int vertex = 0; vertex < count; ++vertex)
                    {
                        Id group = sInvalid;
                        if (part.mRig != nullptr)
                        {
                            group = part.mRig->getSkinGroups()[vertex];
                            group = group != sInvalid ? group + mSkinMeshes.at(part.mMesh).mGroupFirst : sInvalid - 1;
                        }
                        skin[part.mVertex + vertex] = { part.mPalette, group };
                    }
                }
                batch->mMesh = getMeshLocked(*geometry.mGeometry, geometry.mGeometry);
                if (batch->mMesh == sInvalid)
                    continue;
                batch->mGeometry = geometry.mGeometry;
                ++mMeshes[batch->mMesh].mUsers;
                for (const ActorPart& part : batch->mParts)
                {
                    if (part.mMesh != sInvalid)
                        ++mMeshes[part.mMesh].mUsers;
                    addMaterialLocked(part.mMaterial);
                    part.mDrawable->setNodeMask(Mask_GpuProxy);
                    if (part.mRig != nullptr)
                        for (RigEntry& entry : mRigRegistry)
                            if (entry.mRig == part.mRig && entry.mActor == actor)
                                entry.mBatched = true;
                }
                mActorBatches[actor].push_back(std::move(batch));
            }
        }
        for (const Id mesh : retired)
            releaseMeshLocked(mesh);
        updateRigCullLocked();
    }
}
