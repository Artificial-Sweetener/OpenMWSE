#include "groundcoverlod.hpp"
#include "groundcoverdistance.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <osg/Geometry>
#include <osg/Node>
#include <osg/NodeVisitor>

#include <components/esm3/loadland.hpp>

namespace MWRender
{
    GroundcoverLod::GroundcoverLod()
        : GroundcoverLod(new GroundcoverDistance(0.f))
    {
    }

    GroundcoverLod::GroundcoverLod(GroundcoverDistance* distance)
        : mDistance(distance)
    {
        if (!mDistance)
            throw std::invalid_argument("groundcover LOD requires a distance owner");
    }

    GroundcoverLod::GroundcoverLod(const GroundcoverLod& other, const osg::CopyOp& copy)
        : osg::Group(other, copy)
        , mDistance(other.mDistance)
    {
    }

    GroundcoverLod::~GroundcoverLod() = default;

    void GroundcoverLod::traverse(osg::NodeVisitor& visitor)
    {
        if (visitor.getTraversalMode() != osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN)
        {
            osg::Group::traverse(visitor);
            return;
        }
        const float distance = visitor.getDistanceToViewPoint(getBound().center(), true);
        const unsigned int child = distance < mDistance->getFarLodDistance() ? 0 : 1;
        if (child < getNumChildren())
            getChild(child)->accept(visitor);
    }

    namespace
    {
        /// Copy triangle indices from every supported primitive representation.
        std::vector<unsigned int> triangleIndices(const osg::PrimitiveSet& primitive)
        {
            std::vector<unsigned int> result;
            if (primitive.getMode() != GL_TRIANGLES)
                return result;

            if (const auto* arrays = dynamic_cast<const osg::DrawArrays*>(&primitive))
            {
                result.reserve(arrays->getCount());
                for (GLint index = arrays->getFirst(); index < arrays->getFirst() + arrays->getCount(); ++index)
                    result.push_back(static_cast<unsigned int>(index));
                return result;
            }

            const auto append = [&](const auto* elements) {
                if (elements)
                    result.assign(elements->begin(), elements->end());
            };
            append(dynamic_cast<const osg::DrawElementsUByte*>(&primitive));
            append(dynamic_cast<const osg::DrawElementsUShort*>(&primitive));
            append(dynamic_cast<const osg::DrawElementsUInt*>(&primitive));
            return result;
        }

        /// Estimate the source-space area covered by one adjacent triangle pair.
        float cardArea(const osg::Geometry& geometry, const std::vector<unsigned int>& indices, std::size_t card)
        {
            const osg::Array* sourceVertices = geometry.getVertexArray();
            const auto* vertices3 = dynamic_cast<const osg::Vec3Array*>(sourceVertices);
            const auto* vertices4 = dynamic_cast<const osg::Vec4Array*>(sourceVertices);
            if (!vertices3 && !vertices4)
                return 0.f;

            const auto vertex = [&](unsigned int index) {
                if (vertices3 && index < vertices3->size())
                    return (*vertices3)[index];
                if (vertices4 && index < vertices4->size())
                {
                    const osg::Vec4f& value = (*vertices4)[index];
                    return osg::Vec3f(value.x(), value.y(), value.z());
                }
                return osg::Vec3f();
            };

            constexpr std::size_t trianglesPerCard = 2;
            float result = 0.f;
            const std::size_t firstTriangle = card * trianglesPerCard;
            const std::size_t triangleCount = indices.size() / 3;
            const std::size_t endTriangle = std::min(firstTriangle + trianglesPerCard, triangleCount);
            for (std::size_t triangle = firstTriangle; triangle < endTriangle; ++triangle)
            {
                const std::size_t firstIndex = triangle * 3;
                const osg::Vec3f a = vertex(indices[firstIndex]);
                const osg::Vec3f b = vertex(indices[firstIndex + 1]);
                const osg::Vec3f c = vertex(indices[firstIndex + 2]);
                result += ((b - a) ^ (c - a)).length() * 0.5f;
            }
            return result;
        }

        /// Return whether two adjacent triangles form one complete four-vertex card.
        bool isCard(const std::vector<unsigned int>& indices, std::size_t card)
        {
            constexpr std::size_t indicesPerCard = 6;
            const std::size_t first = card * indicesPerCard;
            if (first + indicesPerCard > indices.size())
                return false;

            std::vector<unsigned int> unique(indices.begin() + static_cast<std::ptrdiff_t>(first),
                indices.begin() + static_cast<std::ptrdiff_t>(first + indicesPerCard));
            std::sort(unique.begin(), unique.end());
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
            return unique.size() == 4;
        }

