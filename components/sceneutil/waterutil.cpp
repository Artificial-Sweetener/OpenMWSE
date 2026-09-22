#include "waterutil.hpp"

#include <osg/Depth>
#include <osg/Geometry>
#include <osg/PrimitiveSet>
#include <osg/StateSet>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include <components/sceneutil/material.hpp>

#include "depth.hpp"

namespace SceneUtil
{
    float WaterSurfacePlane::signedDistance(const osg::Vec3f& point) const
    {
        return mNormal * point - mDistance;
    }

    osg::Vec2f WaterSurfacePlane::project(const osg::Vec3f& point) const
    {
        return osg::Vec2f(mTangent * point, mBitangent * point);
    }

    osg::Vec3f WaterSurfacePlane::unproject(const osg::Vec2f& point) const
    {
        return mTangent * point.x() + mBitangent * point.y() + mNormal * mDistance;
    }

    double WaterReflectionScreenRect::projectedDiameter(double viewportWidth, double viewportHeight) const
    {
        return std::max((mRight - mLeft) * viewportWidth, (mTop - mBottom) * viewportHeight);
    }

    WaterReflectionScreenRect waterReflectionScreenRect(
        const osg::BoundingBox& worldBound, const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix)
    {
        WaterReflectionScreenRect result{ 1.0, 1.0, 0.0, 0.0 };
        const osg::Matrixd worldToClip = viewMatrix * projectionMatrix;
        for (unsigned int corner = 0; corner < 8; ++corner)
        {
            const osg::Vec4d clip = osg::Vec4d(worldBound.corner(corner), 1.0) * worldToClip;
            if (clip.w() <= 0.000001)
                return {};

            const double inverseW = 1.0 / clip.w();
            const double x = clip.x() * inverseW * 0.5 + 0.5;
            const double y = clip.y() * inverseW * 0.5 + 0.5;
            result.mLeft = std::min(result.mLeft, x);
            result.mBottom = std::min(result.mBottom, y);
            result.mRight = std::max(result.mRight, x);
            result.mTop = std::max(result.mTop, y);
        }
        result.mLeft = std::clamp(result.mLeft, 0.0, 1.0);
        result.mBottom = std::clamp(result.mBottom, 0.0, 1.0);
        result.mRight = std::clamp(result.mRight, 0.0, 1.0);
        result.mTop = std::clamp(result.mTop, 0.0, 1.0);
        return result;
    }

    bool waterSurfaceLayersCoincide(
        const osg::BoundingBox& first, float firstHeight, const osg::BoundingBox& second, float secondHeight)
    {
        const auto near
            = [](float left, float right) { return std::abs(left - right) <= waterSurfaceInteractionTolerance; };
        return std::abs(firstHeight - secondHeight) <= waterMaterialLayerHeightTolerance
            && near(first.xMin(), second.xMin()) && near(first.xMax(), second.xMax())
            && near(first.yMin(), second.yMin()) && near(first.yMax(), second.yMax());
    }

    bool waterSurfaceTriangleContainsPoint(
        const osg::Vec2f& point, const osg::Vec2f& a, const osg::Vec2f& b, const osg::Vec2f& c)
    {
        const auto edge = [](const osg::Vec2f& start, const osg::Vec2f& end, const osg::Vec2f& sample) {
            return (sample.x() - start.x()) * (end.y() - start.y()) - (sample.y() - start.y()) * (end.x() - start.x());
        };
        constexpr float tolerance = 0.001f;
        const float ab = edge(a, b, point);
        const float bc = edge(b, c, point);
        const float ca = edge(c, a, point);
        const bool hasNegative = ab < -tolerance || bc < -tolerance || ca < -tolerance;
        const bool hasPositive = ab > tolerance || bc > tolerance || ca > tolerance;
        return !(hasNegative && hasPositive);
    }

    unsigned int waterTriangleSubdivisionCount(
        const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c, float maximumEdgeLength)
    {
        if (!(maximumEdgeLength > 0.f) || !std::isfinite(maximumEdgeLength))
            return 1;
        const float longestEdge = std::max({ (b - a).length(), (c - b).length(), (a - c).length() });
        constexpr unsigned int maximumDivisions = 128;
        return std::clamp(static_cast<unsigned int>(std::ceil(longestEdge / maximumEdgeLength)), 1u, maximumDivisions);
    }

