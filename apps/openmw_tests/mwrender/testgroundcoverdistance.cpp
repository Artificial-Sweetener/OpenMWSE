#include <apps/openmw/mwrender/groundcoverdistance.hpp>
#include <apps/openmw/mwrender/groundcovervisibility.hpp>

#include <gtest/gtest.h>

#include <osg/State>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

namespace
{
    /// Record actual LOD child selection at a controlled OSG traversal distance.
    class LodVisitor final : public osg::NodeVisitor
    {
    public:
        /// Use the production node's active-child traversal policy.
        LodVisitor()
            : osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN)
        {
        }

        /// Supply the same camera distance independently of geometry bounds.
        float getDistanceToViewPoint(const osg::Vec3&, bool) const override { return mDistance; }
        /// Record the visited representation without changing its node or settings.
        void apply(osg::Group& group) override { mVisited = group.getName(); }

        float mDistance = 75;
        std::string mVisited;
    };
}

TEST(GroundcoverDistanceTest, UpdatesExistingLodAndFadeUniformWithoutReplacingGeometry)
{
    osg::ref_ptr<MWRender::GroundcoverDistance> distance = new MWRender::GroundcoverDistance(100);
    osg::ref_ptr<MWRender::GroundcoverLod> lod = new MWRender::GroundcoverLod(distance);
    osg::ref_ptr<osg::Group> detailed = new osg::Group;
    detailed->setName("detailed");
    osg::ref_ptr<osg::Group> reduced = new osg::Group;
    reduced->setName("reduced");
    lod->addChild(detailed);
    lod->addChild(reduced);
    osg::ref_ptr<osg::StateSet> legacyState = new osg::StateSet;
    osg::ref_ptr<osg::StateSet> gpuState = new osg::StateSet;
    distance->attachFadeUniform(*legacyState);
    distance->attachFadeUniform(*gpuState);
    osg::Uniform* fade = legacyState->getUniform("groundcoverFade");
    ASSERT_NE(fade, nullptr);
    EXPECT_EQ(gpuState->getUniform("groundcoverFade"), fade);

    LodVisitor visitor;
    lod->traverse(visitor);
    EXPECT_EQ(visitor.mVisited, "detailed");
    distance->set(50);
    lod->traverse(visitor);
    EXPECT_EQ(visitor.mVisited, "reduced");
    EXPECT_EQ(lod->getChild(0), detailed);
    EXPECT_EQ(lod->getChild(1), reduced);
    osg::Vec2f interval;
    ASSERT_TRUE(fade->get(interval));
    EXPECT_EQ(interval, osg::Vec2f(45, 50));
    distance->set(200);
    lod->traverse(visitor);
    EXPECT_EQ(visitor.mVisited, "detailed");
}

TEST(GroundcoverDistanceTest, InvalidatesStationaryCameraVisibilityOnBothDistanceChanges)
{
    osg::ref_ptr<MWRender::GroundcoverDistance> distance = new MWRender::GroundcoverDistance(100);
    osg::ref_ptr<osg::Vec4Array> transforms = new osg::Vec4Array(2);
    std::vector<MWRender::GroundcoverDrawCluster> clusters{
        { { 0, 1 }, osg::BoundingBox(-1, -1, -21, 1, 1, -19) },
        { { 1, 1 }, osg::BoundingBox(-1, -1, -81, 1, 1, -79) },
    };
    osg::ref_ptr<MWRender::GroundcoverVisibility> visibility
        = new MWRender::GroundcoverVisibility(clusters, transforms, 1, distance);
    osg::ref_ptr<osgUtil::CullVisitor> visitor = new osgUtil::CullVisitor;
    visitor->setRenderStage(new osgUtil::RenderStage);
    visitor->setStateGraph(new osgUtil::StateGraph);
    visitor->setCullingMode(osg::CullSettings::NO_CULLING);
    visitor->pushViewport(new osg::Viewport(0, 0, 1024, 1024));
    visitor->pushProjectionMatrix(new osg::RefMatrix(osg::Matrix::ortho(-100, 100, -100, 100, 1, 200)));
    visitor->pushModelViewMatrix(new osg::RefMatrix, osg::Transform::ABSOLUTE_RF);

    EXPECT_EQ(visibility->update(*visitor, 0), 2u);
    EXPECT_EQ(visibility->update(*visitor, 0), 2u);
    distance->set(50);
    EXPECT_EQ(visibility->update(*visitor, 0), 1u);
    distance->set(0);
    EXPECT_EQ(visibility->update(*visitor, 0), 0u);
    distance->set(100);
    EXPECT_EQ(visibility->update(*visitor, 0), 2u);

    visitor->popModelViewMatrix();
    visitor->popProjectionMatrix();
    visitor->popViewport();
}

TEST(GroundcoverDistanceTest, RejectsInvalidDistanceWithoutChangingTheCurrentState)
{
    osg::ref_ptr<MWRender::GroundcoverDistance> distance = new MWRender::GroundcoverDistance(100);
    EXPECT_THROW(distance->set(-1), std::invalid_argument);
    EXPECT_THROW(distance->set(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
    EXPECT_THROW(distance->set(std::numeric_limits<float>::infinity()), std::invalid_argument);
    EXPECT_EQ(distance->get(), 100);
}
