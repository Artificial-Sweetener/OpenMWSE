#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <osg/Array>
#include <osg/Geometry>

#include <components/sceneutil/waterutil.hpp>

namespace
{
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
