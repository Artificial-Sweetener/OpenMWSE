#ifndef OPENMW_COMPONENTS_FX_TECHNIQUE_HPP
#define OPENMW_COMPONENTS_FX_TECHNIQUE_HPP

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <osg/BlendEquation>
#include <osg/BlendFunc>
#include <osg/FrameBufferObject>
#include <osg/StateSet>
#include <osg/Texture2D>

#include <components/vfs/pathutil.hpp>

#include "lexer.hpp"
#include "pass.hpp"
#include "types.hpp"

namespace Resource
{
    class ImageManager;
}

namespace VFS
{
    class Manager;
}

namespace Fx
{
    using FlagsType = size_t;

    /// Own a frame-local post-processing dispatch graph and its render-target bindings.
    struct DispatchNode
    {
        /// Create an empty dispatch graph.
        DispatchNode() = default;

        /// Deep-copy render targets and reconnect copied subpass inputs to the copied textures.
        DispatchNode(const DispatchNode& other, const osg::CopyOp& copyOp = osg::CopyOp::SHALLOW_COPY)
            : mHandle(other.mHandle)
            , mFlags(other.mFlags)
            , mAlwaysEnabled(other.mAlwaysEnabled)
            , mRootStateSet(new osg::StateSet(*other.mRootStateSet, copyOp))
        {
            mPasses.reserve(other.mPasses.size());

            for (const auto& subpass : other.mPasses)
                mPasses.emplace_back(subpass, copyOp);

            for (auto& subpass : mPasses)
            {
                for (std::size_t inputIndex = 0; inputIndex < subpass.mInputTargetNames.size(); ++inputIndex)
                {
                    if (subpass.mInputTargetNames[inputIndex].empty())
                        continue;
                    const auto source = std::find_if(mPasses.begin(), mPasses.end(), [&](const auto& candidate) {
                        return candidate.findOutput(subpass.mInputTargetNames[inputIndex]) != nullptr;
                    });
                    if (source != mPasses.end())
                        subpass.mStateSet->setTextureAttribute(subpass.mInputTextureUnits[inputIndex],
                            source->findOutput(subpass.mInputTargetNames[inputIndex]));
                }
            }
        }

        /// Transfer ownership of a dispatch graph without rebinding its internal textures.
        DispatchNode(DispatchNode&&) noexcept = default;

        /// Destroy the owned frame-local dispatch resources.
        ~DispatchNode() = default;

        /// Replace this graph with a deep copy while preserving strong exception safety.
        DispatchNode& operator=(const DispatchNode& other)
        {
            if (this == &other)
                return *this;
            DispatchNode copy(other);
            *this = std::move(copy);
            return *this;
        }

        /// Transfer-assign a complete frame-local dispatch graph.
        DispatchNode& operator=(DispatchNode&&) noexcept = default;

        /// Own one compiled pass, up to two simultaneous outputs, and three named input bindings.
        struct SubPass
        {
            /// Create an uncompiled subpass with an independent state set.
            SubPass() = default;

            osg::ref_ptr<osg::StateSet> mStateSet = new osg::StateSet;
            osg::ref_ptr<osg::FrameBufferObject> mRenderTarget;
            osg::ref_ptr<osg::Texture2D> mRenderTexture;
            std::string mTargetName;
            osg::ref_ptr<osg::Texture2D> mSecondaryTexture;
            std::string mSecondaryTargetName;
            std::array<std::string, 3> mInputTargetNames;
            std::array<int, 3> mInputTextureUnits{ -1, -1, -1 };
            bool mResolve = false;
            Types::SizeProxy mSize;
            bool mMipMap = false;

            /// Locate a named output in this frame-local pass, including its secondary attachment.
            osg::Texture2D* findOutput(std::string_view name) const
            {
                if (name.empty())
                    return nullptr;
                if (name == mTargetName)
                    return mRenderTexture;
                return name == mSecondaryTargetName ? mSecondaryTexture.get() : nullptr;
            }

