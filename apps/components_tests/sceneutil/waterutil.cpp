#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <osg/Array>
#include <osg/BoundingBox>
#include <osg/Geometry>

#include <components/sceneutil/waterutil.hpp>

namespace
{
    /// Resolve emitter contact against actual water triangles instead of their coarse bounding rectangles.
    TEST(WaterUtilTest, SurfaceTriangleContainmentIncludesEdgesAndRejectsBoundingBoxCorners)
    {
        const osg::Vec2f a(0.f, 0.f);
        const osg::Vec2f b(10.f, 0.f);
        const osg::Vec2f c(0.f, 10.f);
        EXPECT_TRUE(SceneUtil::waterSurfaceTriangleContainsPoint(osg::Vec2f(2.f, 2.f), a, b, c));
        EXPECT_TRUE(SceneUtil::waterSurfaceTriangleContainsPoint(osg::Vec2f(5.f, 0.f), a, b, c));
        EXPECT_FALSE(SceneUtil::waterSurfaceTriangleContainsPoint(osg::Vec2f(9.f, 9.f), a, b, c));
        EXPECT_TRUE(SceneUtil::waterSurfaceTriangleContainsPoint(osg::Vec2f(2.f, 2.f), c, b, a));
    }

    /// Keep mesh-water displacement dense enough to bend while bounding hostile or enormous content.
    TEST(WaterUtilTest, TriangleSubdivisionUsesWorldScaleAndAHardSafetyBound)
    {
        const osg::Vec3f a(0.f, 0.f, 0.f);
        const osg::Vec3f b(256.f, 0.f, 0.f);
        const osg::Vec3f c(0.f, 128.f, 0.f);
        EXPECT_EQ(SceneUtil::waterTriangleSubdivisionCount(a, b, c, 128.f), 3u);
        EXPECT_EQ(SceneUtil::waterTriangleSubdivisionCount(a, b, c, 0.f), 1u);
        EXPECT_EQ(SceneUtil::waterTriangleSubdivisionCount(a, osg::Vec3f(100000.f, 0.f, 0.f), c, 1.f), 128u);
    }

    /// Collapse coincident authored texture layers without merging nearby stepped pools.
    TEST(WaterUtilTest, CoincidentSurfaceLayersRequireMatchingFootprints)
    {
        const osg::BoundingBox base(0.f, 0.f, 100.f, 500.f, 800.f, 100.f);
        const osg::BoundingBox visualLayer(0.f, 0.f, 105.f, 500.f, 800.f, 105.f);
        const osg::BoundingBox offsetLayer(20.f, 0.f, 105.f, 520.f, 800.f, 105.f);

        EXPECT_TRUE(SceneUtil::waterSurfaceLayersCoincide(base, 100.f, visualLayer, 105.f));
        EXPECT_FALSE(SceneUtil::waterSurfaceLayersCoincide(base, 100.f, visualLayer, 109.f));
        EXPECT_FALSE(SceneUtil::waterSurfaceLayersCoincide(base, 100.f, offsetLayer, 105.f));
    }

    /// Accept arbitrarily inclined planes while rejecting connected curved and vertical flow geometry.
    TEST(WaterUtilTest, FitsOnlyConnectedPlanarWaterComponents)
    {
        const std::vector<osg::Vec3f> inclined{
            { 0.f, 0.f, 10.f },
            { 10.f, 0.f, 15.f },
            { 0.f, 10.f, 10.f },
            { 10.f, 0.f, 15.f },
            { 10.f, 10.f, 15.f },
            { 0.f, 10.f, 10.f },
        };
        const auto plane = SceneUtil::fitWaterSurfacePlane(inclined, 0.01f, 0.1f);
        ASSERT_TRUE(plane);
        EXPECT_NEAR(plane->signedDistance(inclined.front()), 0.f, 0.0001f);
        EXPECT_GT(plane->mNormal.z(), 0.1f);
        EXPECT_EQ(SceneUtil::waterSurfacePlaneKey(*plane), SceneUtil::waterSurfacePlaneKey(*plane));

        std::vector<osg::Vec3f> mixedWinding = inclined;
        std::swap(mixedWinding[3], mixedWinding[5]);
        EXPECT_TRUE(SceneUtil::fitWaterSurfacePlane(mixedWinding, 0.01f, 0.1f));

        std::vector<osg::Vec3f> curved = inclined;
        curved.back().z() += 2.f;
        EXPECT_FALSE(SceneUtil::fitWaterSurfacePlane(curved, 0.1f, 0.1f));

        const std::vector<osg::Vec3f> vertical{
            { 0.f, 0.f, 0.f },
            { 0.f, 10.f, 0.f },
            { 0.f, 0.f, 10.f },
        };
        EXPECT_FALSE(SceneUtil::fitWaterSurfacePlane(vertical, 0.01f, 0.1f));
    }

