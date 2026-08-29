#include "groundcoverinstances.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>

#include <osg/BoundingSphere>

namespace
{
    /// Identify one two-dimensional world-space groundcover cluster.
    struct ClusterKey
    {
        std::int64_t mX;
        std::int64_t mY;

        /// Order keys consistently for deterministic first-seen cluster lookup.
        bool operator<(const ClusterKey& other) const { return std::tie(mX, mY) < std::tie(other.mX, other.mY); }
    };

    /// Map one finite world-space position to its spatial-cluster key.
    std::optional<ClusterKey> getClusterKey(const ESM::Position& position, float clusterSize)
    {
        const double x = std::floor(static_cast<double>(position.pos[0]) / clusterSize);
        const double y = std::floor(static_cast<double>(position.pos[1]) / clusterSize);
        constexpr double minIndex = static_cast<double>(std::numeric_limits<std::int64_t>::min());
        constexpr double maxIndex = static_cast<double>(std::numeric_limits<std::int64_t>::max());
        if (!std::isfinite(x) || !std::isfinite(y) || x < minIndex || x >= maxIndex || y < minIndex || y >= maxIndex)
            return std::nullopt;
        return ClusterKey{ static_cast<std::int64_t>(x), static_cast<std::int64_t>(y) };
    }
}

namespace MWRender
{
    GroundcoverInstanceLayout makeGroundcoverInstanceLayout(
        std::span<const Groundcover::GroundcoverEntry> instances, float clusterSize)
    {
        if (!std::isfinite(clusterSize) || clusterSize <= 0.f)
            throw std::invalid_argument("groundcover cluster size must be finite and positive");

        std::vector<std::vector<std::size_t>> sourceClusters;
        std::map<ClusterKey, std::size_t> clusterIndices;
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            const std::optional<ClusterKey> key = getClusterKey(instances[index].mPos, clusterSize);
            if (!key)
            {
                sourceClusters.push_back({ index });
                continue;
            }
            const auto [it, inserted] = clusterIndices.try_emplace(*key, sourceClusters.size());
            if (inserted)
                sourceClusters.emplace_back();
            sourceClusters[it->second].push_back(index);
        }

        GroundcoverInstanceLayout result;
        result.mSourceIndices.reserve(instances.size());
        result.mClusters.reserve(sourceClusters.size());
        for (const std::vector<std::size_t>& cluster : sourceClusters)
        {
            result.mClusters.push_back({ result.mSourceIndices.size(), cluster.size() });
            result.mSourceIndices.insert(result.mSourceIndices.end(), cluster.begin(), cluster.end());
        }
        return result;
    }

    GroundcoverInstanceBatch makeGroundcoverInstanceBatch(std::span<const Groundcover::GroundcoverEntry> instances,
        const GroundcoverInstanceLayout& layout, const osg::Vec3f& batchPosition, float modelExtent)
    {
        if (!std::isfinite(modelExtent) || modelExtent < 0.f)
            throw std::invalid_argument("groundcover model extent must be finite and non-negative");
        if (layout.mSourceIndices.size() != instances.size())
            throw std::invalid_argument("groundcover instance layout must contain every source instance");

        GroundcoverInstanceBatch result;
        result.mTransforms = new osg::Vec4Array(static_cast<unsigned int>(layout.mSourceIndices.size()));
        result.mRotations = new osg::Vec3Array(static_cast<unsigned int>(layout.mSourceIndices.size()));
        result.mClusters.reserve(layout.mClusters.size());
        std::vector<bool> sourceIndicesSeen(instances.size(), false);
        std::size_t expectedFirst = 0;
        for (const GroundcoverInstanceRange& cluster : layout.mClusters)
        {
            if (cluster.mFirst != expectedFirst || cluster.mFirst > layout.mSourceIndices.size()
                || cluster.mCount > layout.mSourceIndices.size() - cluster.mFirst)
                throw std::invalid_argument("groundcover instance clusters must form one contiguous layout");
            expectedFirst += cluster.mCount;

            osg::BoundingBox clusterBounds;
            for (std::size_t orderedIndex = cluster.mFirst; orderedIndex < cluster.mFirst + cluster.mCount;
                ++orderedIndex)
            {
                const std::size_t sourceIndex = layout.mSourceIndices[orderedIndex];
                if (sourceIndex >= instances.size() || sourceIndicesSeen[sourceIndex])
                    throw std::invalid_argument("groundcover instance layout must contain each source index once");
                sourceIndicesSeen[sourceIndex] = true;
                const Groundcover::GroundcoverEntry& instance = instances[sourceIndex];
                const osg::Vec3f relativePosition = instance.mPos.asVec3() - batchPosition;
                (*result.mTransforms)[orderedIndex] = osg::Vec4f(relativePosition, instance.mScale);
                (*result.mRotations)[orderedIndex] = instance.mPos.asRotationVec3();

                const float animationSafeRadius = modelExtent * instance.mScale * 1.1f;
                const osg::BoundingSphere instanceBounds(relativePosition, animationSafeRadius);
                clusterBounds.expandBy(instanceBounds);
                result.mBounds.expandBy(instanceBounds);
            }
            result.mClusters.push_back({ cluster, clusterBounds });
        }
        if (expectedFirst != layout.mSourceIndices.size())
            throw std::invalid_argument("groundcover instance clusters must cover the complete layout");
        return result;
    }
}
