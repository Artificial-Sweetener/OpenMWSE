#include "texturedclouds.hpp"

#include <algorithm>
#include <cmath>

#include <osg/Geometry>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

namespace SceneUtil
{
    namespace
    {
        /// Sample periodic source alpha at normalized texel centers, including compressed weather assets.
        float sampleOpacity(const osg::Image* image, float u, float v)
        {
            if (!image || image->s() < 1 || image->t() < 1)
                return 0.f;
            const float x = u * image->s() - 0.5f;
            const float y = v * image->t() - 0.5f;
            const int x0 = static_cast<int>(std::floor(x));
            const int y0 = static_cast<int>(std::floor(y));
            const float fx = x - x0;
            const float fy = y - y0;
            const auto alpha = [&](int px, int py) {
                return image->getColor((px + image->s()) % image->s(), (py + image->t()) % image->t()).a();
            };
            return std::lerp(std::lerp(alpha(x0, y0), alpha(x0 + 1, y0), fx),
                std::lerp(alpha(x0, y0 + 1), alpha(x0 + 1, y0 + 1), fx), fy);
        }
    }

    void TexturedClouds::setDome(osg::Node& dome)
    {
        constexpr int size = 256;
        const auto bounds = dome.getBound();
        const double distance = bounds.center().length() + bounds.radius() + 1.0;
        osg::ref_ptr<osg::Image> mapping = new osg::Image;
        mapping->allocateImage(size, size, 1, GL_RGBA, GL_FLOAT);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                auto* pixel = reinterpret_cast<float*>(mapping->data(x, y));
                std::fill_n(pixel, 4, 0.f);
                // Stereographic coordinates have no longitude seam or singularity at the zenith.
                const double px = 2.0 * (x + 0.5) / size - 1.0;
                const double py = 2.0 * (y + 0.5) / size - 1.0;
                const double squaredRadius = px * px + py * py;
                if (squaredRadius > 1.0)
                    continue;
                const osg::Vec3d direction
                    = osg::Vec3d(2.0 * px, 2.0 * py, 1.0 - squaredRadius) / (1.0 + squaredRadius);
                osg::ref_ptr<osgUtil::LineSegmentIntersector> ray
                    = new osgUtil::LineSegmentIntersector(osg::Vec3d(), direction * distance);
                ray->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
                osgUtil::IntersectionVisitor visitor(ray);
                dome.accept(visitor);
                if (!ray->containsIntersections())
                    continue;
                const auto& hit = *ray->getIntersections().begin();
                const auto* geometry = hit.drawable->asGeometry();
                const auto* uv
                    = geometry ? dynamic_cast<const osg::Vec2Array*>(geometry->getTexCoordArray(0)) : nullptr;
                if (!uv)
                    continue;
                const auto* colors = dynamic_cast<const osg::Vec4Array*>(geometry->getColorArray());
                osg::Vec2d coordinate;
                double alpha = 0.0;
                for (size_t index = 0; index < hit.indexList.size(); ++index)
                {
                    const auto vertex = hit.indexList[index];
                    const double weight = hit.ratioList[index];
                    if (vertex >= uv->size())
                        continue;
                    coordinate += osg::Vec2d((*uv)[vertex]) * weight;
                    const auto colorIndex = colors && colors->getBinding() == osg::Array::BIND_PER_VERTEX ? vertex : 0;
                    alpha += weight * (colors && colorIndex < colors->size() ? (*colors)[colorIndex].a() : 1.0);
                }
                // Premultiplication keeps empty texels from pulling edge UVs toward (0, 0).
                pixel[0] = static_cast<float>(coordinate.x() * alpha);
                pixel[1] = static_cast<float>(coordinate.y() * alpha);
                pixel[2] = static_cast<float>(alpha);
                pixel[3] = 1.f;
            }
        mDomeMapping = new osg::Texture2D(mapping);
        mDomeMapping->setName("Authored cloud dome UV and fade");
        mDomeMapping->setInternalFormat(GL_RGBA32F_ARB);
        mDomeMapping->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mDomeMapping->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mDomeMapping->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mDomeMapping->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mDomeMapping->setResizeNonPowerOfTwoHint(false);
    }

    void TexturedClouds::setImages(osg::Image* outgoing, osg::Image* incoming)
    {
        if (mOpacity && mImages[0] == outgoing && mImages[1] == incoming)
            return;
        mImages = { outgoing, incoming };
        int width = 1;
        int height = 1;
        for (const auto& source : mImages)
            if (source)
            {
                width = std::max(width, std::min(source->s(), 1024));
                height = std::max(height, std::min(source->t(), 1024));
            }
        osg::ref_ptr<osg::Image> opacity = new osg::Image;
        opacity->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                unsigned char* pixel = opacity->data(x, y);
                for (int layer = 0; layer < 2; ++layer)
                {
                    const float alpha = sampleOpacity(mImages[layer], (x + 0.5f) / width, (y + 0.5f) / height);
                    pixel[layer] = static_cast<unsigned char>(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f);
                }
                pixel[2] = 0;
                pixel[3] = 255;
            }
        mOpacity = new osg::Texture2D(opacity);
        mOpacity->setName("Weather cloud opacity");
        mOpacity->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        mOpacity->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        mOpacity->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        mOpacity->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mOpacity->setResizeNonPowerOfTwoHint(false);
        mOpacity->setUseHardwareMipMapGeneration(true);
    }

    void applyTexturedCloudState(
        osg::StateSet& stateset, const TexturedCloudState& clouds, int textureUnit, int domeTextureUnit)
    {
        stateset.getOrCreateUniform("openmwseTexturedCloudsEnabled", osg::Uniform::BOOL)
            ->set(clouds.mEnabled && clouds.mOpacity.valid());
        stateset.getOrCreateUniform("openmwseTexturedCloudWeights", osg::Uniform::FLOAT_VEC2)->set(clouds.mWeights);
        auto* transforms = stateset.getOrCreateUniform("openmwseTexturedCloudTransforms", osg::Uniform::FLOAT_VEC4, 2);
        for (unsigned int layer = 0; layer < 2; ++layer)
            transforms->setElement(layer, clouds.mTransforms[layer]);
        stateset.getOrCreateUniform("openmwseTexturedCloudOpacity", osg::Uniform::SAMPLER_2D)->set(textureUnit);
        if (clouds.mOpacity)
            stateset.setTextureAttribute(textureUnit, clouds.mOpacity, osg::StateAttribute::ON);
        stateset.getOrCreateUniform("openmwseTexturedCloudDomeMapping", osg::Uniform::SAMPLER_2D)->set(domeTextureUnit);
        stateset.getOrCreateUniform("openmwseTexturedCloudDomeAvailable", osg::Uniform::BOOL)
            ->set(clouds.mDomeMapping.valid());
        if (clouds.mDomeMapping)
            stateset.setTextureAttribute(domeTextureUnit, clouds.mDomeMapping, osg::StateAttribute::ON);
    }
}
