#include "shadowmapdiagnostics.hpp"

#include "renderdiagnosticcapture.hpp"

#include <components/sceneutil/diagnosticshadowwork.hpp>

#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Texture2D>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace MWRender
{
    namespace
    {
        /// Serialize one matrix in row-major declaration order for unambiguous replay analysis.
        void writeMatrix(std::ostream& output, std::string_view name, const osg::Matrixd& matrix)
        {
            output << "    " << name << ": [";
            for (unsigned row = 0; row < 4; ++row)
            {
                for (unsigned column = 0; column < 4; ++column)
                {
                    if (row != 0 || column != 0)
                        output << ", ";
                    output << matrix(row, column);
                }
            }
            output << "]\n";
        }
    }

    ShadowMapDiagnosticCapture captureShadowMapDiagnostics(
        RenderDiagnosticCapture& capture, osg::RenderInfo& renderInfo, unsigned maximumDimension)
    {
        osg::State* state = renderInfo.getState();
        if (state == nullptr || state->getFrameStamp() == nullptr)
            return {};

        const unsigned frame = state->getFrameStamp()->getFrameNumber();
        const unsigned context = state->getContextID();
        std::vector<SceneUtil::DiagnosticShadowMap> maps = SceneUtil::diagnosticShadowMaps(context, frame);
        std::ranges::sort(maps, {}, &SceneUtil::DiagnosticShadowMap::mCascade);

        ShadowMapDiagnosticCapture result;
        std::ostringstream metadata;
        metadata << std::setprecision(17) << "shadow-map-diagnostics-version: 1\n"
                 << "frame: " << frame << "\ncontext: " << context << "\nmaps:\n";
        for (std::size_t index = 0; index < maps.size(); ++index)
        {
            const SceneUtil::DiagnosticShadowMap& map = maps[index];
            const std::string name
                = "shadow-map-depth-cascade-" + std::to_string(map.mCascade) + "-" + std::to_string(index);
            if (auto image = capture.captureDepthTexture(map.mTexture, name, maximumDimension))
                result.mImages.emplace_back(std::move(*image));

            metadata << "  - cascade: " << map.mCascade << "\n"
                     << "    source-size: [" << map.mTexture->getTextureWidth() << ", "
                     << map.mTexture->getTextureHeight() << "]\n"
                     << "    cascade-range: [" << map.mCascadeNear << ", " << map.mCascadeFar << "]\n"
                     << "    receiver-crop: [";
            for (std::size_t bound = 0; bound < map.mReceiverCrop.size(); ++bound)
            {
                if (bound != 0)
                    metadata << ", ";
                metadata << map.mReceiverCrop[bound];
            }
            metadata << "]\n";
            writeMatrix(metadata, "view-matrix", map.mViewMatrix);
            writeMatrix(metadata, "projection-matrix", map.mProjectionMatrix);
            writeMatrix(metadata, "view-projection-matrix", map.mViewMatrix * map.mProjectionMatrix);
        }
        result.mMetadata = metadata.str();
        return result;
    }

    void writeShadowMapDiagnosticMetadata(const std::filesystem::path& path, std::string_view metadata)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("failed opening shadow-map diagnostic metadata");
        output.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
        if (!output)
            throw std::runtime_error("failed writing shadow-map diagnostic metadata");
    }

    ShadowMapDiagnosticMetadataWriter::ShadowMapDiagnosticMetadataWriter(const std::filesystem::path& path)
        : mPath(path)
        , mPartialPath(path.native() + std::filesystem::path(".partial").native())
    {
        if (std::filesystem::exists(mPath) || std::filesystem::exists(mPartialPath))
            throw std::runtime_error("shadow-map diagnostic metadata output already exists");
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        mStream.open(mPartialPath, std::ios::binary | std::ios::trunc);
        if (!mStream)
            throw std::runtime_error("failed opening shadow-map diagnostic metadata stream");
        mStream << "shadow-route-diagnostics-version: 1\nframes:\n";
        if (!mStream)
        {
            mStream.close();
            std::error_code error;
            std::filesystem::remove(mPartialPath, error);
            throw std::runtime_error("failed initializing shadow-map diagnostic metadata stream");
        }
    }

    ShadowMapDiagnosticMetadataWriter::~ShadowMapDiagnosticMetadataWriter()
    {
        mStream.close();
        if (!mPublished)
        {
            std::error_code error;
            std::filesystem::remove(mPartialPath, error);
        }
    }

    void ShadowMapDiagnosticMetadataWriter::append(
        std::size_t routeOrdinal, std::uint64_t renderFrame, std::string_view metadata)
    {
        if (mPublished)
            throw std::logic_error("shadow-map diagnostic metadata is already published");
        mStream << "  - route-ordinal: " << routeOrdinal << "\n    render-frame: " << renderFrame << "\n    capture:\n";
        std::size_t offset = 0;
        while (offset < metadata.size())
        {
            const std::size_t end = metadata.find('\n', offset);
            const std::size_t length = end == std::string_view::npos ? metadata.size() - offset : end - offset;
            mStream << "      " << metadata.substr(offset, length) << '\n';
            if (end == std::string_view::npos)
                break;
            offset = end + 1;
        }
        if (!mStream)
            throw std::runtime_error("failed appending shadow-map diagnostic metadata");
    }

    void ShadowMapDiagnosticMetadataWriter::finish()
    {
        if (mPublished)
            throw std::logic_error("shadow-map diagnostic metadata is already published");
        mStream.close();
        if (!mStream)
            throw std::runtime_error("failed closing shadow-map diagnostic metadata");
        std::filesystem::rename(mPartialPath, mPath);
        mPublished = true;
    }
}