    /// Preserve complete authored planes while keeping every connected bent surface intact.
    TEST(WaterUtilTest, PartitionsOnlyWhollyPlanarConnectedComponents)
    {
        const std::vector<osg::Vec3f> simpleInclined{
            { 0.f, 0.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 0.f, 10.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 10.f, 10.f, 5.f },
            { 0.f, 10.f, 0.f },
        };
        const auto simple = SceneUtil::partitionWaterSurfaceTriangles(simpleInclined, 0.01f, 0.1f, 0.01f);
        ASSERT_EQ(simple.mPlanarPatches.size(), 1u);
        EXPECT_EQ(simple.mPlanarPatches.front().mTriangleIndices.size(), 2u);
        EXPECT_TRUE(simple.mResidualTriangleIndices.empty());

        std::vector<osg::Vec3f> disconnected = simpleInclined;
        for (const osg::Vec3f& vertex : simpleInclined)
            disconnected.push_back(vertex + osg::Vec3f(0.f, 11.f, 0.f));
        const auto separatePlanes = SceneUtil::partitionWaterSurfaceTriangles(disconnected, 0.01f, 0.1f, 0.01f);
        ASSERT_EQ(separatePlanes.mPlanarPatches.size(), 2u);
        EXPECT_EQ(separatePlanes.mPlanarPatches[0].mTriangleIndices.size(), 2u);
        EXPECT_EQ(separatePlanes.mPlanarPatches[1].mTriangleIndices.size(), 2u);
        EXPECT_TRUE(separatePlanes.mResidualTriangleIndices.empty());

        const std::vector<osg::Vec3f> curvedFacets{
            { 0.f, 0.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 0.f, 10.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 10.f, 10.f, 5.f },
            { 0.f, 10.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 20.f, 0.f, 12.f },
            { 10.f, 10.f, 5.f },
            { 20.f, 0.f, 12.f },
            { 20.f, 10.f, 12.f },
            { 10.f, 10.f, 5.f },
            { 20.f, 0.f, 12.f },
            { 30.f, 0.f, 20.f },
            { 20.f, 10.f, 12.f },
            { 30.f, 0.f, 20.f },
            { 30.f, 10.f, 20.f },
            { 20.f, 10.f, 12.f },
        };
        const auto smallFacets = SceneUtil::partitionWaterSurfaceTriangles(curvedFacets, 0.01f, 0.1f, 0.01f);
        EXPECT_TRUE(smallFacets.mPlanarPatches.empty());
        EXPECT_EQ(smallFacets.mResidualTriangleIndices.size(), 6u);

        const std::vector<osg::Vec3f> substantialBentPatch{
            { 0.f, 0.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 0.f, 10.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 10.f, 10.f, 5.f },
            { 0.f, 10.f, 0.f },
            { 10.f, 0.f, 5.f },
            { 20.f, 0.f, 10.f },
            { 10.f, 10.f, 5.f },
            { 20.f, 0.f, 10.f },
            { 20.f, 10.f, 10.f },
            { 10.f, 10.f, 5.f },
            { 20.f, 0.f, 10.f },
            { 30.f, 0.f, 18.f },
            { 20.f, 10.f, 10.f },
            { 30.f, 0.f, 18.f },
            { 30.f, 10.f, 18.f },
            { 20.f, 10.f, 10.f },
        };
        const auto substantialPatch
            = SceneUtil::partitionWaterSurfaceTriangles(substantialBentPatch, 0.01f, 0.1f, 0.01f);
        EXPECT_TRUE(substantialPatch.mPlanarPatches.empty());
        EXPECT_EQ(substantialPatch.mResidualTriangleIndices.size(), 6u);
    }