    unsigned int waterReflectionResolution(double projectedDiameter, unsigned int maximumResolution)
    {
        const unsigned int boundedMaximum = std::max(64u, maximumResolution);
        const unsigned int requested = projectedDiameter >= 700.0 ? 512u : projectedDiameter >= 240.0 ? 256u : 128u;
        return std::min(requested, boundedMaximum);
    }

    bool waterReflectionVisibleAtFrame(unsigned int lastVisibleFrame, unsigned int currentFrame) noexcept
    {
        return currentFrame - lastVisibleFrame <= 1u;
    }

    std::optional<WaterSurfacePlane> fitWaterSurfacePlane(
        std::span<const osg::Vec3f> triangleVertices, float maximumDeviation, float minimumUpwardNormal)
    {
        if (triangleVertices.empty() || triangleVertices.size() % 3 != 0 || maximumDeviation < 0.f
            || minimumUpwardNormal < 0.f || minimumUpwardNormal > 1.f)
            return std::nullopt;

        osg::Vec3f referenceNormal;
        osg::Vec3f centroid;
        for (std::size_t index = 0; index < triangleVertices.size(); index += 3)
        {
            const osg::Vec3f& a = triangleVertices[index];
            const osg::Vec3f& b = triangleVertices[index + 1];
            const osg::Vec3f& c = triangleVertices[index + 2];
            const osg::Vec3f triangleNormal = (b - a) ^ (c - a);
            if (triangleNormal.length2() > referenceNormal.length2())
                referenceNormal = triangleNormal;
            centroid += a + b + c;
        }
        if (referenceNormal.length2() <= 0.000001f)
            return std::nullopt;
        osg::Vec3f normal;
        for (std::size_t index = 0; index < triangleVertices.size(); index += 3)
        {
            const osg::Vec3f triangleNormal = (triangleVertices[index + 1] - triangleVertices[index])
                ^ (triangleVertices[index + 2] - triangleVertices[index]);
            normal += triangleNormal * (triangleNormal * referenceNormal < 0.f ? -1.f : 1.f);
        }
        normal.normalize();
        if (normal.z() < 0.f)
            normal = -normal;
        if (normal.z() < minimumUpwardNormal)
            return std::nullopt;

        centroid /= static_cast<float>(triangleVertices.size());
        const float distance = normal * centroid;
        for (const osg::Vec3f& vertex : triangleVertices)
            if (std::abs(normal * vertex - distance) > maximumDeviation)
                return std::nullopt;

        osg::Vec3f tangent = osg::Vec3f(1.f, 0.f, 0.f) - normal * normal.x();
        if (tangent.length2() <= 0.000001f)
            tangent = osg::Vec3f(0.f, 1.f, 0.f) - normal * normal.y();
        tangent.normalize();
        osg::Vec3f bitangent = normal ^ tangent;
        bitangent.normalize();
        return WaterSurfacePlane{ normal, distance, tangent, bitangent };
    }

    WaterSurfacePartition partitionWaterSurfaceTriangles(std::span<const osg::Vec3f> triangleVertices,
        float maximumDeviation, float minimumUpwardNormal, float positionTolerance)
    {
        WaterSurfacePartition result;
        if (triangleVertices.empty() || triangleVertices.size() % 3 != 0 || maximumDeviation < 0.f
            || minimumUpwardNormal < 0.f || minimumUpwardNormal > 1.f || !(positionTolerance > 0.f))
            return result;

        const std::size_t triangleCount = triangleVertices.size() / 3;

        using PositionKey = std::array<int, 3>;
        const auto positionKey = [positionTolerance](const osg::Vec3f& point) {
            const float scale = 1.f / positionTolerance;
            return PositionKey{ static_cast<int>(std::lround(point.x() * scale)),
                static_cast<int>(std::lround(point.y() * scale)), static_cast<int>(std::lround(point.z() * scale)) };
        };

        std::vector<std::size_t> parents(triangleCount);
        std::iota(parents.begin(), parents.end(), 0);
        const auto findRoot = [&](std::size_t index) {
            while (parents[index] != index)
            {
                parents[index] = parents[parents[index]];
                index = parents[index];
            }
            return index;
        };
        std::map<PositionKey, std::vector<std::size_t>> trianglesByPosition;
        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            for (std::size_t vertex = 0; vertex < 3; ++vertex)
            {
                std::vector<std::size_t>& connected
                    = trianglesByPosition[positionKey(triangleVertices[triangle * 3 + vertex])];
                for (const std::size_t other : connected)
                {
                    const std::size_t otherRoot = findRoot(other);
                    const std::size_t triangleRoot = findRoot(triangle);
                    if (otherRoot != triangleRoot)
                        parents[triangleRoot] = otherRoot;
                }
                connected.push_back(triangle);
            }
        }

