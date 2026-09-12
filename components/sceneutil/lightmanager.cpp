#include "lightmanager.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/hash.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/util.hpp>
#include <components/shader/shadermanager.hpp>

#include "memorybarrier.hpp"

namespace
{
    void configurePosition(osg::Matrixf& mat, const osg::Vec4& pos)
    {
        mat(0, 0) = pos.x();
        mat(0, 1) = pos.y();
        mat(0, 2) = pos.z();
    }

    void configureAmbient(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(1, 0) = color.r();
        mat(1, 1) = color.g();
        mat(1, 2) = color.b();
    }

    void configureDiffuse(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(2, 0) = color.r();
        mat(2, 1) = color.g();
        mat(2, 2) = color.b();
    }

    void configureSpecular(osg::Matrixf& mat, const osg::Vec4& color)
    {
        mat(3, 0) = color.r();
        mat(3, 1) = color.g();
        mat(3, 2) = color.b();
        mat(3, 3) = color.a();
    }

    void configureAttenuation(osg::Matrixf& mat, float c, float l, float q, float r)
    {
        mat(0, 3) = c;
        mat(1, 3) = l;
        mat(2, 3) = q;
        mat(3, 3) = r;
    }
}

namespace SceneUtil
{
    static std::atomic<int> sLightId{ 0 };

    void configureStateSetSunOverride(const Light* light, osg::StateSet* stateset, int mode)
    {
        stateset->addUniform(new osg::Uniform("sun.position", light->getPosition()), mode);
        stateset->addUniform(new osg::Uniform("sun.diffuse", light->getDiffuse()), mode);
        stateset->addUniform(new osg::Uniform("sun.ambient", light->getAmbient()), mode);
        stateset->addUniform(new osg::Uniform("sun.specular", light->getSpecular()), mode);
    }

    void configureSunAmbientOverride(const osg::Vec4f& ambient, osg::StateSet* stateset)
    {
        stateset->getOrCreateUniform("sun.ambient", osg::Uniform::FLOAT_VEC4)->set(ambient);
    }

