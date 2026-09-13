#include "gpusceneinternal.hpp"

#include <SDL_opengl_glext.h>

namespace MWRender
{
    void GpuScene::Textures::prepareTiers()
    {
        if (mTierWork != nullptr)
        {
            if (!mTierWork->isDone())
                return;
            attachTiers(*mTierWork);
            mTierWork = nullptr;
        }
        osg::ref_ptr<TierWork> work = pickTiers();
        if (work == nullptr)
            return;
        if (mWorkQueue == nullptr)
        {
            buildTiers(*work);
            attachTiers(*work);
            return;
        }
        mTierWork = work;
        mWorkQueue->addWorkItem(work);
    }

    /// Builds the template and tier zero.
    void GpuScene::Textures::backProxy(Proxy& proxy)
    {
        proxy.mTemplate = new osg::Texture2D(*proxy.mSource, osg::CopyOp::SHALLOW_COPY);
        proxy.mTemplate->setImage(proxy.mImage);
        proxy.mTemplate->setUnRefImageDataAfterApply(false);
        proxy.mTemplate->setDataVariance(osg::Object::STATIC);
        proxy.mTexture = proxy.mSource;
        proxy.mTiers.push_back({ proxy.mImage, proxy.mSource, proxy.mKey.mBytes });
    }

    osg::ref_ptr<GpuScene::Textures::TierWork> GpuScene::Textures::pickTiers()
    {
        for (Id id = 0; id < mProxies.size(); ++id)
        {
            Proxy& proxy = mProxies[id];
            if (proxy.mPrepared || proxy.mImage == nullptr || proxy.mDesired == 0
                || proxy.mDesired < proxy.mTiers.size())
                continue;
            if (proxy.mTiers.empty())
                backProxy(proxy);
            osg::ref_ptr<TierWork> work = new TierWork;
            work->mProxy = id;
            work->mImage = proxy.mImage;
            work->mTemplate = proxy.mTemplate;
            work->mKey = proxy.mKey;
            work->mFloor = proxy.mFloor;
            return work;
        }
        return nullptr;
    }

    /// Runs without the scene mutex.
    void GpuScene::Textures::buildTiers(TierWork& work)
    {
        const osg::Image& source = *work.mImage;
        for (unsigned int level = 1; level <= work.mFloor; ++level)
        {
            osg::ref_ptr<osg::Image> image;
            if (source.isMipmap())
            {
                image = new osg::Image;
                const unsigned int first = source.getMipmapOffset(level);
                image->setImage(std::max(source.s() >> level, 1), std::max(source.t() >> level, 1), 1,
                    source.getInternalTextureFormat(), source.getPixelFormat(), source.getDataType(),
                    const_cast<unsigned char*>(source.getMipmapData(level)), osg::Image::NO_DELETE,
                    source.getPacking());
                osg::Image::MipmapDataType offsets;
                for (unsigned int tail = level + 1; tail < source.getNumMipmapLevels(); ++tail)
                    offsets.push_back(source.getMipmapOffset(tail) - first);
                image->setMipmapLevels(offsets);
                image->setOrigin(source.getOrigin());
            }
            else if (work.mKey.mGenerated && !source.isCompressed())
            {
                image = new osg::Image(source, osg::CopyOp::DEEP_COPY_ALL);
                image->setMipmapLevels({});
                image->scaleImage(std::max(source.s() >> level, 1), std::max(source.t() >> level, 1), 1);
            }
            if (image == nullptr || !image->valid())
                break;
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(*work.mTemplate, osg::CopyOp::SHALLOW_COPY);
            texture->setImage(image);
            texture->setUseHardwareMipMapGeneration(work.mKey.mGenerated);
            texture->setUnRefImageDataAfterApply(false);
            std::size_t bytes = image->getTotalSizeInBytesIncludingMipmaps();
            if (work.mKey.mGenerated)
            {
                bytes = 0;
                const unsigned int levels = osg::Image::computeNumberOfMipmapLevels(image->s(), image->t());
                for (unsigned int mip = 0; mip < levels; ++mip)
                    bytes += osg::Image::computeImageSizeInBytes(std::max(image->s() >> mip, 1),
                        std::max(image->t() >> mip, 1), 1, image->getPixelFormat(), image->getDataType(),
                        image->getPacking());
            }
            work.mTiers.push_back({ image, texture, bytes });
        }
    }

    void GpuScene::Textures::attachTiers(TierWork& work)
    {
        if (work.mProxy >= mProxies.size())
            return;
        Proxy& proxy = mProxies[work.mProxy];
        // The slot may hold another proxy.
        if (proxy.mTiers.empty() || proxy.mImage != work.mImage || proxy.mPrepared)
            return;
        for (Proxy::Tier& tier : work.mTiers)
            proxy.mTiers.push_back(std::move(tier));
        proxy.mPrepared = true;
        proxy.mFloor = static_cast<unsigned int>(proxy.mTiers.size() - 1);
        proxy.mDesired = std::min(proxy.mDesired, proxy.mFloor);
    }

