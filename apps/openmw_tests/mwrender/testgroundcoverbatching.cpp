#include <apps/openmw/mwrender/groundcoverbatching.hpp>
#include <apps/openmw/mwrender/groundcoverinstances.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <vector>

#include <osg/Geometry>
#include <osg/Group>

namespace
{
    /// Preserve a recognizable drawable callback across batch configuration.
    class ExistingDrawableCullCallback final : public osg::DrawableCullCallback
    {
    public:
        /// Keep the test drawable visible.
        bool cull(osg::NodeVisitor*, osg::Drawable*, osg::RenderInfo*) const override { return false; }
    };

    /// Preserve a recognizable node callback beneath the model visibility callback.
    class ExistingNodeCullCallback final : public osg::Callback
    {
    public:
        /// Accept callback traversal without requiring a live cull visitor.
        bool run(osg::Object*, osg::Object*) override { return true; }
    };

    /// Construct a groundcover entry at the requested world-space position.
    MWRender::Groundcover::GroundcoverEntry makeEntry(float x, float y)
    {
        ESM::CellRef ref;
        ref.mPos.pos[0] = x;
        ref.mPos.pos[1] = y;
        return MWRender::Groundcover::GroundcoverEntry(ref);
    }

    /// Construct a groundcover entry with the complete transform used by batch-attribute checks.
    MWRender::Groundcover::GroundcoverEntry makeEntry(
        float x, float y, float z, float scale, const osg::Vec3f& rotation)
    {
        ESM::CellRef ref;
        ref.mPos.pos[0] = x;
        ref.mPos.pos[1] = y;
        ref.mPos.pos[2] = z;
        ref.mPos.rot[0] = rotation.x();
        ref.mPos.rot[1] = rotation.y();
        ref.mPos.rot[2] = rotation.z();
        ref.mScale = scale;
        return MWRender::Groundcover::GroundcoverEntry(ref);
    }
}

TEST(GroundcoverBatchingTest, ProducesContiguousFirstSeenClusters)
{
    const std::vector<MWRender::Groundcover::GroundcoverEntry> entries{ makeEntry(1.f, 1.f), makeEntry(2.f, 2.f),
        makeEntry(11.f, 1.f), makeEntry(12.f, 2.f), makeEntry(1.f, 1.f) };

    const MWRender::GroundcoverInstanceLayout layout = MWRender::makeGroundcoverInstanceLayout(entries, 10.f);

    EXPECT_EQ(layout.mSourceIndices, (std::vector<std::size_t>{ 0, 1, 4, 2, 3 }));
    ASSERT_EQ(layout.mClusters.size(), 2);
    EXPECT_EQ(layout.mClusters[0].mFirst, 0);
    EXPECT_EQ(layout.mClusters[0].mCount, 3);
    EXPECT_EQ(layout.mClusters[1].mFirst, 3);
    EXPECT_EQ(layout.mClusters[1].mCount, 2);
}

TEST(GroundcoverBatchingTest, FloorsNegativeCoordinatesIntoAdjacentClusters)
{
    const std::vector<MWRender::Groundcover::GroundcoverEntry> entries{ makeEntry(-0.1f, 0.f), makeEntry(-9.9f, 0.f),
        makeEntry(-10.1f, 0.f) };

    const MWRender::GroundcoverInstanceLayout layout = MWRender::makeGroundcoverInstanceLayout(entries, 10.f);

    ASSERT_EQ(layout.mClusters.size(), 2);
    EXPECT_EQ(layout.mClusters[0].mCount, 2);
    EXPECT_EQ(layout.mClusters[1].mFirst, 2);
    EXPECT_EQ(layout.mClusters[1].mCount, 1);
}

TEST(GroundcoverBatchingTest, RejectsInvalidClusterSizes)
{
    const std::vector<MWRender::Groundcover::GroundcoverEntry> entries{ makeEntry(0.f, 0.f) };

    EXPECT_THROW(MWRender::makeGroundcoverInstanceLayout(entries, 0.f), std::invalid_argument);
    EXPECT_THROW(MWRender::makeGroundcoverInstanceLayout(entries, -1.f), std::invalid_argument);
    EXPECT_THROW(MWRender::makeGroundcoverInstanceLayout(entries, std::numeric_limits<float>::quiet_NaN()),
        std::invalid_argument);
}

