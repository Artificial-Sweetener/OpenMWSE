#include <apps/openmw/mwrender/groundcovernearfar.hpp>

#include <gtest/gtest.h>

#include <components/esm/position.hpp>
#include <components/misc/convert.hpp>

namespace
{
    /// Require two matrices to preserve every legacy transform component exactly.
    void expectExactMatrix(const osg::Matrix& actual, const osg::Matrix& expected)
    {
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
                EXPECT_DOUBLE_EQ(actual(row, column), expected(row, column));
    }
}

TEST(GroundcoverNearFarTest, ReconstructsLegacyInstanceMatricesExactly)
{
    ESM::Position position;
    position.pos[0] = 125.5f;
    position.pos[1] = -73.25f;
    position.pos[2] = 18.0f;
    position.rot[0] = 0.2f;
    position.rot[1] = -0.4f;
    position.rot[2] = 1.1f;
    constexpr float scale = 1.75f;
    const osg::Vec3f chunkPosition(100.0f, -100.0f, 0.0f);

    const osg::Matrix expected = osg::Matrix::scale(scale, scale, scale)
        * osg::Matrix(Misc::Convert::makeOsgQuat(position))
        * osg::Matrix::translate(position.asVec3() - chunkPosition);
    const osg::Vec4f transform(position.asVec3() - chunkPosition, scale);
    expectExactMatrix(MWRender::makeGroundcoverInstanceMatrix(transform, position.asRotationVec3()), expected);
}

TEST(GroundcoverNearFarTest, MaterializesEveryCompactInstanceOnce)
{
    osg::ref_ptr<osg::Vec4Array> transforms = new osg::Vec4Array;
    transforms->push_back(osg::Vec4f(1.0f, 2.0f, 3.0f, 0.5f));
    transforms->push_back(osg::Vec4f(-4.0f, 5.0f, -6.0f, 2.0f));
    osg::ref_ptr<osg::Vec3Array> rotations = new osg::Vec3Array;
    rotations->push_back(osg::Vec3f(0.1f, 0.2f, 0.3f));
    rotations->push_back(osg::Vec3f(-0.4f, 0.5f, -0.6f));

    MWRender::GroundcoverInstanceMatrices matrices(transforms, rotations);
    EXPECT_EQ(matrices.size(), 2);
    const std::vector<osg::Matrix>& first = matrices.get();
    const std::vector<osg::Matrix>& second = matrices.get();
    ASSERT_EQ(first.size(), 2);
    EXPECT_EQ(&first, &second);
    expectExactMatrix(first[0], MWRender::makeGroundcoverInstanceMatrix((*transforms)[0], (*rotations)[0]));
    expectExactMatrix(first[1], MWRender::makeGroundcoverInstanceMatrix((*transforms)[1], (*rotations)[1]));
}

TEST(GroundcoverNearFarTest, RejectsMismatchedCompactAttributes)
{
    osg::ref_ptr<osg::Vec4Array> transforms = new osg::Vec4Array(2);
    osg::ref_ptr<osg::Vec3Array> rotations = new osg::Vec3Array(1);
    EXPECT_THROW(MWRender::GroundcoverInstanceMatrices(transforms, rotations), std::invalid_argument);
}