    void GpuScene::Textures::switchTier(GpuScene& scene, Id proxyId, unsigned int tier)
    {
        Proxy& proxy = mProxies[proxyId];
        if (tier >= proxy.mTiers.size() || tier == proxy.mCurrent || proxy.mRetiring)
            return;
        const osg::ref_ptr<osg::Texture2D> oldTexture = proxy.mTexture;
        const std::size_t oldBytes = proxy.mBytes;
        const std::size_t newBytes = proxy.mTiers[tier].mBytes;
        bool changed = false;
        for (const Id material : proxy.mMaterials)
        {
            if (material >= scene.mMaterials.size() || scene.mMaterialStates[material] == MaterialState::Published)
                continue;
            PendingMaterial pending = scene.mMaterials[material];
            for (unsigned int map = 0; map < sMapCount; ++map)
                if (pending.mProxies[map] == proxyId)
                    pending.mTextures[map] = proxy.mTiers[tier].mTexture;
            if (pending.mMode == Shader::MaterialMode::Bindless)
                scene.mPendingMaterials.push_back(pending);
            else if (pending.mMode == Shader::MaterialMode::Legacy && scene.mMaterialSources[material] != nullptr)
            {
                const auto surface = scene.mSurfaces.find(scene.mMaterialPaths[material]);
                if (surface != scene.mSurfaces.end())
                    for (unsigned int map = 0; map < sMapCount; ++map)
                    {
                        const int unit = scene.mMaterialSources[material]->mUnits[map];
                        if (pending.mProxies[map] == proxyId && unit >= 0)
                            scene.mBucketEdits.push_back(BucketEdit{ surface->second.mBucket,
                                static_cast<unsigned int>(unit), proxy.mTiers[tier].mTexture, pending.mSamplers[map] });
                    }
            }
            scene.mMaterials[material] = std::move(pending);
            changed = true;
        }
        if (!changed)
            return;
        PendingRetire retired;
        retired.mMaterial.mIndex = sInvalid;
        retired.mMaterial.mProxies.fill(sInvalid);
        retired.mMaterial.mArrays.fill(sInvalid);
        // The fence frees the old storage.
        retired.mMaterial.mTextures[0] = oldTexture;
        retired.mBytes = oldBytes;
        mPendingBytes += oldBytes;
        retired.mProxy = proxyId;
        scene.mPendingRetired.push_back(std::move(retired));
        mProxyBytes = mProxyBytes - oldBytes + newBytes;
        proxy.mTexture = proxy.mTiers[tier].mTexture;
        proxy.mBytes = newBytes;
        proxy.mCurrent = tier;
        proxy.mDesired = tier;
        proxy.mRetiring = true;
        ++mMipChanges;
    }

    void GpuScene::Textures::manage(GpuScene& scene, unsigned int frame)
    {
        if (!mResolved || mBudgetBytes == 0)
            return;
        const auto active = [&](const Proxy& proxy) {
            return std::any_of(proxy.mMaterials.begin(), proxy.mMaterials.end(), [&](Id material) {
                return material < scene.mMaterialStates.size()
                    && scene.mMaterialStates[material] != MaterialState::Published
                    && (scene.mMaterialUsers[material] != 0 || frame <= scene.mMaterialLast[material] + 3);
            });
        };
        for (Proxy& proxy : mProxies)
            for (const Id material : proxy.mMaterials)
                if (material < scene.mMaterialLast.size())
                    proxy.mLastVisible = std::max(proxy.mLastVisible, scene.mMaterialLast[material]);

        // Retiring bytes leave after their fence.
        const std::size_t available = budget();
        while (mArrayBytes + mProxyBytes > available)
        {
            Id candidate = sInvalid;
            for (Id id = 0; id < mProxies.size(); ++id)
            {
                const Proxy& proxy = mProxies[id];
                if (proxy.mImage == nullptr || proxy.mRetiring || !active(proxy) || proxy.mCurrent >= proxy.mFloor)
                    continue;
                if (candidate == sInvalid || proxy.mLastVisible < mProxies[candidate].mLastVisible)
                    candidate = id;
            }
            if (candidate == sInvalid)
                break;
            Proxy& proxy = mProxies[candidate];
            proxy.mDesired = proxy.mCurrent + 1;
            if (proxy.mDesired >= proxy.mTiers.size())
                break;
            switchTier(scene, candidate, proxy.mDesired);
        }

        const std::size_t promoteLimit = available * 85 / 100;
        while (mArrayBytes + mProxyBytes + pendingBytes() < promoteLimit)
        {
            Id candidate = sInvalid;
            for (Id id = 0; id < mProxies.size(); ++id)
            {
                const Proxy& proxy = mProxies[id];
                if (proxy.mImage == nullptr || proxy.mRetiring || !active(proxy) || proxy.mCurrent == 0)
                    continue;
                const std::size_t projected = mArrayBytes + mProxyBytes + pendingBytes() - proxy.mBytes
                    + proxy.mTiers[proxy.mCurrent - 1].mBytes;
                if (projected > promoteLimit)
                    continue;
                if (candidate == sInvalid || proxy.mLastVisible > mProxies[candidate].mLastVisible)
                    candidate = id;
            }
            if (candidate == sInvalid)
                break;
            switchTier(scene, candidate, mProxies[candidate].mCurrent - 1);
        }
    }
}
