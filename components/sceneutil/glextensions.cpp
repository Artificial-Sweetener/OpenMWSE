#include "glextensions.hpp"

#include <osg/GraphicsContext>

#ifndef GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS
#define GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS 0x90D6
#endif
#ifndef GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#endif

namespace SceneUtil
{
    namespace
    {
        std::set<osg::observer_ptr<osg::GLExtensions>> sGLExtensions;
        GLint sMaxShaderStorageBindings = 0;
        GLint sMaxVertexStorageBlocks = 0;

        class GLExtensionsObserver : public osg::Observer
        {
        public:
            static GLExtensionsObserver sInstance;

            ~GLExtensionsObserver() override
            {
                for (auto& ptr : sGLExtensions)
                {
                    osg::ref_ptr<osg::GLExtensions> ref;
                    if (ptr.lock(ref))
                        ref->removeObserver(this);
                }
            }

            void objectDeleted(void* referenced) override
            {
                sGLExtensions.erase(static_cast<osg::GLExtensions*>(referenced));
            }
        };

        // construct after sGLExtensions so this gets destroyed first.
        GLExtensionsObserver GLExtensionsObserver::sInstance{};
    }

    bool glExtensionsReady()
    {
        return !sGLExtensions.empty();
    }

    osg::GLExtensions& getGLExtensions()
    {
        if (sGLExtensions.empty())
            throw std::runtime_error(
                "GetGLExtensionsOperation was not used when the current context was created or there is no current "
                "context");
        return **sGLExtensions.begin();
    }

    GLint getMaxShaderStorageBindings()
    {
        return sMaxShaderStorageBindings;
    }

    GLint getMaxVertexStorageBlocks()
    {
        return sMaxVertexStorageBlocks;
    }

    GetGLExtensionsOperation::GetGLExtensionsOperation()
        : GraphicsOperation("GetGLExtensionsOperation", false)
    {
    }

    void GetGLExtensionsOperation::operator()(osg::GraphicsContext* graphicsContext)
    {
        auto [itr, _] = sGLExtensions.emplace(graphicsContext->getState()->get<osg::GLExtensions>());
        (*itr)->addObserver(&GLExtensionsObserver::sInstance);
        // Storage limits exist from GL 4.3 on.
        if (static_cast<int>((*itr)->glslLanguageVersion * 100) >= 430)
        {
            glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &sMaxShaderStorageBindings);
            glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &sMaxVertexStorageBlocks);
        }
    }
}
