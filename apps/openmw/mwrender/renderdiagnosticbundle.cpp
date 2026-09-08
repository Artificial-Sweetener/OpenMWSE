#include "renderdiagnosticbundle.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace MWRender
{
    namespace
    {
        /// Append a trivially copyable value to the versioned graphics-provider stream.
        template <class T>
        void writeValue(std::ofstream& stream, const T& value)
        {
            stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
            if (!stream)
                throw std::runtime_error("failed writing render diagnostic value");
        }

        /// Append a length-prefixed UTF-8 label to the versioned graphics-provider stream.
        void writeString(std::ofstream& stream, std::string_view value)
        {
            const auto size = static_cast<std::uint32_t>(value.size());
            writeValue(stream, size);
            stream.write(value.data(), static_cast<std::streamsize>(value.size()));
            if (!stream)
                throw std::runtime_error("failed writing render diagnostic label");
        }
    }

    RenderDiagnosticBundleWriter::RenderDiagnosticBundleWriter(const std::filesystem::path& path)
        : mPath(path)
        , mPartialPath(path.native() + std::filesystem::path(".partial").native())
    {
        if (std::filesystem::exists(mPath) || std::filesystem::exists(mPartialPath))
            throw std::runtime_error("render diagnostic output already exists");
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        mStream.open(mPartialPath, std::ios::binary | std::ios::trunc);
        if (!mStream)
            throw std::runtime_error("failed opening render diagnostic bundle");
        try
        {
            constexpr std::array<char, 8> magic{ 'O', 'M', 'W', 'A', 'T', 'M', 'D', '1' };
            mStream.write(magic.data(), magic.size());
            writeValue(mStream, renderDiagnosticBundleVersion);
            writeValue(mStream, std::uint64_t(0));
            writeValue(mStream, std::uint32_t(0));
        }
        catch (...)
        {
            mStream.close();
            std::error_code error;
            std::filesystem::remove(mPartialPath, error);
            throw;
        }
    }

    RenderDiagnosticBundleWriter::~RenderDiagnosticBundleWriter()
    {
        mStream.close();
        if (!mPublished)
        {
            std::error_code error;
            std::filesystem::remove(mPartialPath, error);
        }
    }

    void RenderDiagnosticBundleWriter::append(std::span<const RenderDiagnosticImage> images)
    {
        if (mPublished)
            throw std::logic_error("render diagnostic bundle is already published");
        if (images.size() > std::numeric_limits<std::uint32_t>::max() - mImageCount)
            throw std::invalid_argument("render diagnostic attachment count exceeds the bundle format");
        for (const RenderDiagnosticImage& image : images)
        {
            if (image.mName.size() > std::numeric_limits<std::uint32_t>::max() || image.mWidth == 0
                || image.mHeight == 0 || image.mChannels == 0
                || static_cast<std::uint64_t>(image.mWidth) * image.mHeight != image.mPixels.size() / image.mChannels
                || image.mPixels.size() % image.mChannels != 0)
                throw std::invalid_argument("render diagnostic attachment dimensions do not match its pixels");
        }
        for (const RenderDiagnosticImage& image : images)
        {
            writeString(mStream, image.mName);
            writeValue(mStream, image.mWidth);
            writeValue(mStream, image.mHeight);
            writeValue(mStream, image.mChannels);
            const std::uint64_t valueCount = image.mPixels.size();
            writeValue(mStream, valueCount);
            mStream.write(reinterpret_cast<const char*>(image.mPixels.data()),
                static_cast<std::streamsize>(valueCount * sizeof(float)));
            if (!mStream)
                throw std::runtime_error("failed writing render diagnostic pixels");
        }
        mImageCount += static_cast<std::uint32_t>(images.size());
    }

    void RenderDiagnosticBundleWriter::finish(std::uint64_t frameNumber)
    {
        if (mPublished)
            throw std::logic_error("render diagnostic bundle is already published");
        mStream.seekp(12);
        writeValue(mStream, frameNumber);
        writeValue(mStream, mImageCount);
        mStream.close();
        if (!mStream)
            throw std::runtime_error("failed closing render diagnostic bundle");
        std::filesystem::rename(mPartialPath, mPath);
        mPublished = true;
    }

    void writeRenderDiagnosticBundle(
        const std::filesystem::path& path, std::uint64_t frameNumber, std::span<const RenderDiagnosticImage> images)
    {
        RenderDiagnosticBundleWriter writer(path);
        writer.append(images);
        writer.finish(frameNumber);
    }
}