            /// Deep-copy a subpass and attach its copied texture to a new framebuffer.
            SubPass(const SubPass& other, const osg::CopyOp& copyOp = osg::CopyOp::SHALLOW_COPY)
                : mStateSet(new osg::StateSet(*other.mStateSet, copyOp))
                , mTargetName(other.mTargetName)
                , mSecondaryTargetName(other.mSecondaryTargetName)
                , mInputTargetNames(other.mInputTargetNames)
                , mInputTextureUnits(other.mInputTextureUnits)
                , mResolve(other.mResolve)
                , mSize(other.mSize)
                , mMipMap(other.mMipMap)
            {
                if (other.mRenderTexture)
                    mRenderTexture = new osg::Texture2D(*other.mRenderTexture, copyOp);
                if (other.mSecondaryTexture)
                    mSecondaryTexture = new osg::Texture2D(*other.mSecondaryTexture, copyOp);
                if (other.mRenderTarget)
                {
                    mRenderTarget = new osg::FrameBufferObject;
                    mRenderTarget->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0,
                        osg::FrameBufferAttachment(mRenderTexture));
                    if (mSecondaryTexture)
                        mRenderTarget->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER1,
                            osg::FrameBufferAttachment(mSecondaryTexture));
                }
            }

            /// Destroy the owned pass state and render-target resources.
            ~SubPass() = default;

