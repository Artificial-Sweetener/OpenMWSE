// This file is based heavily on code from
// https://github.com/ThermalPixel/osgdemos/blob/master/osgdebug/EnableGLDebugOperation.cpp The original licence is
// included below:
/*
Copyright (c) 2014, Andreas Klein
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the FreeBSD Project.
*/

#include "gldebug.hpp"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <tuple>

#include <osg/UserDataContainer>

#include <components/debug/debuglog.hpp>

// OpenGL constants not provided by OSG:
#include <SDL_opengl_glext.h>

namespace Debug
{
    class DebugMessages : public osg::Object
    {
    public:
        DebugMessages() = default;
        DebugMessages(const DebugMessages& other, const osg::CopyOp& copy)
            : osg::Object(other, copy)
        {
        }
        META_Object(Debug, DebugMessages)

        std::uint64_t count(GLenum source, GLenum type, GLuint id, GLenum severity, std::string_view message)
        {
            if (mVerbose)
                return 1;
            std::lock_guard lock(mMutex);
            const auto key = std::make_tuple(source, type, id, severity, message);
            auto found = mCounts.find(key);
            if (found == mCounts.end())
            {
                if (mCounts.size() >= 256)
                    return 1;
                found = mCounts.emplace(key, 0).first;
            }
            const std::uint64_t count = ++found->second;
            return (count & (count - 1)) == 0 ? count : 0;
        }

    private:
        const bool mVerbose = shouldDebugOpenGL();
        std::mutex mMutex;
        std::map<std::tuple<GLenum, GLenum, GLuint, GLenum, std::string>, std::uint64_t, std::less<>> mCounts;
    };