    /// Recover planar water authored as the indexed modes commonly emitted by NIF triangle strips and quads.
    TEST(WaterUtilTest, TriangulatesSupportedAuthoredPrimitiveModes)
    {
        osg::ref_ptr<osg::DrawElementsUInt> strip = new osg::DrawElementsUInt(GL_TRIANGLE_STRIP);
        for (const unsigned int index : { 0u, 1u, 2u, 3u })
            strip->push_back(index);
        const auto stripTriangles = SceneUtil::triangulateWaterSurfacePrimitives(*strip);
        ASSERT_EQ(stripTriangles.size(), 2u);
        EXPECT_EQ(stripTriangles[0], (std::array<unsigned int, 3>{ 0u, 1u, 2u }));
        EXPECT_EQ(stripTriangles[1], (std::array<unsigned int, 3>{ 2u, 1u, 3u }));

        osg::ref_ptr<osg::DrawElementsUInt> quads = new osg::DrawElementsUInt(GL_QUADS);
        for (const unsigned int index : { 4u, 5u, 6u, 7u })
            quads->push_back(index);
        const auto quadTriangles = SceneUtil::triangulateWaterSurfacePrimitives(*quads);
        ASSERT_EQ(quadTriangles.size(), 2u);
        EXPECT_EQ(quadTriangles[0], (std::array<unsigned int, 3>{ 4u, 5u, 6u }));
        EXPECT_EQ(quadTriangles[1], (std::array<unsigned int, 3>{ 4u, 6u, 7u }));
    }

    /// Orient replacement water toward the fitted plane even when source NIF triangles use mixed winding.
    TEST(WaterUtilTest, CanonicalWaterFrontSideUsesTheFittedPlaneOrientation)
    {
        const SceneUtil::WaterSurfacePlane plane;
        const osg::Vec3f a(0.f, 0.f, 10.f);
        const osg::Vec3f b(10.f, 0.f, 10.f);
        const osg::Vec3f c(0.f, 10.f, 10.f);

        EXPECT_FALSE(SceneUtil::waterSurfaceTriangleNeedsReversedWinding(a, b, c, plane));
        EXPECT_TRUE(SceneUtil::waterSurfaceTriangleNeedsReversedWinding(a, c, b, plane));
    }

    /// Share work only across surfaces whose fitted plane equations agree within explicit tolerances.
    TEST(WaterUtilTest, PlaneCompatibilityPreservesDistinctTiersAndSlopes)
    {
        const SceneUtil::WaterSurfacePlane base;
        SceneUtil::WaterSurfacePlane nearby = base;
        nearby.mDistance = 1.5f;
        EXPECT_TRUE(SceneUtil::waterSurfacePlanesCompatible(base, nearby, 0.02f, 2.f));
        nearby.mDistance = 3.f;
        EXPECT_FALSE(SceneUtil::waterSurfacePlanesCompatible(base, nearby, 0.02f, 2.f));

        SceneUtil::WaterSurfacePlane tilted = base;
        tilted.mNormal = osg::Vec3f(0.1f, 0.f, std::sqrt(0.99f));
        EXPECT_FALSE(SceneUtil::waterSurfacePlanesCompatible(base, tilted, 0.02f, 2.f));
    }

    /// Compare neighboring planes where they meet instead of comparing origin-relative plane constants.
    TEST(WaterUtilTest, SpatialPlaneCompatibilityJoinsWorldOffsetSeamsWithoutMergingTiers)
    {
        SceneUtil::WaterSurfacePlane first;
        first.mNormal = osg::Vec3f(0.001f, 0.f, 0.9999995f);
        first.mDistance = first.mNormal * osg::Vec3f(30000.f, -420000.f, 5000.f);
        SceneUtil::WaterSurfacePlane second;
        second.mNormal = osg::Vec3f(0.002f, 0.f, 0.999998f);
        second.mDistance = second.mNormal * osg::Vec3f(30100.f, -420000.f, 5000.f);
        const osg::BoundingBox firstBound(29900.f, -420100.f, 4999.f, 30000.f, -419900.f, 5001.f);
        const osg::BoundingBox secondBound(30002.f, -420100.f, 4999.f, 30100.f, -419900.f, 5001.f);

        EXPECT_GT(std::abs(first.mDistance - second.mDistance), 2.f);
        EXPECT_TRUE(
            SceneUtil::waterSurfacePlanesSpatiallyCompatible(first, firstBound, second, secondBound, 0.01f, 2.f, 4.f));

        SceneUtil::WaterSurfacePlane raised = second;
        raised.mDistance += 20.f;
        EXPECT_FALSE(
            SceneUtil::waterSurfacePlanesSpatiallyCompatible(first, firstBound, raised, secondBound, 0.01f, 2.f, 4.f));
        const osg::BoundingBox distantBound(30100.f, -420100.f, 4999.f, 30200.f, -419900.f, 5001.f);
        EXPECT_FALSE(
            SceneUtil::waterSurfacePlanesSpatiallyCompatible(first, firstBound, second, distantBound, 0.01f, 2.f, 4.f));
    }