    LightManager* findLightManager(const osg::NodePath& path)
    {
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (LightManager* lightManager = dynamic_cast<LightManager*>(path[i]))
                return lightManager;
        }
        return nullptr;
    }

    // Set on a LightSource. Adds the light source to its light manager for the current frame.
    // This allows us to keep track of the current lights in the scene graph without tying creation & destruction to the
    // manager.
    class CollectLightCallback : public NodeCallback<CollectLightCallback>
    {
    public:
        CollectLightCallback()
            : mLightManager(nullptr)
        {
        }

        CollectLightCallback(const CollectLightCallback& copy, const osg::CopyOp& copyop)
            : NodeCallback<CollectLightCallback>(copy, copyop)
            , mLightManager(nullptr)
        {
        }

        META_Object(SceneUtil, CollectLightCallback)

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mLightManager)
            {
                mLightManager = findLightManager(nv->getNodePath());

                if (!mLightManager)
                    throw std::runtime_error("can't find parent LightManager");
            }

            mLightManager->addLight(
                static_cast<LightSource*>(node), osg::computeLocalToWorld(nv->getNodePath()), nv->getTraversalNumber());

            traverse(node, nv);
        }

    private:
        LightManager* mLightManager;
    };

    // Set on a LightManager. Clears the data from the previous frame.
    class LightManagerUpdateCallback : public SceneUtil::NodeCallback<LightManagerUpdateCallback>
    {
    public:
        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            LightManager* lightManager = static_cast<LightManager*>(node);
            lightManager->update(nv->getTraversalNumber());

            traverse(node, nv);
        }
    };

    LightManagerCullCallback::LightManagerCullCallback(const LightSettings& settings)
        : mGridSizeX(settings.mClusteredGridSize.x())
        , mGridSizeY(settings.mClusteredGridSize.y())
        , mGridSizeZ(settings.mClusteredGridSize.z())
        , mNumClusters(mGridSizeX * mGridSizeY * mGridSizeZ)
        , mWorkGroupSize(settings.mClusteredWorkGroupSize)
    {
    }

    void LightManagerCullCallback::operator()(LightManager* node, osgUtil::CullVisitor* cv)
    {
        if (!(cv->getTraversalMask() & node->getLightingMask()))
        {
            traverse(node, cv);
            return;
        }

        const size_t frame = cv->getTraversalNumber();
        const size_t frameId = frame % 2;

        if (cv->getCurrentCamera()->getName() == Constants::SceneCamera && cv->getViewport() != nullptr)
            node->setDistantPixelScale(
                static_cast<float>((*cv->getProjectionMatrix())(1, 1) * cv->getViewport()->height() * 0.5));

        auto& cache = mCache[cv->getCurrentCamera()];
        const bool classic = node->getClassicLighting();

        float clusterFar = node->getPointLightFadeEnd();

        if (clusterFar == 0.f)
        {
            clusterFar = Constants::CellSizeInUnits;
            if (node->getDistantLightsEnabled())
                clusterFar = std::max(clusterFar, node->getDistantLightRange());
        }

        bool rebuildCluster = false;

        if (node->getClusteredLighting() && !classic)
        {
            auto& clusterNode = cache.mClusterComputeNode[frameId];
            auto& cullNode = cache.mCullComputeNode[frameId];

            if (!clusterNode || !cullNode)
            {
                if (!node->getResourceSystem())
                    throw std::runtime_error("Resource system must be defined");

                auto& shaderManager = node->getResourceSystem()->getSceneManager()->getShaderManager();

                clusterNode = new osg::DispatchCompute;
                clusterNode->setCullingActive(false);
                clusterNode->getOrCreateStateSet()->addUniform(
                    new osg::Uniform("inverseProjectionMatrix", osg::Matrixf{}));
                clusterNode->getOrCreateStateSet()->setAttribute(
                    shaderManager.getProgram(
                        nullptr, shaderManager.getShader("core/lighting/cluster.comp", {}, osg::Shader::COMPUTE)),
                    osg::StateAttribute::ON);
                clusterNode->setDrawCallback(new MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));

                cullNode = new osg::DispatchCompute;
                cullNode->setCullingActive(false);
                cullNode->getOrCreateStateSet()->setAttribute(
                    shaderManager.getProgram(nullptr,
                        shaderManager.getShader("core/lighting/cull.comp",
                            {
                                { "workGroupSize", std::to_string(mWorkGroupSize) },
                                { "maxLightsPerCluster", std::to_string(mMaxLightsPerCluster) },
                            },
                            osg::Shader::COMPUTE)),
                    osg::StateAttribute::ON);

                cullNode->setComputeGroups(mNumClusters, 1, 1);
                cullNode->setDrawCallback(new MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
            }

            rebuildCluster = cache.updateCluster(frameId, clusterFar, *cv->getProjectionMatrix());

            if (rebuildCluster)
            {
                // When reverse-z is enabled ensure we pass in the "unreversed" projection matrix.
                // This just makes all the maths constant and easier to deal with in general.
                // We also work with a projection matrix with a far plane detached from the current camera.
                double fovy, aspectRatio, near, _;
                cache.mProjection[frameId].getPerspective(fovy, aspectRatio, near, _);

                const osg::Matrixd projection = osg::Matrixd::perspective(fovy, aspectRatio, near, clusterFar);

                clusterNode->setComputeGroups(mGridSizeX, mGridSizeY, mGridSizeZ);
                clusterNode->getStateSet()
                    ->getUniform("inverseProjectionMatrix")
                    ->set(osg::Matrixf::inverse(projection));
            }
        }

        if (classic && !cache.mCullComputeNode[frameId])
        {
            auto& shaderManager = node->getResourceSystem()->getSceneManager()->getShaderManager();
            auto& cullNode = cache.mCullComputeNode[frameId];
            cullNode = new osg::DispatchCompute;
            cullNode->setCullingActive(false);
            auto* computeState = cullNode->getOrCreateStateSet();
            computeState->setAttribute(shaderManager.getProgram(
                nullptr, shaderManager.getShader("core/lighting/classic.comp", {}, osg::Shader::COMPUTE)));
            computeState->addUniform(new osg::Uniform("lightCount", 0u));
            computeState->addUniform(new osg::Uniform("receiverCount", 0u));
            computeState->addUniform(new osg::Uniform("lightLimit", 0u));
            cullNode->setDrawCallback(new MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
        }

        auto& stateset = cache.mStateSet[frameId];

        if (!stateset)
        {
            stateset = new osg::StateSet;
            stateset->addUniform(new osg::Uniform("sun.position", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("sun.diffuse", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("sun.ambient", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("sun.specular", osg::Vec4f{}));
            stateset->addUniform(new osg::Uniform("lightCluster", 0u));

            if (node->getClusteredLighting())
            {
                stateset->addUniform(new osg::Uniform("clusterFar", clusterFar));
                stateset->addUniform(new osg::Uniform("gridSize",
                    osg::Vec3f(static_cast<float>(mGridSizeX), static_cast<float>(mGridSizeY),
                        static_cast<float>(mGridSizeZ))));

                const int maxLightIndices = mMaxLightsPerCluster * mNumClusters;

                osg::ref_ptr<osg::UByteArray> clusterData = new osg::UByteArray(sizeof(Cluster) * mNumClusters);
                clusterData->setBufferObject(new osg::ShaderStorageBufferObject);
                cache.mClusterSSBB
                    = new osg::ShaderStorageBufferBinding(1, clusterData, 0, clusterData->getTotalDataSize());

                for (size_t i = 0; i < cache.mPointLightSSBB.size(); ++i)
                {
                    cache.mGPULights[i] = new osg::BufferTemplate<std::vector<PointLight>>();
                    cache.mGPULights[i]->setBufferObject(new osg::ShaderStorageBufferObject);

                    cache.mPointLightSSBB[i] = new osg::ShaderStorageBufferBinding(
                        2, cache.mGPULights[i], 0, cache.mGPULights[i]->getTotalDataSize());

                    osg::ref_ptr<osg::UIntArray> gridData = new osg::UIntArray(mNumClusters * 2);
                    gridData->setBufferObject(new osg::ShaderStorageBufferObject);
                    cache.mLightGridSSBB[i]
                        = new osg::ShaderStorageBufferBinding(3, gridData, 0, gridData->getTotalDataSize());

                    osg::ref_ptr<osg::UIntArray> indexData = new osg::UIntArray(maxLightIndices);
                    indexData->setBufferObject(new osg::ShaderStorageBufferObject);
                    cache.mLightIndexListSSBB[i]
                        = new osg::ShaderStorageBufferBinding(4, indexData, 0, indexData->getTotalDataSize());

                    osg::ref_ptr<osg::UIntArray> counterData = new osg::UIntArray(1);
                    counterData->setBufferObject(new osg::ShaderStorageBufferObject);
                    cache.mLightIndexCounterSSBB[i]
                        = new osg::ShaderStorageBufferBinding(5, counterData, 0, counterData->getTotalDataSize());

                    if (classic)
                    {
                        cache.mReceivers[i] = new osg::BufferTemplate<std::vector<LightReceiver>>;
                        cache.mReceivers[i]->setBufferObject(new osg::ShaderStorageBufferObject);
                        cache.mReceiverSSBB[i] = new osg::ShaderStorageBufferBinding(1, cache.mReceivers[i]);
                        cache.mLightBounds[i] = new osg::BufferTemplate<std::vector<LightReceiver>>;
                        cache.mLightBounds[i]->setBufferObject(new osg::ShaderStorageBufferObject);
                        cache.mLightBoundSSBB[i] = new osg::ShaderStorageBufferBinding(5, cache.mLightBounds[i]);
                        cache.mIgnoredLights[i] = new osg::UIntArray;
                        cache.mIgnoredLights[i]->setBufferObject(new osg::ShaderStorageBufferObject);
                        cache.mIgnoredSSBB[i] = new osg::ShaderStorageBufferBinding(6, cache.mIgnoredLights[i]);
                    }
                }
            }
        }

        const bool updateFrame = frame != cache.mLastFrameNumber;
        if (updateFrame)
        {
            cache.mLastFrameNumber = frame;

            const auto& sun = node->getSunlight();

            // Don't use Camera::getViewMatrix, that one might be relative to another camera!
            const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();

            stateset->getUniform("sun.position")->set(sun->getPosition() * (*viewMatrix));
            stateset->getUniform("sun.diffuse")->set(sun->getDiffuse());
            stateset->getUniform("sun.ambient")->set(sun->getAmbient());
            stateset->getUniform("sun.specular")->set(sun->getSpecular());

            if (node->getClusteredLighting())
            {
                if (rebuildCluster)
                {
                    stateset->getUniform("clusterFar")->set(clusterFar);
                    stateset->getUniform("gridSize")
                        ->set(osg::Vec3f(static_cast<float>(mGridSizeX), static_cast<float>(mGridSizeY),
                            static_cast<float>(mGridSizeZ)));
                }

                stateset->setAttribute(cache.mPointLightSSBB[frameId]);
                stateset->setAttribute(cache.mLightGridSSBB[frameId]);
                stateset->setAttribute(cache.mLightIndexListSSBB[frameId]);
                if (classic)
                {
                    cache.mReceivers[frameId]->getData().assign(1, LightReceiver{});
                    cache.mIgnoredLights[frameId]->clear();
                    auto* computeState = cache.mCullComputeNode[frameId]->getStateSet();
                    computeState->setAttribute(cache.mReceiverSSBB[frameId]);
                    computeState->setAttribute(cache.mLightBoundSSBB[frameId]);
                    computeState->setAttribute(cache.mIgnoredSSBB[frameId]);
                }
                else
                {
                    stateset->setAttribute(cache.mClusterSSBB);
                    stateset->setAttribute(cache.mLightIndexCounterSSBB[frameId]);
                }
            }
        }

        cv->pushStateSet(stateset);
        if (rebuildCluster)
            cache.mClusterComputeNode[frameId]->accept(*cv);
        if (node->getClusteredLighting())
            cache.mCullComputeNode[frameId]->accept(*cv);
        traverse(node, cv);
        cv->popStateSet();

        // Cull collects the current paged lights.
        if (updateFrame && node->getClusteredLighting())
        {
            const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();
            auto& lights = cache.mGPULights[frameId]->getData();
            lights.clear();
            if (classic)
                cache.mLightBounds[frameId]->getData().clear();
            for (const auto& bound : node->getLightsInViewSpace(cv, viewMatrix, frame))
            {
                if (!classic && bound.mCulled)
                    continue;

                const auto& light = bound.mLightSource->getLight(frame);
                lights.push_back(PointLight{
                    .mPosition = light->getPosition() * (*viewMatrix),
                    .mDiffuse = light->getDiffuse() * bound.mFade,
                    .mAmbient = light->getAmbient(),
                    .mSpecular = light->getSpecular() * bound.mFade,
                    .mConstant = light->getConstantAttenuation(),
                    .mLinear = light->getLinearAttenuation(),
                    .mQuadratic = light->getQuadraticAttenuation(),
                    .mRadius = bound.mViewBound.radius(),
                });
                if (classic)
                    cache.mLightBounds[frameId]->getData().push_back(
                        LightReceiver{ osg::Vec4f(bound.mViewBound.center(), bound.mViewBound.radius()),
                            { static_cast<std::uint32_t>(bound.mLightSource->getId()), 0, 0, 0 } });
            }

            if (classic)
            {
                const auto count = cache.mReceivers[frameId]->getData().size();
                const auto limit = static_cast<unsigned int>(node->getMaxLights());
                // OpenGL guarantees 128 MiB per block.
                constexpr std::size_t storageLimit = 128u << 20;
                if (count > storageLimit / (limit * sizeof(std::uint32_t)) || count > 65535u * 64u
                    || lights.size() > storageLimit / sizeof(PointLight)
                    || cache.mIgnoredLights[frameId]->size() > storageLimit / sizeof(std::uint32_t))
                    throw std::runtime_error("Classic light storage exceeds hardware limits");
                auto* computeState = cache.mCullComputeNode[frameId]->getStateSet();
                computeState->getUniform("lightCount")->set(static_cast<unsigned int>(lights.size()));
                computeState->getUniform("receiverCount")->set(static_cast<unsigned int>(count));
                computeState->getUniform("lightLimit")->set(limit);
                cache.mCullComputeNode[frameId]->setComputeGroups(static_cast<int>((count + 63) / 64), 1, 1);
                auto* grid = static_cast<osg::UIntArray*>(cache.mLightGridSSBB[frameId]->getBufferData());
                if (grid->size() != count * 2)
                {
                    grid->resize(count * 2);
                    grid->dirty();
                    cache.mLightGridSSBB[frameId]->setSize(grid->getTotalDataSize());
                }
                auto* indices = static_cast<osg::UIntArray*>(cache.mLightIndexListSSBB[frameId]->getBufferData());
                if (indices->size() != count * limit)
                {
                    indices->resize(count * limit);
                    indices->dirty();
                    cache.mLightIndexListSSBB[frameId]->setSize(indices->getTotalDataSize());
                }
                if (cache.mLightBounds[frameId]->getData().empty())
                    cache.mLightBounds[frameId]->getData().emplace_back();
                if (cache.mIgnoredLights[frameId]->empty())
                    cache.mIgnoredLights[frameId]->push_back(0);
                for (const auto& binding :
                    { cache.mReceiverSSBB[frameId], cache.mLightBoundSSBB[frameId], cache.mIgnoredSSBB[frameId] })
                {
                    binding->setSize(binding->getBufferData()->getTotalDataSize());
                    binding->getBufferData()->dirty();
                }
            }

            // SSBO bindings require nonzero storage.
            if (lights.empty())
                lights.emplace_back();

            cache.mPointLightSSBB[frameId]->setSize(cache.mGPULights[frameId]->getTotalDataSize());
            cache.mGPULights[frameId]->dirty();
            if (!classic)
            {
                static_cast<osg::UIntArray*>(cache.mLightIndexCounterSSBB[frameId]->getBufferData())->at(0) = 0;
                static_cast<osg::UIntArray*>(cache.mLightIndexCounterSSBB[frameId]->getBufferData())->dirty();
            }
        }

        if (node->getPPLightsBuffer() && cv->getCurrentCamera()->getName() == Constants::SceneCamera)
            node->getPPLightsBuffer()->updateCount(frame);
    }

    LightManager::LightManager(const LightSettings& settings, Resource::ResourceSystem* resourceSystem)
        : mResourceSystem(resourceSystem)
        , mLightingMask(~0u)
        , mSun(nullptr)
        , mClassicFalloff(settings.mClassicFalloff)
        , mIndexedLightLists(settings.mIndexedLightLists)
        , mPointLightRadiusMultiplier(1.f)
        , mPointLightFadeEnd(0.f)
        , mPointLightFadeStart(0.f)
    {
        osg::GLExtensions* exts = SceneUtil::glExtensionsReady() ? &SceneUtil::getGLExtensions() : nullptr;
        // In theory the two extensions should allow us to use SSBO and std430 layout in version 120
        //  1. GL_ARB_shader_storage_buffer_object
        //  2. GL_ARB_shading_language_420pack
        // However, this is not the case in practice and is known to be broken on at least Mesa drivers.
        bool supportsSSBO = exts && static_cast<int>(exts->glslLanguageVersion * 100) >= 430 && exts->glDispatchCompute
            && exts->glMemoryBarrier && exts->glBindBufferBase && getMaxShaderStorageBindings() >= 7
            && getMaxVertexStorageBlocks() >= 3 && getFragmentStorageBlocks() >= 3;

        mSupportsClustered = supportsSSBO;

        setUpdateCallback(new LightManagerUpdateCallback);
        setMaxLights(settings.mMaxLights);

        mCullCallback = new LightManagerCullCallback(settings);
        addCullCallback(mCullCallback);

        static bool hasLoggedWarnings = false;

        if (settings.mClusteredLighting && !hasLoggedWarnings)
        {
            if (!supportsSSBO)
                Log(Debug::Warning) << "Clustered lighting requirements unavailable; using standard lighting.";
            hasLoggedWarnings = true;
        }

        if (settings.mClusteredLighting && mSupportsClustered)
            enableClustered(true);
        else
            initPerObjectUniform(settings.mMaxLights);

        getOrCreateStateSet()->addUniform(new osg::Uniform("PointLightCount", 0));

        updateSettings(settings.mLightRadiusMultiplier, settings.mMaximumLightDistance, settings.mLightFadeStart,
            settings.mMinimumLightRadius);
    }

    LightManager::LightManager(const LightManager& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
        , mResourceSystem(copy.mResourceSystem)
        , mLightingMask(copy.mLightingMask)
        , mSun(copy.mSun)
        , mClusteredLighting(copy.mClusteredLighting)
        , mClassicFalloff(copy.mClassicFalloff)
        , mIndexedLightLists(copy.mIndexedLightLists)
        , mPointLightRadiusMultiplier(copy.mPointLightRadiusMultiplier)
        , mPointLightFadeEnd(copy.mPointLightFadeEnd)
        , mPointLightFadeStart(copy.mPointLightFadeStart)
        , mMinimumLightRadius(copy.mMinimumLightRadius)
        , mMaxLights(copy.mMaxLights)
        , mSupportsClustered(copy.mSupportsClustered)
        , mPPLightBuffer(copy.mPPLightBuffer)
    {
    }

    bool LightManager::getClusteredLighting() const
    {
        return mClusteredLighting;
    }

    void LightManager::setClassicFalloff(bool enabled)
    {
        if (mClassicFalloff == enabled)
            return;
        mClassicFalloff = enabled;
        mCullCallback->reset();
    }

    std::uint32_t LightManager::addReceiver(osgUtil::CullVisitor& cv, const osg::BoundingSphere& viewBound,
        const std::set<LightSource*>* ignored, std::uint32_t parent)
    {
        if (!getClassicLighting() || !(cv.getTraversalMask() & mLightingMask))
            return 0;
        auto& cache = mCullCallback->mCache[cv.getCurrentCamera()];
        const std::size_t frame = cv.getTraversalNumber() % 2;
        if (!cache.mReceivers[frame])
            return 0;
        auto& receivers = cache.mReceivers[frame]->getData();
        auto& ids = *cache.mIgnoredLights[frame];
        const auto index = static_cast<std::uint32_t>(receivers.size());
        receivers.push_back(LightReceiver{ osg::Vec4f(viewBound.center(), viewBound.radius()),
            { static_cast<std::uint32_t>(ids.size()), ignored ? static_cast<std::uint32_t>(ignored->size()) : 0, parent,
                0 } });
        if (ignored)
            for (const auto* source : *ignored)
                ids.push_back(static_cast<std::uint32_t>(source->getId()));
        return index;
    }

    int LightManager::getMaxLights() const
    {
        return mMaxLights;
    }

    void LightManager::setMaxLights(int value)
    {
        mMaxLights = std::clamp(value, 2, 64);
        for (StateSetPool& pool : mStateSetPools)
            pool = StateSetPool();
    }

    Shader::ShaderManager::DefineMap LightManager::getLightDefines() const
    {
        Shader::ShaderManager::DefineMap defines;

        defines["maxLights"] = std::to_string(getMaxLights());
        defines["lightingMethodClustered"] = mClusteredLighting ? "1" : "0";
        defines["clusteredClassic"] = getClassicLighting() ? "1" : "0";
        defines["legacyLightList"] = mClusteredLighting ? "0" : "1";
        defines["useGPUShader4"] = std::to_string(mClusteredLighting);
        defines["simpleLighting"] = "0";

        return defines;
    }

    void LightManager::processChangedSettings(
        float lightRadiusMultiplier, float maximumLightDistance, float lightFadeStart, float minimumLightRadius)
    {
        updateSettings(lightRadiusMultiplier, maximumLightDistance, lightFadeStart, minimumLightRadius);
    }

    void LightManager::updateMaxLights(int maxLights)
    {
        setMaxLights(maxLights);

        getStateSet()->removeUniform("LightBuffer");
        getStateSet()->addUniform(generateLightBufferUniform());
    }

    void LightManager::updateSettings(
        float lightRadiusMultiplier, float maximumLightDistance, float lightFadeStart, float minimumLightRadius)
    {
        mPointLightRadiusMultiplier = lightRadiusMultiplier;
        mMinimumLightRadius = minimumLightRadius;
        mPointLightFadeEnd = maximumLightDistance;
        if (mPointLightFadeEnd > 0)
            mPointLightFadeStart = mPointLightFadeEnd * lightFadeStart;
        mCullCallback->reset();
    }

    void LightManager::initPerObjectUniform(int targetLights)
    {
        mClusteredLighting = false;
        setMaxLights(targetLights);

        getOrCreateStateSet()->addUniform(generateLightBufferUniform());

        mCullCallback->reset();
    }

    void LightManager::enableClustered(bool enabled)
    {
        if (enabled)
        {
            mClusteredLighting = true;
            mCullCallback->reset();
        }
        else
            initPerObjectUniform(mMaxLights);
    }

    void LightManager::enableIndexedLightLists(bool enabled)
    {
        if (mIndexedLightLists == enabled)
            return;
        mIndexedLightLists = enabled;
        mBaselineLightListStateSets.clear();
        mListSlots.clear();
        mListIds.clear();
        mListTable.clear();
        for (StateSetPool& pool : mStateSetPools)
            pool = StateSetPool();
    }

    void LightManager::setLightingMask(size_t mask)
    {
        mLightingMask = mask;
    }

    size_t LightManager::getLightingMask() const
    {
        return mLightingMask;
    }

    LightManager::DistantLight* LightManager::findDistantSlot(LightSource& light)
    {
        if (light.mDistantSlot >= 0 && static_cast<size_t>(light.mDistantSlot) < mDistantLights.size()
            && mDistantLights[light.mDistantSlot].mLightSource == &light)
            return &mDistantLights[light.mDistantSlot];
        if (light.getPagedId() == 0)
            return nullptr;
        const auto found = mDistantIds.find(light.getPagedId());
        if (found == mDistantIds.end())
            return nullptr;
        DistantLight& distant = mDistantLights[found->second];
        distant.mLightSource->mDistantSlot = -1;
        distant.mLightSource = &light;
        light.mDistantSlot = static_cast<int>(found->second);
        return &distant;
    }

    void LightManager::update(size_t frameNum)
    {
        if (mPPLightBuffer)
            mPPLightBuffer->clear(frameNum);

        // Bridges chunk swaps.
        constexpr size_t grace = 4;
        for (std::size_t i = 0; i < mDistantLights.size();)
        {
            if (frameNum > mDistantLights[i].mLastSeen + grace)
            {
                LightSource& dropped = *mDistantLights[i].mLightSource;
                dropped.mDistantSlot = -1;
                if (dropped.getPagedId() != 0)
                    mDistantIds.erase(dropped.getPagedId());
                mDistantLights[i] = std::move(mDistantLights.back());
                mDistantLights.pop_back();
                if (i < mDistantLights.size())
                {
                    LightSource& moved = *mDistantLights[i].mLightSource;
                    moved.mDistantSlot = static_cast<int>(i);
                    if (moved.getPagedId() != 0)
                        mDistantIds[moved.getPagedId()] = i;
                }
            }
            else
                ++i;
        }
        mLights.clear();
        mActiveLightIds.clear();
        mLightsInViewSpace.clear();
        mBaselineLightListStateSets.clear();
        mListSlots.clear();
        mListIds.clear();
        std::fill(mListTable.begin(), mListTable.end(), 0u);
        mStateSetPools[frameNum % 2].mUsed = 0;
    }

    void LightManager::addLight(LightSource* lightSource, const osg::Matrixf& worldMat, size_t frameNum)
    {
        osg::BoundingSphere worldBound(osg::Vec3f(), lightSource->getRadius());
        transformBoundingSphere(worldMat, worldBound);
        osg::Vec3f pos = worldMat.getTrans();
        lightSource->getLight(frameNum)->setPosition(osg::Vec4f(pos, 1.f));

        mLights.push_back(LightSourceBound{ lightSource, worldBound, worldMat, false });
        if (mDistantLightsEnabled && lightSource->getPagedId() != 0)
            mActiveLightIds.push_back(lightSource->getPagedId());
    }

    void LightManager::addDistantBound(LightSource* lightSource, const osg::BoundingSphere& worldBound, size_t frameNum)
    {
        if (lightSource->mDistantFrame == frameNum)
            return;
        lightSource->mDistantFrame = frameNum;
        mPendingDistant.push_back(LightSourceBound{ lightSource, worldBound, osg::Matrixf(), true });
    }

    void LightManager::setSunlight(osg::ref_ptr<Light> sun)
    {
        mSun = sun;
    }

    osg::ref_ptr<Light> LightManager::getSunlight()
    {
        return mSun;
    }

    std::size_t LightManager::LightListHash::operator()(const LightListView& key) const
    {
        std::size_t result = std::hash<const osg::RefMatrix*>{}(key.mViewMatrix);
        for (const LightSourceViewBound* light : key.mLights)
            Misc::hashCombine(result, light->mLightSource->getId());
        return result;
    }

    osg::Matrixf LightManager::makeLightData(
        const LightSourceViewBound& bound, size_t frameNum, const osg::RefMatrix* viewMatrix) const
    {
        auto* light = bound.mLightSource->getLight(frameNum);
        osg::Matrixf data;
        configurePosition(data, light->getPosition() * (*viewMatrix));
        configureAmbient(data, light->getAmbient());
        configureDiffuse(data, light->getDiffuse());
        configureSpecular(data, light->getSpecular());
        configureAttenuation(data, light->getConstantAttenuation(), light->getLinearAttenuation(),
            light->getQuadraticAttenuation(), bound.mLightSource->getRadius() * mPointLightRadiusMultiplier);
        return data;
    }

    // References last until the next insert.
    osg::ref_ptr<osg::StateSet>& LightManager::getLightList(
        const LightList& lightList, const osg::RefMatrix* viewMatrix)
    {
        const LightListView view{ viewMatrix, lightList };
        const std::size_t hash = LightListHash{}(view);
        const auto matches = [&](const LightListSlot& slot) {
            if (slot.mHash != hash || slot.mViewMatrix != viewMatrix || slot.mIdsCount != lightList.size())
                return false;
            for (std::uint32_t i = 0; i < slot.mIdsCount; ++i)
                if (mListIds[slot.mIdsFirst + i] != lightList[i]->mLightSource->getId())
                    return false;
            return true;
        };
        if (!mListTable.empty())
        {
            const std::size_t mask = mListTable.size() - 1;
            for (std::size_t probe = hash & mask;; probe = (probe + 1) & mask)
            {
                const std::uint32_t index = mListTable[probe];
                if (index == 0)
                    break;
                if (matches(mListSlots[index - 1]))
                    return mListSlots[index - 1].mStateSet;
            }
        }
        if (mListTable.empty() || (mListSlots.size() + 1) * 10 >= mListTable.size() * 7)
        {
            mListTable.assign(std::max<std::size_t>(64, mListTable.size() * 2), 0u);
            const std::size_t mask = mListTable.size() - 1;
            for (std::size_t i = 0; i < mListSlots.size(); ++i)
            {
                std::size_t probe = mListSlots[i].mHash & mask;
                while (mListTable[probe] != 0)
                    probe = (probe + 1) & mask;
                mListTable[probe] = static_cast<std::uint32_t>(i + 1);
            }
        }
        LightListSlot slot;
        slot.mViewMatrix = viewMatrix;
        slot.mIdsFirst = static_cast<std::uint32_t>(mListIds.size());
        slot.mIdsCount = static_cast<std::uint32_t>(lightList.size());
        slot.mHash = hash;
        for (const LightSourceViewBound* light : lightList)
            mListIds.push_back(light->mLightSource->getId());
        mListSlots.push_back(std::move(slot));
        const std::size_t mask = mListTable.size() - 1;
        std::size_t probe = hash & mask;
        while (mListTable[probe] != 0)
            probe = (probe + 1) & mask;
        mListTable[probe] = static_cast<std::uint32_t>(mListSlots.size());
        return mListSlots.back().mStateSet;
    }

    osg::ref_ptr<osg::StateSet> LightManager::getLightListStateSet(
        const LightList& lightList, size_t frameNum, const osg::RefMatrix* viewMatrix)
    {
        if (!mIndexedLightLists)
        {
            std::vector<int> lightIds;
            lightIds.reserve(lightList.size());
            for (const LightSourceViewBound* light : lightList)
                lightIds.push_back(light->mLightSource->getId());

            auto [found, inserted]
                = mBaselineLightListStateSets.try_emplace(std::make_pair(viewMatrix, std::move(lightIds)));
            if (!inserted)
                return found->second;

            osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
            osg::ref_ptr<osg::Uniform> data = generateLightBufferUniform();
            for (std::size_t i = 0; i < lightList.size(); ++i)
                data->setElement(static_cast<unsigned int>(i), makeLightData(*lightList[i], frameNum, viewMatrix));
            stateSet->addUniform(data);
            stateSet->addUniform(new osg::Uniform("PointLightCount", static_cast<int>(lightList.size())));
            found->second = stateSet;
            return stateSet;
        }

        osg::ref_ptr<osg::StateSet>& stateSet = getLightList(lightList, viewMatrix);
        if (stateSet != nullptr)
            return stateSet;

        // Reuse state sets after two frames.
        StateSetPool& pool = mStateSetPools[frameNum % 2];
        if (pool.mUsed == pool.mStateSets.size())
        {
            PooledStateSet pooled;
            pooled.mStateSet = new osg::StateSet;
            pooled.mData = generateLightBufferUniform();
            pooled.mCount = new osg::Uniform("PointLightCount", 0);
            pooled.mStateSet->addUniform(pooled.mData);
            pooled.mStateSet->addUniform(pooled.mCount);
            pool.mStateSets.push_back(std::move(pooled));
        }
        const PooledStateSet& pooled = pool.mStateSets[pool.mUsed++];
        osg::Uniform& data = *pooled.mData;
        for (std::size_t i = 0; i < lightList.size(); ++i)
            data.setElement(static_cast<unsigned int>(i), makeLightData(*lightList[i], frameNum, viewMatrix));
        pooled.mCount->set(static_cast<int>(lightList.size()));
        stateSet = pooled.mStateSet;

        return stateSet;
    }

    namespace
    {
        std::array<int, 3> gridCell(const LightManager::LightGrid& grid, const osg::Vec3f& point)
        {
            std::array<int, 3> cell;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float value = std::floor((point[axis] - grid.mOrigin[axis]) / grid.mCellSize);
                cell[axis] = static_cast<int>(std::clamp(value, 0.f, static_cast<float>(grid.mSize[axis] - 1)));
            }
            return cell;
        }

        template <class F>
        void forEachCell(const LightManager::LightGrid& grid, const osg::BoundingSphere& bound, F&& f)
        {
            const osg::Vec3f radius(bound.radius(), bound.radius(), bound.radius());
            const std::array<int, 3> lo = gridCell(grid, bound.center() - radius);
            const std::array<int, 3> hi = gridCell(grid, bound.center() + radius);
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int x = lo[0]; x <= hi[0]; ++x)
                        f((static_cast<std::size_t>(z) * grid.mSize[1] + y) * grid.mSize[0] + x);
        }

        void buildLightGrid(
            LightManager::LightGrid& grid, const std::vector<LightManager::LightSourceViewBound>& bounds)
        {
            constexpr int maxCells = 32;
            grid.mBuilt = true;
            grid.mCellStart.clear();
            grid.mEntries.clear();
            grid.mMark.assign(bounds.size(), 0);
            grid.mStamp = 0;
            if (bounds.empty())
                return;
            osg::BoundingBox box;
            float cellSize = 1.f;
            for (const LightManager::LightSourceViewBound& light : bounds)
            {
                box.expandBy(light.mViewBound);
                cellSize = std::max(cellSize, light.mViewBound.radius());
            }
            for (int axis = 0; axis < 3; ++axis)
                cellSize = std::max(cellSize, (box._max[axis] - box._min[axis]) / maxCells);
            grid.mOrigin = box._min;
            grid.mCellSize = cellSize;
            std::size_t cells = 1;
            for (int axis = 0; axis < 3; ++axis)
            {
                grid.mSize[axis]
                    = std::clamp(static_cast<int>((box._max[axis] - box._min[axis]) / cellSize) + 1, 1, maxCells);
                cells *= grid.mSize[axis];
            }
            grid.mCellStart.assign(cells + 1, 0);
            for (const LightManager::LightSourceViewBound& light : bounds)
                forEachCell(grid, light.mViewBound, [&](std::size_t cell) { ++grid.mCellStart[cell + 1]; });
            for (std::size_t cell = 0; cell < cells; ++cell)
                grid.mCellStart[cell + 1] += grid.mCellStart[cell];
            grid.mEntries.resize(grid.mCellStart[cells]);
            std::vector<std::uint32_t> fill(grid.mCellStart.begin(), grid.mCellStart.end() - 1);
            for (std::uint32_t index = 0; index < bounds.size(); ++index)
                forEachCell(
                    grid, bounds[index].mViewBound, [&](std::size_t cell) { grid.mEntries[fill[cell]++] = index; });
        }
    }

    void LightManager::getLightsIntersecting(osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix,
        size_t frameNum, const osg::BoundingSphere& viewBound, LightList& out)
    {
        out.clear();
        if (!viewBound.valid())
            return;
        ViewLights& view = getViewLights(cv, viewMatrix, frameNum);
        const std::vector<LightSourceViewBound>& bounds = view.mBounds;
        if (!mIndexedLightLists)
        {
            for (const LightSourceViewBound& light : bounds)
                if (light.mViewBound.intersects(viewBound))
                    out.push_back(&light);
            return;
        }
        LightGrid& grid = view.mGrid;
        if (!grid.mBuilt)
            buildLightGrid(grid, bounds);
        if (bounds.empty())
            return;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float extent = grid.mSize[axis] * grid.mCellSize;
            if (viewBound.center()[axis] + viewBound.radius() < grid.mOrigin[axis]
                || viewBound.center()[axis] - viewBound.radius() > grid.mOrigin[axis] + extent)
                return;
        }
        if (++grid.mStamp == 0)
        {
            std::fill(grid.mMark.begin(), grid.mMark.end(), 0);
            grid.mStamp = 1;
        }
        thread_local std::vector<std::uint32_t> found;
        found.clear();
        forEachCell(grid, viewBound, [&](std::size_t cell) {
            for (std::uint32_t entry = grid.mCellStart[cell]; entry < grid.mCellStart[cell + 1]; ++entry)
            {
                const std::uint32_t index = grid.mEntries[entry];
                if (grid.mMark[index] == grid.mStamp)
                    continue;
                grid.mMark[index] = grid.mStamp;
                if (bounds[index].mViewBound.intersects(viewBound))
                    found.push_back(index);
            }
        });
        std::sort(found.begin(), found.end());
        for (const std::uint32_t index : found)
            out.push_back(&bounds[index]);
    }

    const std::vector<LightManager::LightSourceViewBound>& LightManager::getLightsInViewSpace(
        osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum)
    {
        return getViewLights(cv, viewMatrix, frameNum).mBounds;
    }

    LightManager::ViewLights& LightManager::getViewLights(
        osgUtil::CullVisitor* cv, const osg::RefMatrix* viewMatrix, size_t frameNum)
    {
        osg::Camera* camera = cv->getCurrentCamera();

        osg::observer_ptr<osg::Camera> camPtr(camera);
        auto it = mLightsInViewSpace.find(camPtr);

        if (it == mLightsInViewSpace.end())
        {
            for (const LightSourceBound& pending : mPendingDistant)
            {
                LightSource& light = *pending.mLightSource;
                if (DistantLight* distant = findDistantSlot(light))
                {
                    distant->mWorldBound = pending.mWorldBound;
                    distant->mLastSeen = frameNum;
                }
                else
                {
                    const std::size_t slot = mDistantLights.size();
                    light.mDistantSlot = static_cast<int>(slot);
                    if (light.getPagedId() != 0)
                        mDistantIds.emplace(light.getPagedId(), slot);
                    mDistantLights.push_back(DistantLight{ &light, pending.mWorldBound, frameNum });
                }
            }
            mPendingDistant.clear();
            if (mLightsInViewSpace.empty())
                std::sort(mActiveLightIds.begin(), mActiveLightIds.end());
            it = mLightsInViewSpace.insert(std::make_pair(camPtr, ViewLights())).first;
            std::vector<LightSourceViewBound>& lights = it->second.mBounds;
            const osg::Matrixf viewMatrixf(*viewMatrix);
            osg::BoundingSphere viewScaleBound(osg::Vec3f(), 1.f);
            transformBoundingSphere(viewMatrixf, viewScaleBound);
            const float viewScale = viewScaleBound.radius();
            float pixelScale = getDistantPixelScale();
            // Viewport-less culls use the seeded scale.
            if (cv->getViewport() != nullptr && cv->getProjectionMatrix() != nullptr)
                pixelScale = static_cast<float>((*cv->getProjectionMatrix())(1, 1) * cv->getViewport()->height() * 0.5);

            const auto addLight = [&](const LightSourceBound& light) {
                const float radius = light.mLightSource->getRadius() * mPointLightRadiusMultiplier;
                osg::BoundingSphere viewBound;
                if (light.mDistant)
                {
                    viewBound = light.mWorldBound;
                    viewBound.radius() *= mPointLightRadiusMultiplier;
                    viewBound.center() = viewBound.center() * viewMatrixf;
                    viewBound.radius() *= viewScale;
                }
                else
                {
                    // Classic bounds match upstream bit for bit.
                    const osg::Matrixf worldViewMat = light.mWorldMatrix * (*viewMatrix);
                    viewBound = osg::BoundingSphere(osg::Vec3f(), radius);
                    transformBoundingSphere(worldViewMat, viewBound);
                }

                float fade = 1.f;
                if (mClusteredLighting)
                {
                    const float distance = viewBound.center().length();
                    if (mPointLightFadeEnd > 0.f)
                    {
                        if (distance >= mPointLightFadeEnd)
                            return;
                        if (distance > mPointLightFadeStart)
                            fade = (mPointLightFadeEnd - distance) / (mPointLightFadeEnd - mPointLightFadeStart);
                    }
                    if (!getClassicLighting() && mMinimumLightRadius > 0.f && pixelScale > 0.f)
                    {
                        float pixels = viewBound.radius() * pixelScale;
                        if (!cv->getViewport() || (*cv->getProjectionMatrix())(3, 3) == 0.f)
                            pixels /= std::max(std::abs(viewBound.center().z()), 1.f);
                        if (pixels <= mMinimumLightRadius)
                            return;
                        const float sizeFade = std::min(pixels / mMinimumLightRadius - 1.f, 1.f);
                        fade *= sizeFade * sizeFade * (3.f - 2.f * sizeFade);
                    }
                }
                else if (light.mLightSource->getLastAppliedFrame() != frameNum && mPointLightFadeEnd != 0.f)
                {
                    const float fadeDelta = mPointLightFadeEnd - mPointLightFadeStart;
                    const float viewDelta = viewBound.center().length() - mPointLightFadeStart;
                    const float distanceFade = 1 - std::clamp(viewDelta / fadeDelta, 0.f, 1.f);
                    if (distanceFade == 0.f)
                        return;

                    auto* osgLight = light.mLightSource->getLight(frameNum);
                    osgLight->setDiffuse(osgLight->getDiffuse() * distanceFade);
                    osgLight->setSpecular(osgLight->getSpecular() * distanceFade);
                    light.mLightSource->setLastAppliedFrame(frameNum);
                }

                LightSourceViewBound l;
                l.mLightSource = light.mLightSource;
                l.mViewBound = viewBound;
                l.mFade = fade;
                lights.push_back(l);
            };
            for (const LightSourceBound& light : mLights)
                addLight(light);
            if (mDistantLightsEnabled)
            {
                for (const DistantLight& distant : mDistantLights)
                {
                    if (std::binary_search(
                            mActiveLightIds.begin(), mActiveLightIds.end(), distant.mLightSource->getPagedId()))
                        continue;
                    distant.mLightSource->getLight(frameNum)->setPosition(
                        osg::Vec4f(distant.mWorldBound.center(), 1.f));
                    addLight(LightSourceBound{ distant.mLightSource, distant.mWorldBound, osg::Matrixf(), true });
                }
            }

            const bool fillPPBuffer = mPPLightBuffer && it->first->getName() == Constants::SceneCamera;

            if ((mClusteredLighting && !getClassicLighting()) || fillPPBuffer)
            {
                auto sorter = [](const LightManager::LightSourceViewBound& left,
                                  const LightManager::LightSourceViewBound& right) {
                    return left.mViewBound.center().length2() - left.mViewBound.radius2()
                        < right.mViewBound.center().length2() - right.mViewBound.radius2();
                };

                std::sort(lights.begin(), lights.end(), sorter);

                osg::CullingSet& cullingSet = cv->getModelViewCullingStack().front();
                for (auto& bound : lights)
                {
                    const auto* light = bound.mLightSource->getLight(frameNum);
                    const float radius = bound.mLightSource->getRadius() * mPointLightRadiusMultiplier;
                    osg::BoundingSphere frustumBound = bound.mViewBound;
                    frustumBound.radius() = (mClusteredLighting ? bound.mViewBound.radius() : radius) * 2.f;

                    bound.mCulled = cullingSet.isCulled(frustumBound);

                    if (bound.mCulled || bound.mLightSource->getEmpty() || light->getDiffuse().x() < 0.f)
                        continue;

                    if (fillPPBuffer)
                        getPPLightsBuffer()->setLight(frameNum, light, radius, bound.mFade);
                }
            }
        }

        return it->second;
    }

    osg::ref_ptr<osg::Uniform> LightManager::generateLightBufferUniform()
    {
        osg::ref_ptr<osg::Uniform> uniform = new osg::Uniform(osg::Uniform::FLOAT_MAT4, "LightBuffer", getMaxLights());

        return uniform;
    }

    void LightManager::setCollectPPLights(bool enabled)
    {
        if (enabled)
            mPPLightBuffer = std::make_shared<PPLightBuffer>();
        else
            mPPLightBuffer = nullptr;
    }

    LightSource::LightSource()
        : mRadius(0.f)
        , mActorFade(1.f)
        , mLastAppliedFrame(0)
    {
        setUpdateCallback(new CollectLightCallback);
        mId = sLightId++;
    }

    void LightSource::setDistant(bool distant)
    {
        mDistant = distant;
        if (!distant)
            return;
        // Paging registers it without flicker.
        setUpdateCallback(nullptr);
        setInitialBound(osg::BoundingSphere(osg::Vec3f(), mRadius * 2.f));
    }

    LightSource::LightSource(const LightSource& copy, const osg::CopyOp& copyop)
        : osg::Node(copy, copyop)
        , mRadius(copy.mRadius)
        , mActorFade(copy.mActorFade)
        , mLastAppliedFrame(copy.mLastAppliedFrame)
        , mDistant(copy.mDistant)
        , mPagedId(copy.mPagedId)
    {
        mId = sLightId++;

        for (size_t i = 0; i < mLight.size(); ++i)
            mLight[i] = new Light(*copy.mLight[i].get(), copyop);
    }

    void LightListCallback::operator()(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mLightManager)
            mLightManager = findLightManager(cv->getNodePath());
        const bool classic = mLightManager && mLightManager->getClassicLighting();
        const auto parent = classic ? mLightManager->currentReceiver() : 0;
        bool pushedState = pushLightState(node, cv);
        if (classic && pushedState)
            mLightManager->setReceiver(mReceiver);
        traverse(node, cv);
        if (classic)
            mLightManager->setReceiver(parent);
        if (pushedState)
            cv->popStateSet();
    }

    bool LightListCallback::pushLightState(osg::Node* node, osgUtil::CullVisitor* cv)
    {
        if (!mLightManager)
        {
            mLightManager = findLightManager(cv->getNodePath());
            if (!mLightManager)
                return false;
        }

        if (!(cv->getTraversalMask() & mLightManager->getLightingMask()))
            return false;

        if (mLightManager->getClassicLighting())
        {
            osg::BoundingSphere bound;
            if (const auto* transform = node->asTransform())
                for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                    bound.expandBy(transform->getChild(i)->getBound());
            else
                bound = node->getBound();
            transformBoundingSphere(*cv->getModelViewMatrix(), bound);
            const auto parent = mLightManager->currentReceiver();
            const auto receiver = mLightManager->addReceiver(*cv, bound, &mIgnoredLightSources, parent);
            const auto frame = cv->getTraversalNumber();
            if (mReceiverFrame != frame)
            {
                mReceiverFrame = frame;
                mReceiverUsed = 0;
            }
            auto& states = mReceiverStates[frame % 2];
            if (mReceiverUsed == states.size())
            {
                osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
                state->addUniform(new osg::Uniform("lightCluster", 0u));
                states.push_back(state);
            }
            auto& state = states[mReceiverUsed++];
            state->getUniform("lightCluster")->set(receiver);
            cv->pushStateSet(state);
            mReceiver = receiver;
            return true;
        }

        if (mLightManager->getClusteredLighting())
            return false;

        // Possible optimizations:
        // - organize lights in a quad tree

        // Don't use Camera::getViewMatrix, that one might be relative to another camera!
        const osg::RefMatrix* viewMatrix = cv->getCurrentRenderStage()->getInitialViewMatrix();

        // Update light list if necessary
        // This makes sure we don't update it more than once per frame when rendering with multiple cameras
        if (mLastFrameNumber != cv->getTraversalNumber())
        {
            mLastFrameNumber = cv->getTraversalNumber();

            // Get the node bounds in view space
            // NB: do not node->getBound() * modelView, that would apply the node's transformation twice
            osg::BoundingSphere nodeBound;
            const osg::Transform* transform = node->asTransform();
            if (transform)
            {
                for (unsigned int i = 0; i < transform->getNumChildren(); ++i)
                    nodeBound.expandBy(transform->getChild(i)->getBound());
            }
            else
                nodeBound = node->getBound();

            transformBoundingSphere(*cv->getModelViewMatrix(), nodeBound);

            mLightManager->getLightsIntersecting(cv, viewMatrix, mLastFrameNumber, nodeBound, mLightList);
            if (!mIgnoredLightSources.empty())
                std::erase_if(mLightList, [&](const LightManager::LightSourceViewBound* light) {
                    return mIgnoredLightSources.contains(light->mLightSource);
                });

            const size_t maxLights = mLightManager->getMaxLights();

            if (mLightList.size() > maxLights)
            {
                // Sort by proximity to object: prefer closer lights with larger radius
                std::sort(mLightList.begin(), mLightList.end(),
                    [&](const SceneUtil::LightManager::LightSourceViewBound* left,
                        const SceneUtil::LightManager::LightSourceViewBound* right) {
                        const float leftDist = (nodeBound.center() - left->mViewBound.center()).length2();
                        const float rightDist = (nodeBound.center() - right->mViewBound.center()).length2();
                        // A tricky way to compare normalized distance. This avoids division by near zero
                        return left->mViewBound.radius() * rightDist > right->mViewBound.radius() * leftDist;
                    });

                mLightList.resize(maxLights);
            }
        }

        if (!mLightList.empty())
        {
            cv->pushStateSet(mLightManager->getLightListStateSet(mLightList, mLastFrameNumber, viewMatrix));
            return true;
        }
        return false;
    }
}