            /// Copy-assign subpass state using OpenSceneGraph reference ownership.
            SubPass& operator=(const SubPass&) = default;
        };

        /// Mark the last screen-writing subpass as the dispatch resolve operation.
        void compile()
        {
            for (auto rit = mPasses.rbegin(); rit != mPasses.rend(); ++rit)
            {
                if (!rit->mRenderTarget)
                {
                    rit->mResolve = true;
                    break;
                }
            }
        }

        // not safe to read/write in draw thread
        std::shared_ptr<Fx::Technique> mHandle = nullptr;

        FlagsType mFlags = 0;

        bool mAlwaysEnabled = false;

        std::vector<SubPass> mPasses;

        osg::ref_ptr<osg::StateSet> mRootStateSet = new osg::StateSet;
    };

    using DispatchArray = std::vector<DispatchNode>;

    class Technique
    {
    public:
        using PassList = std::vector<std::shared_ptr<Pass>>;
        using TexList = std::vector<osg::ref_ptr<osg::Texture>>;

        using UniformMap = std::vector<std::shared_ptr<Types::UniformBase>>;
        using RenderTargetMap = std::unordered_map<std::string_view, Types::RenderTarget>;

        static constexpr std::string_view sExt = "omwfx";
        static constexpr VFS::Path::NormalizedView sSubdir{ "shaders" };

        enum class Status
        {
            Success,
            Uncompiled,
            File_Not_exists,
            Parse_Error
        };

        static constexpr FlagsType Flag_Disable_Interiors = (1 << 0);
        static constexpr FlagsType Flag_Disable_Exteriors = (1 << 1);
        static constexpr FlagsType Flag_Disable_Underwater = (1 << 2);
        static constexpr FlagsType Flag_Disable_Abovewater = (1 << 3);
        static constexpr FlagsType Flag_Disable_SunGlare = (1 << 4);
        static constexpr FlagsType Flag_Hidden = (1 << 5);

        static VFS::Path::Normalized makeFileName(std::string_view name);

        Technique(const VFS::Manager& vfs, Resource::ImageManager& imageManager, VFS::Path::NormalizedView fileName,
            std::string name, int width, int height, bool ubo, bool supportsNormals);

        /// Compile the pass graph and validate named outputs, dimensions and simultaneous-input hazards.
        bool compile();

        std::string getName() const;

        const VFS::Path::Normalized& getFileName() const { return mFilePath; }

        bool setLastModificationTime(std::filesystem::file_time_type timeStamp);

        bool isValid() const { return mValid; }

        bool getHDR() const { return mHDR; }

        bool getNormals() const { return mNormals && mSupportsNormals; }

        bool getLights() const { return mLights; }

        const PassList& getPasses() { return mPasses; }

        const TexList& getTextures() const { return mTextures; }

        Status getStatus() const { return mStatus; }

        std::string_view getAuthor() const { return mAuthor; }

        std::string_view getDescription() const { return mDescription; }

        std::string_view getVersion() const { return mVersion; }

        int getGLSLVersion() const { return mGLSLVersion; }

        std::string getGLSLProfile() const { return mGLSLProfile; }

        const std::unordered_set<std::string>& getGLSLExtensions() const { return mGLSLExtensions; }

        FlagsType getFlags() const { return mFlags; }

        bool getHidden() const { return mFlags & Flag_Hidden; }

        UniformMap& getUniformMap() { return mDefinedUniforms; }

        RenderTargetMap& getRenderTargetsMap() { return mRenderTargets; }

        std::string getLastError() const { return mLastError; }

        UniformMap::iterator findUniform(std::string_view name);

        bool getDynamic() const { return mDynamic; }

        void setLocked(bool locked) { mLocked = locked; }
        bool getLocked() const { return mLocked; }

        void setInternal(bool internal) { mInternal = internal; }
        bool getInternal() const { return mInternal; }

        /// Keep an internal technique active independently of the user post-processing toggle.
        void setAlwaysEnabled(bool alwaysEnabled) { mAlwaysEnabled = alwaysEnabled; }
        /// Return whether this internal technique bypasses the user post-processing toggle.
        bool getAlwaysEnabled() const { return mAlwaysEnabled; }

    private:
        [[noreturn]] void error(std::string_view msg);

        void clear();

        std::string_view asLiteral() const;

        template <class T>
        void expect(std::string_view err = {});

        template <class T, class T2>
        void expect(std::string_view err = {});

        template <class T>
        bool isNext();

        void parse(std::string&& buffer);

        template <class SrcT, class T>
        void parseUniform();

        template <class T>
        void parseSampler();

        template <class T>
        void parseBlock(bool named = true);

        template <class T>
        void parseBlockImp()
        {
        }

        /// Parse pass output, input and blending declarations before the shader body.
        void parseBlockHeader();

        bool parseBool();

        std::string_view parseString();

        float parseFloat();

        int parseInteger();

        int parseInternalFormat();

        int parseSourceType();

        int parseSourceFormat();

        template <class SrcT, class T>
        void parseWidgetType(Types::Uniform<SrcT>& uniform);

        template <class SrcT, class T>
        SrcT getUniformValue();

        osg::BlendEquation::Equation parseBlendEquation();

        osg::BlendFunc::BlendFuncMode parseBlendFuncMode();

        osg::Texture::WrapMode parseWrapMode();

        osg::Texture::InternalFormatMode parseCompression();

        FlagsType parseFlags();

        osg::Texture::FilterMode parseFilterMode();

        template <class TDelimeter>
        std::vector<std::string_view> parseLiteralList();

        template <class OSGVec, class T>
        OSGVec parseVec();

        std::string getBlockWithLineDirective();

        std::unique_ptr<Lexer::Lexer> mLexer;
        Lexer::Token mToken;

        std::string mShared;
        std::string mName;
        VFS::Path::Normalized mFilePath;
        std::string_view mBlockName;
        std::string_view mAuthor;
        std::string_view mDescription;
        std::string_view mVersion;

        std::unordered_set<std::string> mGLSLExtensions;
        int mGLSLVersion;
        std::string mGLSLProfile;

        FlagsType mFlags;

        Status mStatus;

        bool mEnabled;

        std::filesystem::file_time_type mLastModificationTime;
        bool mValid;
        bool mHDR;
        bool mNormals;
        bool mLights;
        int mWidth;
        int mHeight;

        RenderTargetMap mRenderTargets;

        TexList mTextures;
        PassList mPasses;

        std::unordered_map<std::string_view, std::shared_ptr<Pass>> mPassMap;
        std::vector<std::string_view> mPassKeys;

        Pass::Type mLastAppliedType;

        UniformMap mDefinedUniforms;

        const VFS::Manager& mVFS;
        Resource::ImageManager& mImageManager;
        bool mUBO;
        bool mSupportsNormals;

        std::string mBuffer;

        std::string mLastError;

        bool mDynamic = false;
        bool mLocked = false;
        bool mInternal = false;
        bool mAlwaysEnabled = false;
    };

    template <>
    void Technique::parseBlockImp<Lexer::Shared>();
    template <>
    void Technique::parseBlockImp<Lexer::Technique>();
    template <>
    void Technique::parseBlockImp<Lexer::Render_Target>();
    template <>
    void Technique::parseBlockImp<Lexer::Vertex>();
    template <>
    void Technique::parseBlockImp<Lexer::Fragment>();
    template <>
    void Technique::parseBlockImp<Lexer::Compute>();
    template <>
    void Technique::parseBlockImp<Lexer::Sampler_1D>();
    template <>
    void Technique::parseBlockImp<Lexer::Sampler_2D>();
    template <>
    void Technique::parseBlockImp<Lexer::Sampler_3D>();
    template <>
    void Technique::parseBlockImp<Lexer::Uniform_Bool>();
    template <>
    void Technique::parseBlockImp<Lexer::Uniform_Float>();
    template <>
    void Technique::parseBlockImp<Lexer::Uniform_Int>();
    template <>
    void Technique::parseBlockImp<Lexer::Uniform_Vec2>();
    template <>
    void Technique::parseBlockImp<Lexer::Uniform_Vec3>();
    template <>
    void Technique::parseBlockImp<Lexer::Uniform_Vec4>();
}

#endif