TEST(GroundcoverBatchingTest, BuildsSharedAttributesInSpatialOrder)
{
    const std::vector<MWRender::Groundcover::GroundcoverEntry> entries{
        makeEntry(1.f, 2.f, 3.f, 2.f, osg::Vec3f(0.1f, 0.2f, 0.3f)),
        makeEntry(11.f, 12.f, 13.f, 3.f, osg::Vec3f(0.4f, 0.5f, 0.6f)),
        makeEntry(2.f, 3.f, 4.f, 4.f, osg::Vec3f(0.7f, 0.8f, 0.9f)),
    };
    const MWRender::GroundcoverInstanceLayout layout = MWRender::makeGroundcoverInstanceLayout(entries, 10.f);

    const MWRender::GroundcoverInstanceBatch batch
        = MWRender::makeGroundcoverInstanceBatch(entries, layout, osg::Vec3f(1.f, 1.f, 1.f), 2.f);

    ASSERT_EQ(batch.mTransforms->size(), 3);
    EXPECT_EQ((*batch.mTransforms)[0], osg::Vec4f(0.f, 1.f, 2.f, 2.f));
    EXPECT_EQ((*batch.mTransforms)[1], osg::Vec4f(1.f, 2.f, 3.f, 4.f));
    EXPECT_EQ((*batch.mTransforms)[2], osg::Vec4f(10.f, 11.f, 12.f, 3.f));
    EXPECT_EQ((*batch.mRotations)[0], osg::Vec3f(0.1f, 0.2f, 0.3f));
    EXPECT_EQ((*batch.mRotations)[1], osg::Vec3f(0.7f, 0.8f, 0.9f));
    EXPECT_EQ((*batch.mRotations)[2], osg::Vec3f(0.4f, 0.5f, 0.6f));
    ASSERT_EQ(batch.mClusters.size(), 2);
    EXPECT_EQ(batch.mClusters[0].mInstances, (MWRender::GroundcoverInstanceRange{ 0, 2 }));
    EXPECT_EQ(batch.mClusters[1].mInstances, (MWRender::GroundcoverInstanceRange{ 2, 1 }));
    EXPECT_TRUE(batch.mClusters[0].mBounds.contains(osg::Vec3f(0.f, 1.f, 2.f)));
    EXPECT_TRUE(batch.mClusters[0].mBounds.contains(osg::Vec3f(1.f, 2.f, 3.f)));
    EXPECT_TRUE(batch.mClusters[1].mBounds.contains(osg::Vec3f(10.f, 11.f, 12.f)));
}

TEST(GroundcoverBatchingTest, RejectsInvalidSharedBatchInputs)
{
    const std::vector<MWRender::Groundcover::GroundcoverEntry> entries{ makeEntry(0.f, 0.f), makeEntry(1.f, 1.f) };
    MWRender::GroundcoverInstanceLayout layout = MWRender::makeGroundcoverInstanceLayout(entries, 10.f);

    EXPECT_THROW(
        MWRender::makeGroundcoverInstanceBatch(entries, layout, osg::Vec3f(), std::numeric_limits<float>::quiet_NaN()),
        std::invalid_argument);
    layout.mSourceIndices.clear();
    EXPECT_THROW(MWRender::makeGroundcoverInstanceBatch(entries, layout, osg::Vec3f(), 1.f), std::invalid_argument);

    layout = MWRender::makeGroundcoverInstanceLayout(entries, 10.f);
    layout.mSourceIndices[1] = layout.mSourceIndices[0];
    EXPECT_THROW(MWRender::makeGroundcoverInstanceBatch(entries, layout, osg::Vec3f(), 1.f), std::invalid_argument);

    layout = MWRender::makeGroundcoverInstanceLayout(entries, 10.f);
    layout.mClusters[0].mFirst = layout.mSourceIndices.size() + 1;
    EXPECT_THROW(MWRender::makeGroundcoverInstanceBatch(entries, layout, osg::Vec3f(), 1.f), std::invalid_argument);
}

TEST(GroundcoverBatchingTest, ConfiguresSupportedGeometryWithoutReplacingItsCullPolicy)
{
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3, 4));
    osg::ref_ptr<ExistingDrawableCullCallback> existingCallback = new ExistingDrawableCullCallback;
    geometry->setCullCallback(existingCallback);
    osg::ref_ptr<MWRender::GroundcoverBatch> batch = new MWRender::GroundcoverBatch({}, 100.f);

    EXPECT_TRUE(batch->configure(*geometry));

    ASSERT_EQ(geometry->getNumPrimitiveSets(), 1);
    EXPECT_STREQ(geometry->getPrimitiveSet(0)->className(), "GroundcoverClusteredPrimitive");
    EXPECT_EQ(geometry->getCullCallback(), existingCallback);
}

TEST(GroundcoverBatchingTest, LeavesUnsupportedPrimitivePathsUnchanged)
{
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    osg::ref_ptr<osg::DrawArrayLengths> primitive = new osg::DrawArrayLengths(GL_TRIANGLES, 0);
    primitive->push_back(3);
    geometry->addPrimitiveSet(primitive);
    osg::ref_ptr<MWRender::GroundcoverBatch> batch = new MWRender::GroundcoverBatch({}, 100.f);

    EXPECT_FALSE(batch->configure(*geometry));

    ASSERT_EQ(geometry->getNumPrimitiveSets(), 1);
    EXPECT_EQ(geometry->getPrimitiveSet(0), primitive);
}

TEST(GroundcoverBatchingTest, AttachesOneModelCallbackAboveExistingCullPolicy)
{
    osg::ref_ptr<osg::Group> node = new osg::Group;
    osg::ref_ptr<ExistingNodeCullCallback> existingCallback = new ExistingNodeCullCallback;
    node->setCullCallback(existingCallback);
    osg::ref_ptr<MWRender::GroundcoverBatch> batch = new MWRender::GroundcoverBatch({}, 100.f);

    batch->attach(*node);

    ASSERT_NE(node->getCullCallback(), existingCallback);
    EXPECT_EQ(node->getCullCallback()->getNestedCallback(), existingCallback);
}

TEST(GroundcoverBatchingTest, RejectsInvalidViewDistances)
{
    EXPECT_THROW(new MWRender::GroundcoverBatch({}, -1.f), std::invalid_argument);
    EXPECT_THROW(new MWRender::GroundcoverBatch({}, std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
}