        std::map<std::size_t, std::vector<std::size_t>> components;
        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
            components[findRoot(triangle)].push_back(triangle);

        const auto componentVertices = [&](const std::vector<std::size_t>& component) {
            std::vector<osg::Vec3f> vertices;
            vertices.reserve(component.size() * 3);
            for (const std::size_t triangle : component)
                vertices.insert(vertices.end(), triangleVertices.begin() + triangle * 3,
                    triangleVertices.begin() + triangle * 3 + 3);
            return vertices;
        };
        for (const auto& [_, component] : components)
        {
            const std::vector<osg::Vec3f> vertices = componentVertices(component);
            if (const auto plane = fitWaterSurfacePlane(vertices, maximumDeviation, minimumUpwardNormal))
            {
                result.mPlanarPatches.push_back({ *plane, component });
                continue;
            }
            result.mResidualTriangleIndices.insert(
                result.mResidualTriangleIndices.end(), component.begin(), component.end());
        }
        std::sort(result.mResidualTriangleIndices.begin(), result.mResidualTriangleIndices.end());
        return result;
    }

    std::vector<std::array<unsigned int, 3>> triangulateWaterSurfacePrimitives(const osg::PrimitiveSet& primitives)
    {
        std::vector<std::array<unsigned int, 3>> result;
        const unsigned int count = primitives.getNumIndices();
        const auto append = [&](unsigned int first, unsigned int second, unsigned int third) {
            const unsigned int a = primitives.index(first);
            const unsigned int b = primitives.index(second);
            const unsigned int c = primitives.index(third);
            if (a != b && b != c && a != c)
                result.push_back({ a, b, c });
        };
        switch (primitives.getMode())
        {
            case GL_TRIANGLES:
                result.reserve(count / 3);
                for (unsigned int index = 0; index + 2 < count; index += 3)
                    append(index, index + 1, index + 2);
                break;
            case GL_TRIANGLE_STRIP:
                result.reserve(count > 2 ? count - 2 : 0);
                for (unsigned int index = 0; index + 2 < count; ++index)
                    if (index % 2 == 0)
                        append(index, index + 1, index + 2);
                    else
                        append(index + 1, index, index + 2);
                break;
            case GL_TRIANGLE_FAN:
            case GL_POLYGON:
                result.reserve(count > 2 ? count - 2 : 0);
                for (unsigned int index = 1; index + 1 < count; ++index)
                    append(0, index, index + 1);
                break;
            case GL_QUADS:
                result.reserve(count / 2);
                for (unsigned int index = 0; index + 3 < count; index += 4)
                {
                    append(index, index + 1, index + 2);
                    append(index, index + 2, index + 3);
                }
                break;
            case GL_QUAD_STRIP:
                result.reserve(count > 2 ? count - 2 : 0);
                for (unsigned int index = 0; index + 3 < count; index += 2)
                {
                    append(index, index + 1, index + 3);
                    append(index, index + 3, index + 2);
                }
                break;
            default:
                break;
        }
        return result;
    }

    bool waterSurfaceTriangleNeedsReversedWinding(
        const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c, const WaterSurfacePlane& plane)
    {
        return (((b - a) ^ (c - a)) * plane.mNormal) < 0.f;
    }

    WaterSurfacePlaneKey waterSurfacePlaneKey(const WaterSurfacePlane& plane)
    {
        constexpr float normalResolution = 4096.f;
        constexpr float distanceResolution = 4.f;
        return { static_cast<int>(std::lround(plane.mNormal.x() * normalResolution)),
            static_cast<int>(std::lround(plane.mNormal.y() * normalResolution)),
            static_cast<int>(std::lround(plane.mNormal.z() * normalResolution)),
            static_cast<int>(std::lround(plane.mDistance * distanceResolution)) };
    }

    std::vector<WaterSurfaceBoundaryEdge> waterSurfaceBoundaryEdges(
        std::span<const osg::Vec3f> triangleVertices, float positionTolerance)
    {
        if (triangleVertices.size() % 3 != 0 || !(positionTolerance > 0.f))
            return {};
        using PositionKey = std::array<int, 3>;
        using EdgeKey = std::array<PositionKey, 2>;
        struct EdgeRecord
        {
            WaterSurfaceBoundaryEdge mEdge;
            unsigned int mCount = 0;
        };
        const auto positionKey = [positionTolerance](const osg::Vec3f& point) {
            const float scale = 1.f / positionTolerance;
            return PositionKey{ static_cast<int>(std::lround(point.x() * scale)),
                static_cast<int>(std::lround(point.y() * scale)), static_cast<int>(std::lround(point.z() * scale)) };
        };
        std::map<EdgeKey, EdgeRecord> records;
        for (std::size_t index = 0; index < triangleVertices.size(); index += 3)
        {
            for (unsigned int edge = 0; edge < 3; ++edge)
            {
                const osg::Vec3f& start = triangleVertices[index + edge];
                const osg::Vec3f& end = triangleVertices[index + (edge + 1) % 3];
                PositionKey startKey = positionKey(start);
                PositionKey endKey = positionKey(end);
                EdgeKey key{ startKey, endKey };
                if (endKey < startKey)
                    key = { endKey, startKey };
                EdgeRecord& record = records[key];
                if (record.mCount++ == 0)
                    record.mEdge = { start, end };
            }
        }
        std::vector<WaterSurfaceBoundaryEdge> result;
        for (const auto& [_, record] : records)
            if (record.mCount == 1)
                result.push_back(record.mEdge);
        return result;
    }

    std::optional<std::array<osg::Vec3f, 6>> waterSurfaceSeamTriangles(const WaterSurfaceBoundaryEdge& first,
        const WaterSurfaceBoundaryEdge& second, const WaterSurfacePlane& plane, float maximumGap)
    {
        if (!(maximumGap > 0.f))
            return std::nullopt;
        const osg::Vec2f firstStart = plane.project(first.mStart);
        const osg::Vec2f firstEnd = plane.project(first.mEnd);
        osg::Vec2f secondStart = plane.project(second.mStart);
        osg::Vec2f secondEnd = plane.project(second.mEnd);
        osg::Vec3f secondStartWorld = second.mStart;
        osg::Vec3f secondEndWorld = second.mEnd;
        const float directDistance = (firstStart - secondStart).length() + (firstEnd - secondEnd).length();
        const float reversedDistance = (firstStart - secondEnd).length() + (firstEnd - secondStart).length();
        if (reversedDistance < directDistance)
        {
            std::swap(secondStart, secondEnd);
            std::swap(secondStartWorld, secondEndWorld);
        }

        const osg::Vec2f firstDirection = firstEnd - firstStart;
        const osg::Vec2f secondDirection = secondEnd - secondStart;
        const float firstLength = firstDirection.length();
        const float secondLength = secondDirection.length();
        if (firstLength <= 0.001f || secondLength <= 0.001f || std::abs(firstLength - secondLength) > maximumGap)
            return std::nullopt;
        const float alignment = std::abs((firstDirection * secondDirection) / (firstLength * secondLength));
        if (alignment < std::cos(0.05f))
            return std::nullopt;

        const osg::Vec2f startGap = secondStart - firstStart;
        const osg::Vec2f endGap = secondEnd - firstEnd;
        const float largestGap = std::max(startGap.length(), endGap.length());
        if (largestGap <= 0.01f || largestGap > maximumGap)
            return std::nullopt;
        const osg::Vec2f edgeDirection = firstDirection / firstLength;
        if (std::abs(startGap * edgeDirection) > 0.25f * maximumGap
            || std::abs(endGap * edgeDirection) > 0.25f * maximumGap)
            return std::nullopt;

        return std::array<osg::Vec3f, 6>{ first.mStart, first.mEnd, secondEndWorld, first.mStart, secondEndWorld,
            secondStartWorld };
    }

    bool waterSurfacePlanesCompatible(const WaterSurfacePlane& first, const WaterSurfacePlane& second,
        float maximumAngularDifferenceRadians, float maximumPlaneDistance)
    {
        if (maximumAngularDifferenceRadians < 0.f || maximumPlaneDistance < 0.f)
            return false;
        const float minimumAlignment = std::cos(std::min(maximumAngularDifferenceRadians, 3.14159265358979323846f));
        return first.mNormal * second.mNormal >= minimumAlignment
            && std::abs(first.mDistance - second.mDistance) <= maximumPlaneDistance;
    }

    bool waterSurfacePlanesSpatiallyCompatible(const WaterSurfacePlane& first, const osg::BoundingBox& firstBound,
        const WaterSurfacePlane& second, const osg::BoundingBox& secondBound, float maximumAngularDifferenceRadians,
        float maximumSurfaceDistance, float maximumHorizontalGap)
    {
        if (!firstBound.valid() || !secondBound.valid() || maximumAngularDifferenceRadians < 0.f
            || maximumSurfaceDistance < 0.f || maximumHorizontalGap < 0.f || std::abs(first.mNormal.z()) < 0.0001f
            || std::abs(second.mNormal.z()) < 0.0001f)
            return false;

        const float minimumAlignment = std::cos(std::min(maximumAngularDifferenceRadians, 3.14159265358979323846f));
        if (first.mNormal * second.mNormal < minimumAlignment)
            return false;

        const auto nearestCoordinates
            = [](float firstMinimum, float firstMaximum, float secondMinimum, float secondMaximum) {
                  if (firstMaximum < secondMinimum)
                      return std::pair(firstMaximum, secondMinimum);
                  if (secondMaximum < firstMinimum)
                      return std::pair(firstMinimum, secondMaximum);
                  const float overlapCenter
                      = 0.5f * (std::max(firstMinimum, secondMinimum) + std::min(firstMaximum, secondMaximum));
                  return std::pair(overlapCenter, overlapCenter);
              };
        const auto [firstX, secondX]
            = nearestCoordinates(firstBound.xMin(), firstBound.xMax(), secondBound.xMin(), secondBound.xMax());
        const auto [firstY, secondY]
            = nearestCoordinates(firstBound.yMin(), firstBound.yMax(), secondBound.yMin(), secondBound.yMax());
        if (osg::Vec2f(secondX - firstX, secondY - firstY).length() > maximumHorizontalGap)
            return false;

        const float sampleX = 0.5f * (firstX + secondX);
        const float sampleY = 0.5f * (firstY + secondY);
        const auto heightAt = [sampleX, sampleY](const WaterSurfacePlane& plane) {
            return (plane.mDistance - plane.mNormal.x() * sampleX - plane.mNormal.y() * sampleY) / plane.mNormal.z();
        };
        return std::abs(heightAt(first) - heightAt(second)) <= maximumSurfaceDistance;
    }

    osg::Matrixd waterSurfaceReflectionMatrix(const WaterSurfacePlane& plane)
    {
        const osg::Vec3d normal(plane.mNormal);
        osg::Matrixd reflection;
        reflection.set(1.0 - 2.0 * normal.x() * normal.x(), -2.0 * normal.x() * normal.y(),
            -2.0 * normal.x() * normal.z(), 0.0, -2.0 * normal.y() * normal.x(), 1.0 - 2.0 * normal.y() * normal.y(),
            -2.0 * normal.y() * normal.z(), 0.0, -2.0 * normal.z() * normal.x(), -2.0 * normal.z() * normal.y(),
            1.0 - 2.0 * normal.z() * normal.z(), 0.0, 2.0 * plane.mDistance * normal.x(),
            2.0 * plane.mDistance * normal.y(), 2.0 * plane.mDistance * normal.z(), 1.0);
        return reflection;
    }

    bool waterReflectionShouldRender(
        bool hasRendered, unsigned int lastVisibleFrame, unsigned int currentFrame) noexcept
    {
        return !hasRendered || waterReflectionVisibleAtFrame(lastVisibleFrame, currentFrame);
    }

    void WaterReflectionResolutionPolicy::markVisible(unsigned int frameNumber, unsigned int requestedResolution)
    {
        if (mRequestFrame != frameNumber)
        {
            mRequestFrame = frameNumber;
            mRequestedResolution = requestedResolution;
        }
        else
            mRequestedResolution = std::max(mRequestedResolution, requestedResolution);
    }

    std::optional<unsigned int> WaterReflectionResolutionPolicy::consume(unsigned int currentResolution)
    {
        if (mConsumedRequestFrame == mRequestFrame)
            return std::nullopt;
        mConsumedRequestFrame = mRequestFrame;

        if (mRequestedResolution > currentResolution)
        {
            mDowngradeFrames = 0;
            if (++mUpgradeFrames >= 2)
            {
                mUpgradeFrames = 0;
                return mRequestedResolution;
            }
        }
        else if (mRequestedResolution < currentResolution)
        {
            mUpgradeFrames = 0;
            if (++mDowngradeFrames >= 45)
            {
                mDowngradeFrames = 0;
                return mRequestedResolution;
            }
        }
        else
        {
            mUpgradeFrames = 0;
            mDowngradeFrames = 0;
        }
        return std::nullopt;
    }

    WaterReflectionHandoffRequest WaterReflectionHandoffPolicy::request(unsigned int desiredResolution)
    {
        if (desiredResolution == mActiveResolution)
        {
            if (!mPendingResolution)
                return {};
            const unsigned int cancelledResolution = *mPendingResolution;
            mPendingResolution.reset();
            return { WaterReflectionHandoffAction::Cancel, cancelledResolution };
        }

        if (!mPendingResolution)
        {
            mPendingResolution = desiredResolution;
            return { WaterReflectionHandoffAction::Stage, std::nullopt };
        }
        if (*mPendingResolution == desiredResolution)
            return {};

        const unsigned int replacedResolution = *mPendingResolution;
        mPendingResolution = desiredResolution;
        return { WaterReflectionHandoffAction::Replace, replacedResolution };
    }

    std::optional<unsigned int> WaterReflectionHandoffPolicy::publish()
    {
        if (!mPendingResolution)
            return std::nullopt;
        const unsigned int previousResolution = mActiveResolution;
        mActiveResolution = *mPendingResolution;
        mPendingResolution.reset();
        return previousResolution;
    }

    float waterWaveInteractionHeight(float waveHeight)
    {
        return std::max(32.f, 2.f * std::max(0.f, waveHeight) + 32.f);
    }

    // disable nonsense test against a worldsize bb what will always pass
    class WaterBoundCallback : public osg::Drawable::ComputeBoundingBoxCallback
    {
        osg::BoundingBox computeBound(const osg::Drawable&) const override { return osg::BoundingBox(); }
    };

    osg::ref_ptr<osg::Geometry> createWaterGeometry(float size, int segments, float textureRepeats)
    {
        osg::ref_ptr<osg::Vec3Array> verts(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec2Array> texcoords(new osg::Vec2Array);

        // some drivers don't like huge triangles, so we do some subdivisons
        // a paged solution would be even better
        const float step = size / segments;
        const float texCoordStep = textureRepeats / segments;
        for (int x = 0; x < segments; ++x)
        {
            for (int y = 0; y < segments; ++y)
            {
                float x1 = -size / 2.f + x * step;
                float y1 = -size / 2.f + y * step;
                float x2 = x1 + step;
                float y2 = y1 + step;

                verts->push_back(osg::Vec3f(x1, y2, 0.f));
                verts->push_back(osg::Vec3f(x1, y1, 0.f));
                verts->push_back(osg::Vec3f(x2, y1, 0.f));
                verts->push_back(osg::Vec3f(x2, y2, 0.f));

                float u1 = x * texCoordStep;
                float v1 = textureRepeats - y * texCoordStep;
                float u2 = u1 + texCoordStep;
                float v2 = v1 - texCoordStep;

                texcoords->push_back(osg::Vec2f(u1, v2));
                texcoords->push_back(osg::Vec2f(u1, v1));
                texcoords->push_back(osg::Vec2f(u2, v1));
                texcoords->push_back(osg::Vec2f(u2, v2));
            }
        }

        osg::ref_ptr<osg::Geometry> waterGeom(new osg::Geometry);
        waterGeom->setVertexArray(verts);
        waterGeom->setTexCoordArray(0, texcoords);

        osg::ref_ptr<osg::Vec3Array> normal(new osg::Vec3Array);
        normal->push_back(osg::Vec3f(0, 0, 1));
        waterGeom->setNormalArray(normal, osg::Array::BIND_OVERALL);

        waterGeom->addPrimitiveSet(
            new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, static_cast<GLsizei>(verts->size())));
        waterGeom->setComputeBoundingBoxCallback(new WaterBoundCallback);
        waterGeom->setCullingActive(false);
        return waterGeom;
    }

    osg::ref_ptr<osg::Geometry> createDisplacedWaterGeometry(float size, float textureRepeats)
    {
        constexpr unsigned int angularSegments = 150;
        constexpr unsigned int radialSegments = 120;
        constexpr float detailedRadius = 9600.f;
        constexpr float horizonRadius = 500000.f;
        constexpr float twoPi = 6.28318530717958647692f;

        osg::ref_ptr<osg::Vec3Array> vertices(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec2Array> texcoords(new osg::Vec2Array);
        vertices->reserve(1 + angularSegments * radialSegments);
        texcoords->reserve(1 + angularSegments * radialSegments);
        vertices->push_back(osg::Vec3f(0.f, 0.f, -1.f));
        texcoords->push_back(osg::Vec2f(textureRepeats * 0.5f, textureRepeats * 0.5f));

        for (unsigned int ring = 0; ring < radialSegments; ++ring)
        {
            const float fraction = static_cast<float>(ring) / static_cast<float>(radialSegments);
            const float radius = ring + 1 == radialSegments
                ? horizonRadius
                : detailedRadius * (0.9f * fraction * fraction * fraction + 0.1f * fraction);
            for (unsigned int segment = 0; segment < angularSegments; ++segment)
            {
                const float angle = twoPi * static_cast<float>(segment) / static_cast<float>(angularSegments);
                const float x = radius * std::cos(angle);
                const float y = radius * std::sin(angle);
                vertices->push_back(osg::Vec3f(x, y, -1.f));
                texcoords->push_back(
                    osg::Vec2f((x / size + 0.5f) * textureRepeats, (0.5f - y / size) * textureRepeats));
            }
        }

        osg::ref_ptr<osg::DrawElementsUInt> indices(new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES));
        indices->reserve(angularSegments * (1 + 6 * (radialSegments - 1)));
        for (unsigned int segment = 0; segment < angularSegments; ++segment)
        {
            indices->push_back(0);
            indices->push_back(1 + segment);
            indices->push_back(1 + (segment + 1) % angularSegments);
        }
        for (unsigned int ring = 1; ring < radialSegments; ++ring)
        {
            const unsigned int inner = 1 + angularSegments * (ring - 1);
            const unsigned int outer = inner + angularSegments;
            for (unsigned int segment = 0; segment < angularSegments; ++segment)
            {
                const unsigned int next = (segment + 1) % angularSegments;
                indices->push_back(inner + segment);
                indices->push_back(outer + segment);
                indices->push_back(inner + next);
                indices->push_back(outer + segment);
                indices->push_back(outer + next);
                indices->push_back(inner + next);
            }
        }

        osg::ref_ptr<osg::Geometry> geometry(new osg::Geometry);
        geometry->setVertexArray(vertices);
        geometry->setTexCoordArray(0, texcoords);
        osg::ref_ptr<osg::Vec3Array> normal(new osg::Vec3Array);
        normal->push_back(osg::Vec3f(0.f, 0.f, 1.f));
        geometry->setNormalArray(normal, osg::Array::BIND_OVERALL);
        geometry->addPrimitiveSet(indices);
        geometry->setComputeBoundingBoxCallback(new WaterBoundCallback);
        geometry->setCullingActive(false);
        return geometry;
    }

    osg::ref_ptr<osg::StateSet> createSimpleWaterStateSet(float alpha, int renderBin)
    {
        osg::ref_ptr<osg::StateSet> stateset(new osg::StateSet);

        osg::ref_ptr<SceneUtil::Material> material(new SceneUtil::Material);
        material->setEmission(osg::Vec4f(0.f, 0.f, 0.f, 1.f));
        material->setDiffuse(osg::Vec4f(1.f, 1.f, 1.f, alpha));
        material->setAmbient(osg::Vec4f(1.f, 1.f, 1.f, 1.f));
        material->setVertexColorMode(SceneUtil::VertexColorModes::None);
        stateset->setAttributeAndModes(material, osg::StateAttribute::ON);

        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);

        osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
        depth->setWriteMask(false);
        stateset->setAttributeAndModes(depth, osg::StateAttribute::ON);

        stateset->addUniform(new osg::Uniform("waterSurface", true));
        stateset->setRenderBinDetails(renderBin, "DepthSortedBin");

        return stateset;
    }

}