    void GL_APIENTRY debugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
        const GLchar* message, const void* userParam)
    {
#ifdef GL_DEBUG_OUTPUT
        const std::string_view text(message, length);
        const std::uint64_t count
            = static_cast<DebugMessages*>(const_cast<void*>(userParam))->count(source, type, id, severity, text);
        if (count == 0)
            return;
        std::string srcStr;
        switch (source)
        {
            case GL_DEBUG_SOURCE_API:
                srcStr = "API";
                break;
            case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
                srcStr = "WINDOW_SYSTEM";
                break;
            case GL_DEBUG_SOURCE_SHADER_COMPILER:
                srcStr = "SHADER_COMPILER";
                break;
            case GL_DEBUG_SOURCE_THIRD_PARTY:
                srcStr = "THIRD_PARTY";
                break;
            case GL_DEBUG_SOURCE_APPLICATION:
                srcStr = "APPLICATION";
                break;
            case GL_DEBUG_SOURCE_OTHER:
                srcStr = "OTHER";
                break;
            default:
                srcStr = "UNDEFINED";
                break;
        }

        std::string typeStr;

        Level logSeverity = Warning;
        switch (type)
        {
            case GL_DEBUG_TYPE_ERROR:
                typeStr = "ERROR";
                logSeverity = Error;
                break;
            case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
                typeStr = "DEPRECATED_BEHAVIOR";
                logSeverity = Warning;
                break;
            case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
                typeStr = "UNDEFINED_BEHAVIOR";
                logSeverity = Warning;
                break;
            case GL_DEBUG_TYPE_PORTABILITY:
                typeStr = "PORTABILITY";
                logSeverity = Debug;
                break;
            case GL_DEBUG_TYPE_PERFORMANCE:
                typeStr = "PERFORMANCE";
                logSeverity = Debug;
                break;
            case GL_DEBUG_TYPE_OTHER:
                typeStr = "OTHER";
                logSeverity = Debug;
                break;
            default:
                typeStr = "UNDEFINED";
                break;
        }

        if (severity == GL_DEBUG_SEVERITY_HIGH)
            logSeverity = Error;
        else if (source == GL_DEBUG_SOURCE_SHADER_COMPILER)
            logSeverity = Warning;
        Log(logSeverity) << "OpenGL " << typeStr << " [" << srcStr << "] id=" << id << " severity=" << severity
                         << " count=" << count << ": " << text;
#endif
    }

    void GL_APIENTRY debugCallbackAMD(
        GLuint id, GLenum category, GLenum severity, GLsizei length, const GLchar* message, void* userParam)
    {
        const std::string_view text(message, length);
        const std::uint64_t count = static_cast<DebugMessages*>(userParam)->count(0, category, id, severity, text);
        if (count == 0)
            return;
        const Level level
            = category == GL_DEBUG_CATEGORY_API_ERROR_AMD || severity == GL_DEBUG_SEVERITY_HIGH_AMD ? Error : Warning;
        Log(level) << "OpenGL AMD category=" << category << " id=" << id << " severity=" << severity
                   << " count=" << count << ": " << text;
    }

    class PushDebugGroup
    {
    public:
        static std::unique_ptr<PushDebugGroup> sInstance;

        void(GL_APIENTRY* glPushDebugGroup)(GLenum source, GLuint id, GLsizei length, const GLchar* message);

        void(GL_APIENTRY* glPopDebugGroup)(void);

        bool valid() { return glPushDebugGroup && glPopDebugGroup; }
    };

    std::unique_ptr<PushDebugGroup> PushDebugGroup::sInstance{ std::make_unique<PushDebugGroup>() };

    EnableGLDebugOperation::EnableGLDebugOperation()
        : osg::GraphicsOperation("EnableGLDebugOperation", false)
    {
    }

    void EnableGLDebugOperation::operator()(osg::GraphicsContext* graphicsContext)
    {
#ifdef GL_DEBUG_OUTPUT
        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(mMutex);

        unsigned int contextID = graphicsContext->getState()->getContextID();
        const osg::ref_ptr<DebugMessages> messages = new DebugMessages;
        graphicsContext->getOrCreateUserDataContainer()->addUserObject(messages);

        typedef void(GL_APIENTRY * DEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
            const GLchar* message, const void* userParam);
        typedef void(GL_APIENTRY * GLDebugMessageControlFunction)(
            GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled);
        typedef void(GL_APIENTRY * GLDebugMessageCallbackFunction)(DEBUGPROC, const void* userParam);

        GLDebugMessageControlFunction glDebugMessageControl = nullptr;
        GLDebugMessageCallbackFunction glDebugMessageCallback = nullptr;

        const bool khr = osg::isGLExtensionSupported(contextID, "GL_KHR_debug");
        if (khr)
        {
            osg::setGLExtensionFuncPtr(glDebugMessageCallback, "glDebugMessageCallback");
            osg::setGLExtensionFuncPtr(glDebugMessageControl, "glDebugMessageControl");
            osg::setGLExtensionFuncPtr(PushDebugGroup::sInstance->glPushDebugGroup, "glPushDebugGroup");
            osg::setGLExtensionFuncPtr(PushDebugGroup::sInstance->glPopDebugGroup, "glPopDebugGroup");
        }
        else if (osg::isGLExtensionSupported(contextID, "GL_ARB_debug_output"))
        {
            osg::setGLExtensionFuncPtr(glDebugMessageCallback, "glDebugMessageCallbackARB");
            osg::setGLExtensionFuncPtr(glDebugMessageControl, "glDebugMessageControlARB");
        }
        else if (osg::isGLExtensionSupported(contextID, "GL_AMD_debug_output"))
        {
            PFNGLDEBUGMESSAGECALLBACKAMDPROC callback = nullptr;
            PFNGLDEBUGMESSAGEENABLEAMDPROC enable = nullptr;
            osg::setGLExtensionFuncPtr(callback, "glDebugMessageCallbackAMD");
            osg::setGLExtensionFuncPtr(enable, "glDebugMessageEnableAMD");
            if (callback != nullptr && enable != nullptr)
            {
                callback(debugCallbackAMD, messages.get());
                enable(0, 0, 0, nullptr, false);
                enable(GL_DEBUG_CATEGORY_API_ERROR_AMD, 0, 0, nullptr, true);
                enable(GL_DEBUG_CATEGORY_UNDEFINED_BEHAVIOR_AMD, 0, 0, nullptr, true);
                enable(GL_DEBUG_CATEGORY_SHADER_COMPILER_AMD, 0, 0, nullptr, true);
                enable(0, GL_DEBUG_SEVERITY_HIGH_AMD, 0, nullptr, true);
                if (shouldDebugOpenGL())
                    enable(0, 0, 0, nullptr, true);
                Log(Info) << "OpenGL AMD debug callback attached.";
                return;
            }
        }

        if (glDebugMessageCallback && glDebugMessageControl)
        {
            if (khr)
                glEnable(GL_DEBUG_OUTPUT);
            if (shouldDebugOpenGL())
                glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(debugCallback, messages.get());
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, false);
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR, GL_DONT_CARE, 0, nullptr, true);
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR, GL_DONT_CARE, 0, nullptr, true);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, nullptr, true);
            glDebugMessageControl(GL_DEBUG_SOURCE_SHADER_COMPILER, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
            if (shouldDebugOpenGL())
                glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);

            Log(Info) << "OpenGL debug callback attached: " << (khr ? "KHR" : "ARB");
        }
        else