    /// Reflect points across an arbitrary fitted plane without assuming a world-Z water height.
    TEST(WaterUtilTest, ReflectionMatrixUsesTheCompletePlaneEquation)
    {
        const std::vector<osg::Vec3f> triangles{
            { 0.f, 0.f, 0.f },
            { 10.f, 0.f, 10.f },
            { 0.f, 10.f, 0.f },
        };
        const auto plane = SceneUtil::fitWaterSurfacePlane(triangles, 0.001f, 0.1f);
        ASSERT_TRUE(plane);
        const osg::Vec3f point(0.f, 0.f, 10.f);
        const osg::Vec3f reflected = point * SceneUtil::waterSurfaceReflectionMatrix(*plane);
        EXPECT_NEAR(plane->signedDistance(reflected), -plane->signedDistance(point), 0.0001f);
        const osg::Vec3f onPlane = plane->unproject(plane->project(triangles.front()));
        EXPECT_NEAR((onPlane - triangles.front()).length(), 0.f, 0.0001f);
    }

    /// Preserve the established world-water camera transform when using the generalized plane representation.
    TEST(WaterUtilTest, HorizontalReflectionMatrixMatchesEstablishedCameraTransform)
    {
        SceneUtil::WaterSurfacePlane plane;
        plane.mDistance = 173.25f;
        const osg::Matrixd established
            = osg::Matrixd::scale(1.0, 1.0, -1.0) * osg::Matrixd::translate(0.0, 0.0, 2.0 * plane.mDistance);
        const osg::Matrixd generalized = SceneUtil::waterSurfaceReflectionMatrix(plane);

        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                EXPECT_DOUBLE_EQ(generalized(row, column), established(row, column));
    }

    /// Bridge only matching lateral boundary seams and leave intentional longitudinal openings alone.
    TEST(WaterUtilTest, BoundarySeamsRequireParallelCompatibleEdges)
    {
        const std::vector<osg::Vec3f> square{
            { 0.f, 0.f, 0.f },
            { 10.f, 0.f, 0.f },
            { 0.f, 10.f, 0.f },
            { 10.f, 0.f, 0.f },
            { 10.f, 10.f, 0.f },
            { 0.f, 10.f, 0.f },
        };
        const auto edges = SceneUtil::waterSurfaceBoundaryEdges(square, 0.01f);
        EXPECT_EQ(edges.size(), 4u);

        const SceneUtil::WaterSurfacePlane plane;
        const SceneUtil::WaterSurfaceBoundaryEdge first{ { 0.f, 0.f, 0.f }, { 10.f, 0.f, 0.f } };
        const SceneUtil::WaterSurfaceBoundaryEdge lateral{ { 0.f, 1.f, 0.f }, { 10.f, 1.f, 0.f } };
        EXPECT_TRUE(SceneUtil::waterSurfaceSeamTriangles(first, lateral, plane, 2.f));

        const SceneUtil::WaterSurfaceBoundaryEdge distant{ { 0.f, 3.f, 0.f }, { 10.f, 3.f, 0.f } };
        EXPECT_FALSE(SceneUtil::waterSurfaceSeamTriangles(first, distant, plane, 2.f));
        const SceneUtil::WaterSurfaceBoundaryEdge longitudinal{ { 11.f, 0.f, 0.f }, { 21.f, 0.f, 0.f } };
        EXPECT_FALSE(SceneUtil::waterSurfaceSeamTriangles(first, longitudinal, plane, 2.f));
    }

    /// Keep small mesh-water reflections cheap while preserving large on-screen surfaces.
    TEST(WaterUtilTest, ReflectionResolutionUsesStableProjectedSizeBuckets)
    {
        EXPECT_EQ(SceneUtil::waterReflectionResolution(100.0, 512), 128u);
        EXPECT_EQ(SceneUtil::waterReflectionResolution(240.0, 512), 256u);
        EXPECT_EQ(SceneUtil::waterReflectionResolution(700.0, 512), 512u);
        EXPECT_EQ(SceneUtil::waterReflectionResolution(900.0, 256), 256u);
        EXPECT_EQ(SceneUtil::waterReflectionResolution(900.0, 32), 64u);
    }