        /// Select one high-coverage complete card from each ordered region of the source primitive.
        osg::ref_ptr<osg::PrimitiveSet> reduceTriangles(
            const osg::Geometry& geometry, const osg::PrimitiveSet& source, float triangleRatio)
        {
            const std::vector<unsigned int> indices = triangleIndices(source);
            const std::size_t triangleCount = indices.size() / 3;
            if (triangleCount < 4)
                return nullptr;

            constexpr std::size_t trianglesPerCard = 2;
            const std::size_t cardCount = (triangleCount + trianglesPerCard - 1) / trianglesPerCard;
            if (triangleCount % trianglesPerCard != 0)
                return nullptr;
            for (std::size_t card = 0; card < cardCount; ++card)
            {
                if (!isCard(indices, card))
                    return nullptr;
            }

            constexpr std::size_t minimumFarCards = 2;
            const std::size_t retainedCards = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::ceil(static_cast<float>(cardCount) * triangleRatio)),
                std::min(minimumFarCards, cardCount), cardCount);
            if (retainedCards == cardCount)
                return nullptr;

            osg::ref_ptr<osg::DrawElementsUInt> reduced = new osg::DrawElementsUInt(GL_TRIANGLES);
            reduced->setName(source.getName());
            reduced->setDataVariance(source.getDataVariance());
            reduced->setNumInstances(source.getNumInstances());
            reduced->reserve(retainedCards * trianglesPerCard * 3);
            for (std::size_t retained = 0; retained < retainedCards; ++retained)
            {
                const std::size_t firstCard = retained * cardCount / retainedCards;
                const std::size_t endCard = (retained + 1) * cardCount / retainedCards;
                std::size_t card = firstCard;
                float largestArea = cardArea(geometry, indices, card);
                for (std::size_t candidate = firstCard + 1; candidate < endCard; ++candidate)
                {
                    const float area = cardArea(geometry, indices, candidate);
                    if (area > largestArea)
                    {
                        largestArea = area;
                        card = candidate;
                    }
                }
                const std::size_t firstTriangle = card * trianglesPerCard;
                const std::size_t endTriangle = std::min(firstTriangle + trianglesPerCard, triangleCount);
                for (std::size_t triangle = firstTriangle; triangle < endTriangle; ++triangle)
                {
                    const auto firstIndex = indices.begin() + static_cast<std::ptrdiff_t>(triangle * 3);
                    reduced->insert(reduced->end(), firstIndex, firstIndex + 3);
                }
            }
            return reduced;
        }

        /// Replace supported triangle primitives throughout one independently cloned far model.
        class GroundcoverFarLodVisitor final : public osg::NodeVisitor
        {
        public:
            /// Retain the requested spatial triangle sampling ratio.
            explicit GroundcoverFarLodVisitor(float triangleRatio)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTriangleRatio(triangleRatio)
            {
            }

            /// Reduce each supported triangle primitive without changing its vertex attributes or render state.
            void apply(osg::Geometry& geometry) override
            {
                for (unsigned int index = 0; index < geometry.getNumPrimitiveSets(); ++index)
                {
                    if (osg::ref_ptr<osg::PrimitiveSet> reduced
                        = reduceTriangles(geometry, *geometry.getPrimitiveSet(index), mTriangleRatio))
                    {
                        geometry.setPrimitiveSet(index, reduced);
                    }
                }
            }

        private:
            float mTriangleRatio;
        };
    }

    float getGroundcoverFarLodDistance(float viewDistance)
    {
        if (!std::isfinite(viewDistance) || viewDistance < 0.f)
            throw std::invalid_argument("groundcover view distance must be finite and non-negative");

        constexpr float minimumFarDistanceInCells = 6.f;
        return std::min(viewDistance, std::max(viewDistance * 0.5f, ESM::Land::REAL_SIZE * minimumFarDistanceInCells));
    }

    osg::ref_ptr<osg::Node> makeGroundcoverFarLod(const osg::Node& detailedModel, float triangleRatio)
    {
        if (!std::isfinite(triangleRatio) || triangleRatio <= 0.f || triangleRatio > 1.f)
            throw std::invalid_argument("groundcover far LOD triangle ratio must be within (0, 1]");

        osg::ref_ptr<osg::Node> result = static_cast<osg::Node*>(
            detailedModel.clone(osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
                | osg::CopyOp::DEEP_COPY_USERDATA | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES));
        GroundcoverFarLodVisitor visitor(triangleRatio);
        result->accept(visitor);
        return result;
    }
}
