#include <apps/openmw/mwrender/gpuscenekeys.hpp>

#include <gtest/gtest.h>

#include <osg/Image>
#include <osg/ImageStream>
#include <osg/Texture2D>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
    using MWRender::SamplerKey;
    using MWRender::samplerKey;
    using MWRender::storage;
    using MWRender::StorageKey;

    constexpr GLint sGenericCompressedRgba = 0x84EE;

    osg::ref_ptr<osg::Image> makeImage(
        osg::Image* image, int width, int height, GLint internal, GLenum format, std::size_t bytes)
    {
        unsigned char* data = new unsigned char[bytes];
        std::memset(data, 0, bytes);
        image->setImage(width, height, 1, internal, format, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        return image;
    }

    osg::ref_ptr<osg::Image> makeRgba(int width, int height)
    {
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
        return makeImage(new osg::Image, width, height, GL_RGBA, GL_RGBA, bytes);
    }

    /// Supplies initial mip levels.
    osg::ref_ptr<osg::Image> makeMipImage(int size, unsigned int levels)
    {
        osg::Image::MipmapDataType offsets;
        std::size_t bytes = 0;
        for (unsigned int level = 0; level < levels; ++level)
        {
            if (level > 0)
                offsets.push_back(static_cast<unsigned int>(bytes));
            const std::size_t side = static_cast<std::size_t>(std::max(size >> level, 1));
            bytes += side * side * 4;
        }
        osg::ref_ptr<osg::Image> image = makeImage(new osg::Image, size, size, GL_RGBA, GL_RGBA, bytes);
        image->setMipmapLevels(offsets);
        return image;
    }

    osg::ref_ptr<osg::Texture2D> makeTexture(osg::Image* image)
    {
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setUseHardwareMipMapGeneration(false);
        return texture;
    }

    osg::ref_ptr<osg::Texture2D> makeStatic(int width = 4, int height = 4)
    {
        return makeTexture(makeRgba(width, height));
    }

    StorageKey keyOf(const osg::Texture2D& texture)
    {
        StorageKey key;
        EXPECT_TRUE(storage(texture, key));
        return key;
    }

    const char* rejectionOf(const osg::Texture2D& texture)
    {
        StorageKey key;
        const char* reason = "";
        EXPECT_FALSE(storage(texture, key, &reason));
        return reason;
    }

    /// Distinct keys order strictly one way.
    template <class Key>
    void expectDistinct(const Key& left, const Key& right)
    {
        EXPECT_NE(left < right, right < left);
    }

    template <class Key>
    void expectSame(const Key& left, const Key& right)
    {
        EXPECT_FALSE(left < right);
        EXPECT_FALSE(right < left);
    }

    class NullSubload : public osg::Texture2D::SubloadCallback
    {
    public:
        void load(const osg::Texture2D&, osg::State&) const override {}
        void subload(const osg::Texture2D&, osg::State&) const override {}
    };

    TEST(GpuSceneKeysTest, AcceptsStaticTexture)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeStatic();
        StorageKey key;
        ASSERT_TRUE(storage(*texture, key));
        EXPECT_EQ(key.mWidth, 4);
        EXPECT_EQ(key.mHeight, 4);
        EXPECT_EQ(key.mFormat, static_cast<GLenum>(GL_RGBA));
        EXPECT_EQ(key.mType, static_cast<GLenum>(GL_UNSIGNED_BYTE));
        EXPECT_EQ(key.mImageInternal, static_cast<GLint>(GL_RGBA));
        EXPECT_EQ(key.mInternal, static_cast<GLint>(GL_RGBA8));
        EXPECT_EQ(key.mPacking, 1u);
        EXPECT_EQ(key.mLevels, 1u);
        EXPECT_FALSE(key.mGenerated);
        EXPECT_EQ(key.mBytes, 64u);
    }

    TEST(GpuSceneKeysTest, RejectsMissingImage)
    {
        const osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
        EXPECT_STREQ(rejectionOf(*texture), "dynamic");
    }

    TEST(GpuSceneKeysTest, RejectsDynamicImage)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeStatic();
        texture->getImage()->setDataVariance(osg::Object::DYNAMIC);
        EXPECT_STREQ(rejectionOf(*texture), "dynamic");
    }

    TEST(GpuSceneKeysTest, RejectsDynamicTexture)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeStatic();
        texture->setDataVariance(osg::Object::DYNAMIC);
        EXPECT_STREQ(rejectionOf(*texture), "dynamic");
    }

    TEST(GpuSceneKeysTest, RejectsTextureBorder)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeStatic();
        texture->setBorderWidth(1);
        EXPECT_STREQ(rejectionOf(*texture), "dynamic");
    }

    TEST(GpuSceneKeysTest, RejectsImageStream)
    {
        const osg::ref_ptr<osg::Image> stream = makeImage(new osg::ImageStream, 4, 4, GL_RGBA, GL_RGBA, 64);
        ASSERT_TRUE(stream->valid());
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(stream);
        EXPECT_STREQ(rejectionOf(*texture), "dynamic");
        // The same pixels pass outside a stream.
        StorageKey key;
        EXPECT_TRUE(storage(*makeStatic(), key));
    }

    TEST(GpuSceneKeysTest, RejectsSubloadCallback)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeStatic();
        texture->setSubloadCallback(new NullSubload);
        EXPECT_STREQ(rejectionOf(*texture), "dynamic");
    }

    TEST(GpuSceneKeysTest, AcceptsCompleteMipChain)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeMipImage(8, 4));
        StorageKey key;
        ASSERT_TRUE(storage(*texture, key));
        EXPECT_EQ(key.mLevels, 4u);
        EXPECT_FALSE(key.mGenerated);
        EXPECT_EQ(key.mBytes, 340u);
    }

    TEST(GpuSceneKeysTest, AcceptsPartialMipChain)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeMipImage(8, 2));
        ASSERT_TRUE(texture->getImage()->isMipmap());
        StorageKey key;
        ASSERT_TRUE(storage(*texture, key));
        EXPECT_EQ(key.mLevels, 2u);
        EXPECT_FALSE(key.mGenerated);
        EXPECT_EQ(key.mBytes, 320u);
        expectDistinct(key, keyOf(*makeTexture(makeMipImage(8, 4))));
    }

    TEST(GpuSceneKeysTest, GeneratesMissingMipLevels)
    {
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(makeRgba(8, 8));
        texture->setUseHardwareMipMapGeneration(true);
        StorageKey key;
        ASSERT_TRUE(storage(*texture, key));
        EXPECT_TRUE(key.mGenerated);
        EXPECT_EQ(key.mLevels, 4u);
        EXPECT_EQ(key.mBytes, 340u);
        EXPECT_GT(key.mBytes, texture->getImage()->getTotalSizeInBytes());
    }

    TEST(GpuSceneKeysTest, AcceptsCompressedBlocks)
    {
        const osg::ref_ptr<osg::Image> image
            = makeImage(new osg::Image, 8, 8, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 32);
        ASSERT_TRUE(image->isCompressed());
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(image);
        StorageKey key;
        ASSERT_TRUE(storage(*texture, key));
        EXPECT_EQ(key.mInternal, static_cast<GLint>(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT));
        EXPECT_EQ(key.mInternal, key.mImageInternal);
        EXPECT_EQ(key.mFormat, static_cast<GLenum>(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT));
        EXPECT_EQ(key.mLevels, 1u);
        EXPECT_EQ(key.mBytes, 32u);
    }

    TEST(GpuSceneKeysTest, ConvertsAlphaStorage)
    {
        const osg::ref_ptr<osg::Image> image = makeImage(new osg::Image, 4, 4, GL_ALPHA, GL_ALPHA, 16);
        const osg::ref_ptr<osg::Texture2D> texture = makeTexture(image);
        StorageKey key;
        ASSERT_TRUE(storage(*texture, key));
        EXPECT_EQ(key.mInternal, static_cast<GLint>(GL_R8));
        EXPECT_EQ(key.mFormat, static_cast<GLenum>(GL_RED));
        EXPECT_EQ(key.mSwizzle, (std::array<int, 4>{ GL_ONE, GL_ONE, GL_ONE, GL_RED }));
        EXPECT_EQ(key.mBytes, 16u);
    }

    TEST(GpuSceneKeysTest, RejectsGenericCompressed)
    {
        const osg::ref_ptr<osg::Image> image
            = makeImage(new osg::Image, 8, 8, sGenericCompressedRgba, sGenericCompressedRgba, 32);
        ASSERT_TRUE(image->isCompressed());
        EXPECT_STREQ(rejectionOf(*makeTexture(image)), "reinterpret");
    }

    TEST(GpuSceneKeysTest, MatchesIdenticalStorage)
    {
        expectSame(keyOf(*makeStatic()), keyOf(*makeStatic()));
    }

    TEST(GpuSceneKeysTest, SeparatesStorageSize)
    {
        const StorageKey base = keyOf(*makeStatic(4, 4));
        const StorageKey wider = keyOf(*makeStatic(8, 4));
        const StorageKey taller = keyOf(*makeStatic(4, 8));
        EXPECT_NE(base.mWidth, wider.mWidth);
        EXPECT_NE(base.mHeight, taller.mHeight);
        expectDistinct(base, wider);
        expectDistinct(base, taller);
        expectDistinct(wider, taller);
    }

    TEST(GpuSceneKeysTest, SeparatesStorageFormat)
    {
        const StorageKey base = keyOf(*makeStatic());
        const osg::ref_ptr<osg::Texture2D> rgb = makeTexture(makeImage(new osg::Image, 4, 4, GL_RGB, GL_RGB, 48));
        const StorageKey other = keyOf(*rgb);
        EXPECT_NE(base.mInternal, other.mInternal);
        expectDistinct(base, other);

        const osg::ref_ptr<osg::Texture2D> packed = makeStatic();
        packed->getImage()->setPacking(4);
        const StorageKey repacked = keyOf(*packed);
        EXPECT_NE(base.mPacking, repacked.mPacking);
        EXPECT_EQ(base.mBytes, repacked.mBytes);
        expectDistinct(base, repacked);
    }

    TEST(GpuSceneKeysTest, SeparatesStorageLayout)
    {
        const StorageKey base = keyOf(*makeStatic());

        const osg::ref_ptr<osg::Texture2D> kept = makeStatic();
        kept->setResizeNonPowerOfTwoHint(false);
        const StorageKey resized = keyOf(*kept);
        EXPECT_NE(base.mResize, resized.mResize);
        expectDistinct(base, resized);

        const osg::ref_ptr<osg::Texture2D> swizzled = makeStatic();
        swizzled->setSwizzle(osg::Vec4i(GL_RED, GL_RED, GL_RED, GL_ALPHA));
        const StorageKey swapped = keyOf(*swizzled);
        EXPECT_NE(base.mSwizzle, swapped.mSwizzle);
        expectDistinct(base, swapped);
    }

    TEST(GpuSceneKeysTest, MatchesIdenticalSamplers)
    {
        const osg::ref_ptr<osg::Texture2D> left = new osg::Texture2D;
        const osg::ref_ptr<osg::Texture2D> right = new osg::Texture2D;
        expectSame(samplerKey(*left), samplerKey(*right));
    }

    TEST(GpuSceneKeysTest, SeparatesSamplerWraps)
    {
        const osg::ref_ptr<osg::Texture2D> plain = new osg::Texture2D;
        const SamplerKey base = samplerKey(*plain);
        for (const osg::Texture::WrapParameter which :
            { osg::Texture::WRAP_S, osg::Texture::WRAP_T, osg::Texture::WRAP_R })
        {
            const osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
            texture->setWrap(which, osg::Texture::REPEAT);
            expectDistinct(base, samplerKey(*texture));
        }
    }

    TEST(GpuSceneKeysTest, SeparatesSamplerFilters)
    {
        const osg::ref_ptr<osg::Texture2D> plain = new osg::Texture2D;
        const SamplerKey base = samplerKey(*plain);

        const osg::ref_ptr<osg::Texture2D> minified = new osg::Texture2D;
        minified->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        expectDistinct(base, samplerKey(*minified));

        const osg::ref_ptr<osg::Texture2D> magnified = new osg::Texture2D;
        magnified->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        expectDistinct(base, samplerKey(*magnified));

        const osg::ref_ptr<osg::Texture2D> sharp = new osg::Texture2D;
        sharp->setMaxAnisotropy(8.f);
        expectDistinct(base, samplerKey(*sharp));
    }

    TEST(GpuSceneKeysTest, SeparatesSamplerLod)
    {
        const osg::ref_ptr<osg::Texture2D> plain = new osg::Texture2D;
        const SamplerKey base = samplerKey(*plain);

        const osg::ref_ptr<osg::Texture2D> floored = new osg::Texture2D;
        floored->setMinLOD(1.0);
        expectDistinct(base, samplerKey(*floored));

        const osg::ref_ptr<osg::Texture2D> capped = new osg::Texture2D;
        capped->setMaxLOD(4.0);
        expectDistinct(base, samplerKey(*capped));

        const osg::ref_ptr<osg::Texture2D> biased = new osg::Texture2D;
        biased->setLODBias(0.5);
        expectDistinct(base, samplerKey(*biased));
    }

    TEST(GpuSceneKeysTest, SeparatesSamplerBorder)
    {
        const osg::ref_ptr<osg::Texture2D> plain = new osg::Texture2D;
        const SamplerKey base = samplerKey(*plain);
        const osg::ref_ptr<osg::Texture2D> bordered = new osg::Texture2D;
        bordered->setBorderColor(osg::Vec4d(1.0, 0.0, 0.0, 1.0));
        const SamplerKey key = samplerKey(*bordered);
        EXPECT_NE(base.mBorder, key.mBorder);
        expectDistinct(base, key);
    }

    TEST(GpuSceneKeysTest, SeparatesSamplerCompare)
    {
        const osg::ref_ptr<osg::Texture2D> plain = new osg::Texture2D;
        const SamplerKey base = samplerKey(*plain);

        const osg::ref_ptr<osg::Texture2D> compared = new osg::Texture2D;
        compared->setShadowComparison(true);
        expectDistinct(base, samplerKey(*compared));

        const osg::ref_ptr<osg::Texture2D> reversed = new osg::Texture2D;
        reversed->setShadowCompareFunc(osg::Texture::GEQUAL);
        expectDistinct(base, samplerKey(*reversed));

        const osg::ref_ptr<osg::Texture2D> intense = new osg::Texture2D;
        intense->setShadowTextureMode(osg::Texture::INTENSITY);
        expectDistinct(base, samplerKey(*intense));
    }

    TEST(GpuSceneKeysTest, EqualsMatchesOrdering)
    {
        const osg::ref_ptr<osg::Texture2D> left = new osg::Texture2D;
        const osg::ref_ptr<osg::Texture2D> right = new osg::Texture2D;
        const SamplerKey a = samplerKey(*left);
        SamplerKey b = samplerKey(*right);
        EXPECT_TRUE(a == b);
        EXPECT_EQ(a == b, !(a < b) && !(b < a));

        right->setMaxAnisotropy(4.f);
        b = samplerKey(*right);
        EXPECT_FALSE(a == b);
        EXPECT_EQ(a == b, !(a < b) && !(b < a));
    }
}