    /// Permit either cull order without keeping an off-screen reflection alive indefinitely.
    TEST(WaterUtilTest, VisibilityDrivenReflectionSurvivesOneFrameCullOrdering)
    {
        EXPECT_TRUE(SceneUtil::waterReflectionVisibleAtFrame(10u, 10u));
        EXPECT_TRUE(SceneUtil::waterReflectionVisibleAtFrame(10u, 11u));
        EXPECT_FALSE(SceneUtil::waterReflectionVisibleAtFrame(10u, 12u));
        EXPECT_TRUE(SceneUtil::waterReflectionVisibleAtFrame(std::numeric_limits<unsigned int>::max(), 0u));
    }

    /// Initialize every replacement target before its texture can be sampled by the scene camera.
    TEST(WaterUtilTest, NewReflectionTargetsRenderOnceBeforeVisibilityCulling)
    {
        EXPECT_TRUE(SceneUtil::waterReflectionShouldRender(false, 0u, 400u));
        EXPECT_TRUE(SceneUtil::waterReflectionShouldRender(true, 399u, 400u));
        EXPECT_FALSE(SceneUtil::waterReflectionShouldRender(true, 398u, 400u));
    }

    /// Measure an exact water footprint in pixels rather than doubling a bounding-sphere radius in NDC space.
    TEST(WaterUtilTest, ReflectionScreenRectProjectsAndClampsExactSurfaceBounds)
    {
        const osg::BoundingBox surface(-1.f, -0.5f, -5.f, 1.f, 0.5f, -5.f);
        const SceneUtil::WaterReflectionScreenRect rect
            = SceneUtil::waterReflectionScreenRect(surface, osg::Matrixd::identity(), osg::Matrixd::identity());

        EXPECT_DOUBLE_EQ(rect.mLeft, 0.0);
        EXPECT_DOUBLE_EQ(rect.mBottom, 0.25);
        EXPECT_DOUBLE_EQ(rect.mRight, 1.0);
        EXPECT_DOUBLE_EQ(rect.mTop, 0.75);
        EXPECT_DOUBLE_EQ(rect.projectedDiameter(1920.0, 1080.0), 1920.0);
    }

    /// Fall back to the full reflection target when a surface crosses the camera plane.
    TEST(WaterUtilTest, ReflectionScreenRectIsConservativeAcrossCameraPlane)
    {
        osg::Matrixd perspective;
        perspective.makePerspective(60.0, 16.0 / 9.0, 0.1, 1000.0);
        const osg::BoundingBox crossing(-1.f, -1.f, -1.f, 1.f, 1.f, 1.f);
        const SceneUtil::WaterReflectionScreenRect rect
            = SceneUtil::waterReflectionScreenRect(crossing, osg::Matrixd::identity(), perspective);

        EXPECT_DOUBLE_EQ(rect.mLeft, 0.0);
        EXPECT_DOUBLE_EQ(rect.mBottom, 0.0);
        EXPECT_DOUBLE_EQ(rect.mRight, 1.0);
        EXPECT_DOUBLE_EQ(rect.mTop, 1.0);
    }

    /// Prevent camera motion near a bucket boundary from reallocating a reflection every frame.
    TEST(WaterUtilTest, ReflectionResolutionHysteresisUpgradesFastAndDowngradesSlowly)
    {
        SceneUtil::WaterReflectionResolutionPolicy policy;
        policy.markVisible(1, 256);
        EXPECT_FALSE(policy.consume(128));
        policy.markVisible(2, 256);
        EXPECT_EQ(policy.consume(128), 256u);

        for (unsigned int frame = 3; frame < 47; ++frame)
        {
            policy.markVisible(frame, 128);
            EXPECT_FALSE(policy.consume(256));
        }
        policy.markVisible(47, 128);
        EXPECT_EQ(policy.consume(256), 128u);
    }

    /// Retain the complete reflection until a staged target is explicitly published.
    TEST(WaterUtilTest, ReflectionHandoffNeverPublishesAnIncompleteResize)
    {
        SceneUtil::WaterReflectionHandoffPolicy policy(256);

        const SceneUtil::WaterReflectionHandoffRequest stage = policy.request(512);
        EXPECT_EQ(stage.mAction, SceneUtil::WaterReflectionHandoffAction::Stage);
        EXPECT_FALSE(stage.mReleaseResolution);
        EXPECT_EQ(policy.activeResolution(), 256u);
        EXPECT_EQ(policy.pendingResolution(), 512u);

        const SceneUtil::WaterReflectionHandoffRequest duplicate = policy.request(512);
        EXPECT_EQ(duplicate.mAction, SceneUtil::WaterReflectionHandoffAction::None);
        EXPECT_EQ(policy.activeResolution(), 256u);

        EXPECT_EQ(policy.publish(), 256u);
        EXPECT_EQ(policy.activeResolution(), 512u);
        EXPECT_FALSE(policy.pendingResolution());
    }

