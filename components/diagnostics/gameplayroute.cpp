#include "gameplayroute.hpp"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace Diagnostics
{
    namespace
    {
        /// Reject values that cannot safely drive a deterministic engine frame.
        void validateFrame(const GameplayRouteFrame& frame)
        {
            if (!(frame.mDuration > 0.0 && frame.mDuration <= maximumGameplayRouteFrameDuration)
                || !std::isfinite(frame.mDuration))
                throw std::invalid_argument("gameplay route frame duration is outside (0, 0.2] seconds");
            if (frame.mCell.empty() || frame.mCell.size() > 512)
                throw std::invalid_argument("gameplay route frame cell identity is invalid");
            for (const auto& values : { frame.mPosition, frame.mRotation, frame.mCameraAngles })
            {
                for (const float value : values)
                {
                    if (!std::isfinite(value))
                        throw std::invalid_argument("gameplay route frame contains a non-finite transform");
                }
            }
        }

        /// Decode one required three-component transform.
        std::array<float, 3> decodeVector(const YAML::Node& node, std::string_view name)
        {
            if (!node.IsSequence() || node.size() != 3)
                throw std::invalid_argument("gameplay route has an invalid " + std::string(name));
            return { node[0].as<float>(), node[1].as<float>(), node[2].as<float>() };
        }
    }

    std::vector<std::byte> encodeGameplayRoute(std::span<const GameplayRouteFrame> frames)
    {
        if (frames.empty() || frames.size() > maximumGameplayRouteFrames)
            throw std::length_error("gameplay route frame count is outside its bounds");
        YAML::Emitter output;
        output << YAML::BeginMap << YAML::Key << "route-version" << YAML::Value << gameplayRouteVersion << YAML::Key
               << "frames" << YAML::Value << YAML::BeginSeq;
        for (const GameplayRouteFrame& frame : frames)
        {
            validateFrame(frame);
            output << YAML::Flow << YAML::BeginMap << YAML::Key << "dt" << YAML::Value << frame.mDuration << YAML::Key
                   << "cell" << YAML::Value << frame.mCell << YAML::Key << "position" << YAML::Value << YAML::Flow
                   << YAML::BeginSeq << frame.mPosition[0] << frame.mPosition[1] << frame.mPosition[2] << YAML::EndSeq
                   << YAML::Key << "rotation" << YAML::Value << YAML::Flow << YAML::BeginSeq << frame.mRotation[0]
                   << frame.mRotation[1] << frame.mRotation[2] << YAML::EndSeq << YAML::Key << "camera" << YAML::Value
                   << YAML::Flow << YAML::BeginSeq << frame.mCameraAngles[0] << frame.mCameraAngles[1]
                   << frame.mCameraAngles[2] << YAML::EndSeq << YAML::Key << "first-person" << YAML::Value
                   << frame.mFirstPerson << YAML::EndMap;
        }
        output << YAML::EndSeq << YAML::EndMap;
        if (!output.good())
            throw std::runtime_error("failed encoding gameplay route");
        const std::string text(output.c_str(), output.size());
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        return { begin, begin + text.size() };
    }

    std::vector<GameplayRouteFrame> decodeGameplayRoute(std::span<const std::byte> payload)
    {
        const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
        const YAML::Node root = YAML::Load(text);
        if (root["route-version"].as<unsigned>(0) != gameplayRouteVersion)
            throw std::invalid_argument("gameplay route version is not supported");
        const YAML::Node encodedFrames = root["frames"];
        if (!encodedFrames.IsSequence() || encodedFrames.size() == 0
            || encodedFrames.size() > maximumGameplayRouteFrames)
            throw std::length_error("gameplay route frame count is outside its bounds");
        std::vector<GameplayRouteFrame> frames;
        frames.reserve(encodedFrames.size());
        for (const YAML::Node& encoded : encodedFrames)
        {
            GameplayRouteFrame frame;
            frame.mDuration = encoded["dt"].as<double>();
            frame.mCell = encoded["cell"].as<std::string>();
            frame.mPosition = decodeVector(encoded["position"], "position");
            frame.mRotation = decodeVector(encoded["rotation"], "rotation");
            frame.mCameraAngles = decodeVector(encoded["camera"], "camera angles");
            frame.mFirstPerson = encoded["first-person"].as<bool>();
            validateFrame(frame);
            frames.push_back(std::move(frame));
        }
        return frames;
    }
}
