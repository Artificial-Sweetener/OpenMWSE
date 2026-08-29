#include "renderdiagnosticbundle.hpp"

#include <array>
#include <fstream>
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

    void writeRenderDiagnosticBundle(
        const std::filesystem::path& path, std::uint64_t frameNumber, std::span<const RenderDiagnosticImage> images)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("failed opening render diagnostic bundle");

        constexpr std::array<char, 8> magic{ 'O', 'M', 'W', 'A', 'T', 'M', 'D', '1' };
        stream.write(magic.data(), magic.size());
        writeValue(stream, renderDiagnosticBundleVersion);
        writeValue(stream, frameNumber);
        writeValue(stream, static_cast<std::uint32_t>(images.size()));
        for (const RenderDiagnosticImage& image : images)
        {
            writeString(stream, image.mName);
            writeValue(stream, image.mWidth);
            writeValue(stream, image.mHeight);
            writeValue(stream, image.mChannels);
            const std::uint64_t valueCount = image.mPixels.size();
            writeValue(stream, valueCount);
            stream.write(reinterpret_cast<const char*>(image.mPixels.data()),
                static_cast<std::streamsize>(valueCount * sizeof(float)));
            if (!stream)
                throw std::runtime_error("failed writing render diagnostic pixels");
        }
    }
}
