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
}
