#include <apps/openmw/mwrender/groundcoverlod.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include <osg/Geometry>
#include <osg/Group>

namespace
{
    /// Create indexed cards whose second card in each pair has greater source-space area.
    osg::ref_ptr<osg::Geometry> makeCardGeometry(std::size_t cardCount)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(cardCount * 4);
        geometry->setVertexArray(vertices);
        osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
        for (unsigned int card = 0; card < cardCount; ++card)
        {
            const unsigned int first = card * 4;
            const float width = static_cast<float>(card + 1);
            (*vertices)[first] = osg::Vec3f(0.f, 0.f, 0.f);
            (*vertices)[first + 1] = osg::Vec3f(width, 0.f, 0.f);
            (*vertices)[first + 2] = osg::Vec3f(width, 1.f, 0.f);
            (*vertices)[first + 3] = osg::Vec3f(0.f, 1.f, 0.f);
            for (const unsigned int index : { first, first + 1, first + 2, first, first + 2, first + 3 })
                indices->push_back(index);
        }
        geometry->addPrimitiveSet(indices);
        return geometry;
    }
}

TEST(GroundcoverLodTest, PreservesDetailedModelAndSamplesCompleteCards)
{
    osg::ref_ptr<osg::Group> detailed = new osg::Group;
    osg::ref_ptr<osg::Geometry> geometry = makeCardGeometry(8);
    detailed->addChild(geometry);

    osg::ref_ptr<osg::Node> farNode = MWRender::makeGroundcoverFarLod(*detailed, 0.5f);

    ASSERT_EQ(geometry->getPrimitiveSet(0)->getNumIndices(), 48);
    osg::Geometry* farGeometry = farNode->asGroup()->getChild(0)->asGeometry();
    ASSERT_NE(farGeometry, nullptr);
    const auto* farIndices = dynamic_cast<const osg::DrawElementsUInt*>(farGeometry->getPrimitiveSet(0));
    ASSERT_NE(farIndices, nullptr);
    EXPECT_EQ(farIndices->size(), 24);
    EXPECT_EQ((*farIndices)[0], 4u);
    EXPECT_EQ((*farIndices)[6], 12u);
    EXPECT_EQ((*farIndices)[12], 20u);
    EXPECT_EQ((*farIndices)[18], 28u);
}

TEST(GroundcoverLodTest, PreservesNonCardTriangleMeshes)
{
    osg::ref_ptr<osg::Group> detailed = new osg::Group;
    osg::ref_ptr<osg::Geometry> geometry = makeCardGeometry(4);
    auto* indices = static_cast<osg::DrawElementsUInt*>(geometry->getPrimitiveSet(0));
    (*indices)[4] = 4;
    detailed->addChild(geometry);

    osg::ref_ptr<osg::Node> farNode = MWRender::makeGroundcoverFarLod(*detailed, 0.25f);

    const osg::Geometry* farGeometry = farNode->asGroup()->getChild(0)->asGeometry();
    ASSERT_NE(farGeometry, nullptr);
    EXPECT_EQ(farGeometry->getPrimitiveSet(0)->getNumIndices(), 24);
}

TEST(GroundcoverLodTest, RetainsTwoCrossingCardsAtMinimum)
{
    osg::ref_ptr<osg::Group> detailed = new osg::Group;
    detailed->addChild(makeCardGeometry(4));

    osg::ref_ptr<osg::Node> farNode = MWRender::makeGroundcoverFarLod(*detailed, 0.25f);

    const osg::Geometry* farGeometry = farNode->asGroup()->getChild(0)->asGeometry();
    ASSERT_NE(farGeometry, nullptr);
    EXPECT_EQ(farGeometry->getPrimitiveSet(0)->getNumIndices(), 12);
}

TEST(GroundcoverLodTest, KeepsDetailedGrassWithinSixCells)
{
    constexpr float cellSize = 8192.f;
    EXPECT_FLOAT_EQ(MWRender::getGroundcoverFarLodDistance(cellSize * 8.f), cellSize * 6.f);
    EXPECT_FLOAT_EQ(MWRender::getGroundcoverFarLodDistance(cellSize * 12.f), cellSize * 6.f);
    EXPECT_FLOAT_EQ(MWRender::getGroundcoverFarLodDistance(cellSize * 16.f), cellSize * 8.f);
    EXPECT_FLOAT_EQ(MWRender::getGroundcoverFarLodDistance(cellSize * 4.f), cellSize * 4.f);
}

TEST(GroundcoverLodTest, RejectsInvalidTriangleRatios)
{
    osg::ref_ptr<osg::Group> model = new osg::Group;
    EXPECT_THROW(MWRender::makeGroundcoverFarLod(*model, 0.f), std::invalid_argument);
    EXPECT_THROW(MWRender::makeGroundcoverFarLod(*model, 1.1f), std::invalid_argument);
    EXPECT_THROW(
        MWRender::makeGroundcoverFarLod(*model, std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
    EXPECT_THROW(MWRender::getGroundcoverFarLodDistance(-1.f), std::invalid_argument);
    EXPECT_THROW(
        MWRender::getGroundcoverFarLodDistance(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
}
