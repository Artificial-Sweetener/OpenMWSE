#ifndef OPENMW_MWRENDER_GPUSCENEKEYS_H
#define OPENMW_MWRENDER_GPUSCENEKEYS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <tuple>

#include <osg/Image>
#include <osg/ImageStream>
#include <osg/Texture2D>
#include <osg/Texture>
#include <osg/Vec4d>
#include <osg/Vec4i>

namespace MWRender
{
    /// Return the byte size of one known 4x4 block-compressed image level, or zero for another format.
    inline std::size_t blockCompressedLevelBytes(GLenum format, int width, int height)
    {
        std::size_t blockBytes = 0;
        switch (format)
        {
            case 0x83F0: // DXT1 RGB
            case 0x83F1: // DXT1 RGBA
            case 0x8C4C: // sRGB DXT1
            case 0x8C4D: // sRGBA DXT1
            case 0x8DBB: // red RGTC1
            case 0x8DBC: // signed red RGTC1
            case 0x9270: // red ETC2
            case 0x9271: // signed red ETC2
            case 0x9274: // RGB8 ETC2
            case 0x9275: // sRGB8 ETC2
            case 0x9276: // RGB8 punch-through alpha ETC2
            case 0x9277: // sRGB8 punch-through alpha ETC2
                blockBytes = 8;
                break;
            case 0x83F2: // DXT3 RGBA
            case 0x83F3: // DXT5 RGBA
            case 0x8C4E: // sRGBA DXT3
            case 0x8C4F: // sRGBA DXT5
            case 0x8DBD: // RG RGTC2
            case 0x8DBE: // signed RG RGTC2
            case 0x8E8C: // BPTC unsigned float
            case 0x8E8D: // BPTC signed float
            case 0x8E8E: // BPTC UNORM
            case 0x8E8F: // BPTC sRGB alpha
            case 0x9272: // RG ETC2
            case 0x9273: // signed RG ETC2
            case 0x9278: // RGBA8 ETC2 EAC
            case 0x9279: // sRGBA8 ETC2 EAC
                blockBytes = 16;
                break;
            default:
                return 0;
        }
        const std::size_t blocksWide = static_cast<std::size_t>(std::max(width, 1) + 3) / 4;
        const std::size_t blocksHigh = static_cast<std::size_t>(std::max(height, 1) + 3) / 4;
        return blocksWide * blocksHigh * blockBytes;
    }

    struct StorageKey
    {
        int mWidth = 0;
        int mHeight = 0;
        GLint mInternal = 0;
        GLint mImageInternal = 0;
        GLenum mFormat = 0;
        GLenum mType = 0;
        unsigned int mPacking = 0;
        unsigned int mLevels = 0;
        bool mGenerated = false;
        bool mResize = false;
        std::array<int, 4> mSwizzle{};
        std::size_t mBytes = 0;

        bool operator<(const StorageKey& right) const
        {
            return std::tie(mWidth, mHeight, mInternal, mImageInternal, mFormat, mType, mPacking, mLevels, mGenerated,
                       mResize, mSwizzle, mBytes)
                < std::tie(right.mWidth, right.mHeight, right.mInternal, right.mImageInternal, right.mFormat,
                    right.mType, right.mPacking, right.mLevels, right.mGenerated, right.mResize, right.mSwizzle,
                    right.mBytes);
        }
    };

    struct SamplerKey
    {
        int mWrapS = 0;
        int mWrapT = 0;
        int mWrapR = 0;
        int mMinFilter = 0;
        int mMagFilter = 0;
        float mAnisotropy = 1.f;
        float mMinLod = 0.f;
        float mMaxLod = 0.f;
        float mLodBias = 0.f;
        std::array<double, 4> mBorder{};
        bool mCompare = false;
        int mCompareFunc = 0;
        int mCompareMode = 0;

        bool operator<(const SamplerKey& right) const
        {
            return std::tie(mWrapS, mWrapT, mWrapR, mMinFilter, mMagFilter, mAnisotropy, mMinLod, mMaxLod, mLodBias,
                       mBorder, mCompare, mCompareFunc, mCompareMode)
                < std::tie(right.mWrapS, right.mWrapT, right.mWrapR, right.mMinFilter, right.mMagFilter,
                    right.mAnisotropy, right.mMinLod, right.mMaxLod, right.mLodBias, right.mBorder, right.mCompare,
                    right.mCompareFunc, right.mCompareMode);
        }

        bool operator==(const SamplerKey& right) const { return !(*this < right) && !(right < *this); }
    };