#endif
            Log(Error) << "Unable to attach OpenGL debug callback.";
    }

    bool shouldDebugOpenGL()
    {
        const char* env = std::getenv("OPENMW_DEBUG_OPENGL");
        if (!env)
            return false;
        std::string str(env);
        if (str.length() == 0)
            return true;

        return str.find("OFF") == std::string::npos && str.find('0') == std::string::npos
            && str.find("NO") == std::string::npos;
    }

    DebugGroup::DebugGroup(const std::string& message, GLuint id)
#ifdef GL_DEBUG_OUTPUT
        : mSource(GL_DEBUG_SOURCE_APPLICATION)
#else
        : mSource(0x824A)
#endif
        , mId(id)
        , mMessage(message)
    {
    }

    DebugGroup::DebugGroup(const DebugGroup& debugGroup, const osg::CopyOp& copyop)
        : osg::StateAttribute(debugGroup, copyop)
        , mSource(debugGroup.mSource)
        , mId(debugGroup.mId)
        , mMessage(debugGroup.mMessage)
    {
    }

    void DebugGroup::apply(osg::State& state) const
    {
        if (!PushDebugGroup::sInstance->valid())
        {
            Log(Error) << "OpenGL debug groups not supported on this system, or OPENMW_DEBUG_OPENGL environment "
                          "variable not set.";
            return;
        }

        auto& attributeVec = state.getAttributeVec(this);
        auto& lastAppliedStack = sLastAppliedStack[state.getContextID()];

        size_t firstNonMatch = 0;
        while (firstNonMatch < lastAppliedStack.size()
            && ((firstNonMatch < attributeVec.size()
                    && lastAppliedStack[firstNonMatch] == attributeVec[firstNonMatch].first)
                || lastAppliedStack[firstNonMatch] == this))
            firstNonMatch++;

        for (size_t i = lastAppliedStack.size(); i > firstNonMatch; --i)
            lastAppliedStack[i - 1]->pop(state);
        lastAppliedStack.resize(firstNonMatch);

        lastAppliedStack.reserve(attributeVec.size());
        for (size_t i = firstNonMatch; i < attributeVec.size(); ++i)
        {
            const DebugGroup* group = static_cast<const DebugGroup*>(attributeVec[i].first);
            group->push(state);
            lastAppliedStack.push_back(group);
        }
        if (lastAppliedStack.empty() || !(lastAppliedStack.back() == this))
        {
            push(state);
            lastAppliedStack.push_back(this);
        }
    }

    int DebugGroup::compare(const StateAttribute& sa) const
    {
        COMPARE_StateAttribute_Types(DebugGroup, sa);

        COMPARE_StateAttribute_Parameter(mSource);
        COMPARE_StateAttribute_Parameter(mId);
        COMPARE_StateAttribute_Parameter(mMessage);

        return 0;
    }

    void DebugGroup::releaseGLObjects(osg::State* state) const
    {
        if (state)
            sLastAppliedStack.erase(state->getContextID());
        else
            sLastAppliedStack.clear();
    }

    bool DebugGroup::isValid() const
    {
        return mSource || mId || mMessage.length();
    }

    void DebugGroup::push(osg::State& state) const
    {
        if (isValid())
            PushDebugGroup::sInstance->glPushDebugGroup(
                mSource, mId, static_cast<GLsizei>(mMessage.size()), mMessage.c_str());
    }

    void DebugGroup::pop(osg::State& state) const
    {
        if (isValid())
            PushDebugGroup::sInstance->glPopDebugGroup();
    }

    std::map<unsigned int, std::vector<const DebugGroup*>> DebugGroup::sLastAppliedStack{};

}
