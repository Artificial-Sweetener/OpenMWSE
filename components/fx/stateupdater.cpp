#include "stateupdater.hpp"

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>

#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/cloudshaderstate.hpp>

namespace Fx
{
    std::string StateUpdater::sDefinition = UniformData::getDefinition("_omw_data");

    StateUpdater::StateUpdater(bool useUBO)
        : mUseUBO(useUBO)
    {
        setCloudLayers(SceneUtil::packCloudLayers({}, {}, {}));
        setAtmosphereTerrain({});
    }

    std::optional<int> StateUpdater::getPublishedFrameNumber(const osg::StateSet& stateset)
    {
        const auto* binding = dynamic_cast<const osg::UniformBufferBinding*>(
            stateset.getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor)));
        if (binding)
        {
            const auto* buffer
                = dynamic_cast<const osg::BufferTemplate<UniformData::BufferType>*>(binding->getBufferData());
            if (!buffer)
                throw std::runtime_error("Post-processing frame identity has an incompatible uniform buffer");
            return UniformData::read<FrameNumber>(buffer->getData());
        }
        if (const osg::Uniform* uniform = stateset.getUniform("omw.frameNumber"))
        {
            int frame;
            if (!uniform->get(frame))
                throw std::runtime_error("Post-processing frame identity must be an integer uniform");
            return frame;
        }
        return std::nullopt;
    }

    std::optional<SceneUtil::CloudLayerUniforms> StateUpdater::getPublishedCloudLayers(const osg::StateSet& stateset)
    {
        const auto* binding = dynamic_cast<const osg::UniformBufferBinding*>(
            stateset.getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor)));
        if (binding)
        {
            const auto* buffer
                = dynamic_cast<const osg::BufferTemplate<UniformData::BufferType>*>(binding->getBufferData());
            if (!buffer)
                throw std::runtime_error("Post-processing cloud models have an incompatible uniform buffer");
            return UniformData::read<AtmosphereCloudLayers>(buffer->getData());
        }
        if (const osg::Uniform* uniform = stateset.getUniform("omw.atmosphereCloudLayers"))
        {
            SceneUtil::CloudLayerUniforms layers;
            if (uniform->getNumElements() != layers.size())
                throw std::runtime_error("Post-processing cloud models have an incompatible uniform array size");
            for (unsigned int index = 0; index < layers.size(); ++index)
                if (!uniform->getElement(index, layers[index]))
                    throw std::runtime_error("Post-processing cloud models require vec4 array elements");
            return layers;
        }
        return std::nullopt;
    }

    std::optional<SceneUtil::AtmosphereTerrainGrid> StateUpdater::getPublishedAtmosphereTerrain(
        const osg::StateSet& stateset)
    {
        SceneUtil::AtmosphereTerrainGrid terrain;
        const auto* binding = dynamic_cast<const osg::UniformBufferBinding*>(
            stateset.getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor)));
        if (binding)
        {
            const auto* buffer
                = dynamic_cast<const osg::BufferTemplate<UniformData::BufferType>*>(binding->getBufferData());
            if (!buffer)
                throw std::runtime_error("Post-processing terrain field has an incompatible uniform buffer");
            terrain.mHeights = UniformData::read<AtmosphereTerrainHeights>(buffer->getData());
            terrain.mOriginSpacing = UniformData::read<AtmosphereTerrainOriginSpacing>(buffer->getData());
            terrain.mHeightBounds = UniformData::read<AtmosphereTerrainHeightBounds>(buffer->getData());
        }
        else if (const osg::Uniform* heights = stateset.getUniform("omw.atmosphereTerrainHeights"))
        {
            const osg::Uniform* origin = stateset.getUniform("omw.atmosphereTerrainOriginSpacing");
            const osg::Uniform* bounds = stateset.getUniform("omw.atmosphereTerrainHeightBounds");
            if (heights->getNumElements() != terrain.mHeights.size() || !origin || !bounds
                || !origin->get(terrain.mOriginSpacing) || !bounds->get(terrain.mHeightBounds))
                throw std::runtime_error("Post-processing terrain field has incompatible uniform dimensions");
            for (unsigned int index = 0; index < terrain.mHeights.size(); ++index)
                if (!heights->getElement(index, terrain.mHeights[index]))
                    throw std::runtime_error("Post-processing terrain field requires vec4 array elements");
        }
        else
            return std::nullopt;
        terrain.mValid = true;
        return terrain;
    }

    void StateUpdater::setDefaults(osg::StateSet* stateset)
    {
        if (mUseUBO)
        {
            osg::ref_ptr<osg::UniformBufferObject> ubo = new osg::UniformBufferObject;

            osg::ref_ptr<osg::BufferTemplate<UniformData::BufferType>> data
                = new osg::BufferTemplate<UniformData::BufferType>();
            data->setBufferObject(ubo);

            osg::ref_ptr<osg::UniformBufferBinding> ubb = new osg::UniformBufferBinding(
                static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor), data, 0, mData.getGPUSize());

            stateset->setAttributeAndModes(ubb, osg::StateAttribute::ON);
        }
        else
        {
            const auto createUniform = [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                std::string name = "omw." + std::string(T::sName);
                if constexpr (std::is_base_of_v<Std140::ArrayTag, T>)
                {
                    osg::ref_ptr<osg::Uniform> uniform = new osg::Uniform(
                        osg::Uniform::FLOAT_VEC4, name.c_str(), static_cast<unsigned int>(T::sArraySize));
                    for (unsigned int index = 0; index < T::sArraySize; ++index)
                        uniform->setElement(index, mData.get<T>()[index]);
                    stateset->addUniform(uniform);
                }
                else
                    stateset->addUniform(new osg::Uniform(name.c_str(), mData.get<T>()));
            };

            std::apply([&](const auto&... v) { (createUniform(v), ...); }, mData.getData());
        }
    }

    /// Publish uniforms and the matching cloud variant to the same frame-owned state set.
    void StateUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        SceneUtil::updateCloudShaderState(*stateset, mData.get<AtmosphereCloudScenarioParameters>());
        if (mUseUBO)
        {
            osg::UniformBufferBinding* ubb = dynamic_cast<osg::UniformBufferBinding*>(
                stateset->getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                    static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor)));
            if (!ubb)
                throw std::runtime_error("StateUpdater::apply: failed to get an UniformBufferBinding!");

            auto& dest = static_cast<osg::BufferTemplate<UniformData::BufferType>*>(ubb->getBufferData())->getData();
            mData.copyTo(dest);

            ubb->getBufferData()->dirty();
        }
        else
        {
            const auto setUniform = [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                std::string name = "omw." + std::string(T::sName);
                if constexpr (std::is_base_of_v<Std140::ArrayTag, T>)
                {
                    for (unsigned int index = 0; index < T::sArraySize; ++index)
                        stateset->getUniform(name)->setElement(index, mData.get<T>()[index]);
                }
                else
                    stateset->getUniform(name)->set(mData.get<T>());
            };

            std::apply([&](const auto&... v) { (setUniform(v), ...); }, mData.getData());
        }

        if (mPointLightBuffer)
            mPointLightBuffer->applyUniforms(nv->getTraversalNumber(), stateset);
    }
}