    inline SamplerKey samplerKey(const osg::Texture& texture)
    {
        SamplerKey result;
        result.mWrapS = texture.getWrap(osg::Texture::WRAP_S);
        result.mWrapT = texture.getWrap(osg::Texture::WRAP_T);
        result.mWrapR = texture.getWrap(osg::Texture::WRAP_R);
        result.mMinFilter = texture.getFilter(osg::Texture::MIN_FILTER);
        result.mMagFilter = texture.getFilter(osg::Texture::MAG_FILTER);
        result.mAnisotropy = texture.getMaxAnisotropy();
        result.mMinLod = texture.getMinLOD();
        result.mMaxLod = texture.getMaxLOD();
        result.mLodBias = texture.getLODBias();
        const osg::Vec4d border = texture.getBorderColor();
        for (int i = 0; i < 4; ++i)
            result.mBorder[i] = border[i];
        result.mCompare = texture.getShadowComparison();
        result.mCompareFunc = texture.getShadowCompareFunc();
        result.mCompareMode = texture.getShadowTextureMode();
        return result;
    }

    inline bool storage(const osg::Texture2D& texture, StorageKey& key, const char** reason = nullptr)
    {
        const auto reject = [&](const char* why) {
            if (reason != nullptr)
                *reason = why;
            return false;
        };
        const osg::Image* image = texture.getImage();
        if (image == nullptr || !image->valid() || image->r() != 1 || image->getDataVariance() == osg::Object::DYNAMIC
            || dynamic_cast<const osg::ImageStream*>(image) != nullptr || texture.getSubloadCallback() != nullptr
            || texture.getDataVariance() == osg::Object::DYNAMIC || texture.getBorderWidth() != 0 || image->s() <= 0
            || image->t() <= 0)
            return reject("dynamic");
        key.mWidth = image->s();
        key.mHeight = image->t();
        key.mImageInternal = image->getInternalTextureFormat();
        if (image->isCompressed())
        {
            // Store compressed blocks in their own format.
            key.mInternal = key.mImageInternal;
            switch (key.mInternal)
            {
                case 0x84E9: // generic compressed alpha
                case 0x84EA: // generic compressed luminance
                case 0x84EB: // generic luminance alpha
                case 0x84EC: // generic compressed intensity
                case 0x84ED: // generic compressed rgb
                case 0x84EE: // generic compressed rgba
                case 0x8C48: // generic compressed srgb
                case 0x8C49: // generic srgb alpha
                    return reject("reinterpret");
                default:
                    break;
            }
        }
        else
            key.mInternal = texture.selectSizedInternalFormat(image);
        if (osg::Texture::isCompressedInternalFormat(key.mInternal) != image->isCompressed())
            return reject("reinterpret");
        key.mFormat = image->getPixelFormat();
        key.mType = image->getDataType();
        key.mPacking = image->getPacking();
        key.mResize = texture.getResizeNonPowerOfTwoHint();
        const osg::Vec4i swizzle = texture.getSwizzle();
        for (int i = 0; i < 4; ++i)
            key.mSwizzle[i] = swizzle[i];
        if (!image->isCompressed() && key.mFormat == GL_ALPHA)
        {
            key.mInternal = GL_R8;
            key.mFormat = GL_RED;
            for (int i = 0; i < 4; ++i)
            {
                if (swizzle[i] == GL_ALPHA)
                    key.mSwizzle[i] = GL_RED;
                else if (swizzle[i] == GL_RED || swizzle[i] == GL_GREEN || swizzle[i] == GL_BLUE)
                    key.mSwizzle[i] = GL_ONE;
            }
        }
        const unsigned int complete = osg::Image::computeNumberOfMipmapLevels(image->s(), image->t());
        if (image->isMipmap())
        {
            key.mLevels = image->getNumMipmapLevels();
            if (key.mLevels > complete)
                return reject("mips");
        }
        else if (texture.getUseHardwareMipMapGeneration() && !image->isCompressed())
        {
            key.mLevels = complete;
            key.mGenerated = true;
        }
        else
            key.mLevels = 1;
        key.mBytes = image->getTotalSizeInBytesIncludingMipmaps();
        if (image->isCompressed())
        {
            const std::size_t firstLevelBytes = blockCompressedLevelBytes(key.mFormat, image->s(), image->t());
            if (firstLevelBytes != 0)
            {
                key.mBytes = 0;
                for (unsigned int level = 0; level < key.mLevels; ++level)
                    key.mBytes += blockCompressedLevelBytes(
                        key.mFormat, std::max(image->s() >> level, 1), std::max(image->t() >> level, 1));
            }
        }
        if (key.mGenerated)
        {
            key.mBytes = 0;
            for (unsigned int level = 0; level < key.mLevels; ++level)
                key.mBytes += osg::Image::computeImageSizeInBytes(std::max(image->s() >> level, 1),
                    std::max(image->t() >> level, 1), 1, key.mFormat, key.mType, image->getPacking());
        }
        if (key.mInternal == 0 || key.mFormat == 0 || key.mType == 0 || key.mBytes == 0)
            return reject("format");
        return true;
    }
}

#endif