    /// Cancel or replace unfinished targets without disturbing the active reflection.
    TEST(WaterUtilTest, ReflectionHandoffSurvivesResolutionOscillation)
    {
        SceneUtil::WaterReflectionHandoffPolicy policy(256);
        EXPECT_EQ(policy.request(512).mAction, SceneUtil::WaterReflectionHandoffAction::Stage);

        const SceneUtil::WaterReflectionHandoffRequest replace = policy.request(128);
        EXPECT_EQ(replace.mAction, SceneUtil::WaterReflectionHandoffAction::Replace);
        EXPECT_EQ(replace.mReleaseResolution, 512u);
        EXPECT_EQ(policy.activeResolution(), 256u);
        EXPECT_EQ(policy.pendingResolution(), 128u);

        const SceneUtil::WaterReflectionHandoffRequest cancel = policy.request(256);
        EXPECT_EQ(cancel.mAction, SceneUtil::WaterReflectionHandoffAction::Cancel);
        EXPECT_EQ(cancel.mReleaseResolution, 128u);
        EXPECT_EQ(policy.activeResolution(), 256u);
        EXPECT_FALSE(policy.pendingResolution());
        EXPECT_FALSE(policy.publish());
    }

    /// Keep obstruction work inside a conservative slab around the displaced surface.
    TEST(WaterUtilTest, WaveInteractionHeightScalesWithConfiguredWaves)
    {
        EXPECT_FLOAT_EQ(SceneUtil::waterWaveInteractionHeight(-1.f), 32.f);
        EXPECT_FLOAT_EQ(SceneUtil::waterWaveInteractionHeight(0.f), 32.f);
        EXPECT_FLOAT_EQ(SceneUtil::waterWaveInteractionHeight(30.f), 92.f);
        EXPECT_FLOAT_EQ(SceneUtil::waterWaveInteractionHeight(250.f), 532.f);
    }

    /// Verify displaced water uses MGE XE's compact radial topology and cubic near-field density.
    TEST(WaterUtilTest, DisplacedGeometryMatchesMgeXeRadialTopology)
    {
        constexpr float size = 8192.f * 150.f;
        osg::ref_ptr<osg::Geometry> geometry = SceneUtil::createDisplacedWaterGeometry(size, 900.f);
        const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
        const auto* indices = dynamic_cast<const osg::DrawElementsUInt*>(geometry->getPrimitiveSet(0));

        ASSERT_NE(vertices, nullptr);
        ASSERT_NE(indices, nullptr);
        EXPECT_EQ(vertices->size(), 1u + 150u * 120u);
        EXPECT_EQ(indices->size(), 150u * (3u + 6u * 119u));
        EXPECT_EQ(geometry->getNumPrimitiveSets(), 1u);
        EXPECT_EQ(geometry->getPrimitiveSet(0)->getMode(), osg::PrimitiveSet::TRIANGLES);
        EXPECT_FLOAT_EQ((*vertices)[0].z(), -1.f);

        const auto horizontalRadius = [](const osg::Vec3f& vertex) { return std::hypot(vertex.x(), vertex.y()); };
        const float firstRingRadius = horizontalRadius((*vertices)[1]);
        const float middleRingRadius = horizontalRadius((*vertices)[1 + 150 * 60]);
        const float penultimateRingRadius = horizontalRadius((*vertices)[1 + 150 * 118]);
        const float finalRingRadius = horizontalRadius((*vertices)[1 + 150 * 119]);
        EXPECT_FLOAT_EQ(firstRingRadius, 0.f);
        EXPECT_NEAR(middleRingRadius, 1560.f, 0.01f);
        EXPECT_LT(penultimateRingRadius, 9600.f);
        EXPECT_NEAR(finalRingRadius, 500000.f, 1.f);
    }

    /// Verify the zero-wave geometry remains the exact low-density upstream plane.
    TEST(WaterUtilTest, FlatGeometryRetainsLegacyTopology)
    {
        osg::ref_ptr<osg::Geometry> geometry = SceneUtil::createWaterGeometry(1000.f, 40, 900.f);
        const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());

        ASSERT_NE(vertices, nullptr);
        EXPECT_EQ(vertices->size(), 40u * 40u * 4u);
        EXPECT_EQ(geometry->getPrimitiveSet(0)->getMode(), osg::PrimitiveSet::QUADS);
    }
}
