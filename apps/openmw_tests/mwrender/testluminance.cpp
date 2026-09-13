#include <apps/openmw/mwrender/luminancecalculator.hpp>
#include <apps/openmw/mwrender/pingpongcanvas.hpp>

#include <components/shader/shadermanager.hpp>
#include <components/testing/graphics.hpp>

#include <gtest/gtest.h>

#include <osg/ContextData>
#include <osg/GraphicsContext>
#include <osg/RenderInfo>

#include <cmath>
#include <filesystem>
#include <memory>

namespace
{
    TEST(LuminanceTest, RecyclesTextureStorage)
    {
        const auto context = TestingOpenMW::createGraphicsContext();
        if (!context)
            GTEST_SKIP() << "Offscreen GL context unavailable";
        auto& state = *context->getState();
        auto* ext = state.get<osg::GLExtensions>();
        auto* pool = osg::get<osg::TextureObjectManager>(state.getContextID());
        pool->setMaxTexturePoolSize(64u * 1024u * 1024u);
        Shader::ShaderManager shaders;
        shaders.setShaderPath(std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "shaders");
        auto defines = Shader::getDefaultDefines();
        defines["lightingMethodClustered"] = "0";
        defines["maxLights"] = "16";
        defines["simpleLighting"] = "0";
        shaders.setGlobalDefines(defines);
        auto luminance = std::make_shared<MWRender::LuminanceCalculator>(shaders);
        const osg::ref_ptr<MWRender::PingPongCanvas> canvas
            = new MWRender::PingPongCanvas(shaders, luminance, nullptr, nullptr);
        const osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE);
        image->data()[0] = image->data()[1] = image->data()[2] = 128;
        const osg::ref_ptr<osg::Texture2D> scene = new osg::Texture2D(image);
        scene->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        canvas->setTextureScene(scene);
        luminance->enable();
        osg::RenderInfo info(&state, nullptr);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        for (const int width : { 128, 256, 128, 128 })
        {
            SCOPED_TRACE(width);
            const int side = width / 2;
            const osg::ref_ptr<osg::Texture2D> recycled = new osg::Texture2D;
            recycled->setTextureSize(side, side);
            recycled->setInternalFormat(GL_R16F);
            recycled->setSourceFormat(GL_RED);
            recycled->setNumMipmapLevels(osg::Image::computeNumberOfMipmapLevels(side, side));
            osg::ref_ptr<osg::Texture::TextureObject> object = pool->generateTextureObject(
                recycled, GL_TEXTURE_2D, recycled->getNumMipmapLevels(), GL_R16F, side, side, 1, 0);
            object->bind();
            if (!object->isAllocated())
            {
                ext->glTexStorage2D(GL_TEXTURE_2D, recycled->getNumMipmapLevels(), GL_R16F, side, side);
                object->setAllocated();
            }
            const osg::Texture::TextureProfile profile = object->_profile;
            object->release();
            object = nullptr;
            pool->handlePendingOrphandedTextureObjects();
            ASSERT_GT(pool->getTextureObjectSet(profile)->getNumOrphans(), 0u);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
            luminance->dirty(width, width);
            glViewport(0, 0, side, side);
            for (std::size_t frame : { 0u, 1u, 0u, 1u })
            {
                luminance->draw(*canvas, info, state, ext, frame);
                EXPECT_EQ(glGetError(), GL_NO_ERROR);
                const auto texture = luminance->getLuminanceTexture(frame);
                state.applyTextureAttribute(0, texture);
                float value = 0;
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, &value);
                EXPECT_TRUE(std::isfinite(value));
                EXPECT_GT(value, 0);
                EXPECT_EQ(glGetError(), GL_NO_ERROR);
            }
        }
        state.dirtyAllAttributes();
        canvas->releaseGLObjects(&state);
        shaders.releaseGLObjects(&state);
        context->releaseContext();
        context->close();
    }
}
