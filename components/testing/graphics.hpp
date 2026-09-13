#ifndef OPENMW_COMPONENTS_TESTING_GRAPHICS_H
#define OPENMW_COMPONENTS_TESTING_GRAPHICS_H

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <osg/GraphicsContext>
#include <osg/Image>

namespace TestingOpenMW
{
    /// Creates and activates a small compatibility-profile context for real OpenGL tests.
    inline osg::ref_ptr<osg::GraphicsContext> createGraphicsContext(const char* version = "4.3")
    {
        const osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 8;
        traits->height = 8;
        traits->pbuffer = true;
        traits->doubleBuffer = false;
        traits->glContextVersion = version;
        traits->glContextProfileMask = 2;
        const osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            return nullptr;
        return context;
    }

    /// Creates a uniformly initialized test image with the requested mip-level layout.
    inline osg::ref_ptr<osg::Image> makeImage(
        int width, int height, GLenum format = GL_RGBA, unsigned int levels = 1, unsigned char value = 0)
    {
        osg::Image::MipmapDataType offsets;
        std::size_t bytes = 0;
        for (unsigned int level = 0; level < levels; ++level)
        {
            if (level > 0)
                offsets.push_back(static_cast<unsigned int>(bytes));
            bytes += osg::Image::computeImageSizeInBytes(
                std::max(width >> level, 1), std::max(height >> level, 1), 1, format, GL_UNSIGNED_BYTE, 1);
        }
        const osg::ref_ptr<osg::Image> image = new osg::Image;
        auto* data = new unsigned char[bytes];
        std::memset(data, value, bytes);
        image->setImage(width, height, 1, format, format, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        image->setMipmapLevels(offsets);
        return image;
    }
}

#endif
