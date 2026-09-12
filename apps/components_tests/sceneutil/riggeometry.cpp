#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <gtest/gtest.h>

#include <osg/MatrixTransform>
#include <osg/PrimitiveSet>
#include <osg/TriangleFunctor>
#include <osgAnimation/Skeleton>
#include <osgUtil/CullVisitor>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgUtil/UpdateVisitor>

#include <algorithm>

namespace
{
    struct RigFixture
    {
        RigFixture()
        {
            mSource = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            vertices->push_back({ 0.f, 0.f, 0.f });
            vertices->push_back({ 1.f, 0.f, 0.f });
            vertices->push_back({ 0.f, 1.f, 0.f });
            mSource->setVertexArray(vertices);
            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array(3);
            std::fill(normals->begin(), normals->end(), osg::Vec3f(0.f, 0.f, 1.f));
            mSource->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            osg::ref_ptr<osg::Vec4Array> tangents = new osg::Vec4Array(3);
            std::fill(tangents->begin(), tangents->end(), osg::Vec4f(1.f, 0.f, 0.f, 1.f));
            mSource->setTexCoordArray(7, tangents, osg::Array::BIND_PER_VERTEX);
            mSource->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));

            mRig->setName("rig");
            mRig->setSourceGeometry(mSource);
            std::vector<SceneUtil::RigGeometry::BoneInfo> bones(1);
            bones[0].mName = "bone";
            bones[0].mBoundSphere = osg::BoundingSpheref({ 0.5f, 0.5f, 0.f }, 1.f);
            bones[0].mInvBindMatrix.makeIdentity();
            mRig->setBoneInfo(std::move(bones));
            mRig->setInfluences({ { { 0, 1.f } }, { { 0, 1.f } }, { { 0, 1.f } } });
            mRig->setGpuSkinning(true);

            mBone->setName("bone");
            mSkeleton->addChild(mBone);
            mSkeleton->addChild(mRig);
        }

        void update(unsigned int frame)
        {
            osgUtil::UpdateVisitor update;
            update.setTraversalNumber(frame);
            mSkeleton->accept(update);
        }

        osg::ref_ptr<SceneUtil::RigGeometry> mRig = new SceneUtil::RigGeometry;
        osg::ref_ptr<osg::MatrixTransform> mBone = new osg::MatrixTransform;
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::Geometry> mSource;
    };

    struct CullCapture : osgUtil::CullVisitor
    {
        using osgUtil::CullVisitor::apply;

        void apply(osg::Geometry& geometry) override { mGeometry = &geometry; }

        osg::observer_ptr<osg::Geometry> mGeometry;
    };

    bool intersects(osg::Node& node, const osg::Vec3d& start, const osg::Vec3d& end)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector = new osgUtil::LineSegmentIntersector(start, end);
        osgUtil::IntersectionVisitor visitor(intersector);
        node.accept(visitor);
        return intersector->containsIntersections();
    }

    struct CollectTriangles
    {
        std::vector<osg::Vec3f> mVertices;

        void operator()(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c)
        {
            mVertices.insert(mVertices.end(), { a, b, c });
        }
    };

    TEST(OsgaRigGeometryTest, SoftwareKeepsSourceBuffers)
    {
        RigFixture fixture;
        osg::ref_ptr<osgAnimation::RigGeometry> source = new osgAnimation::RigGeometry;
        source->setSourceGeometry(fixture.mSource);
        source->copyFrom(*fixture.mSource);
        source->setInfluenceMap(new osgAnimation::VertexInfluenceMap);
        osg::ref_ptr<SceneUtil::RigGeometryHolder> holder
            = new SceneUtil::RigGeometryHolder(*source, osg::CopyOp::SHALLOW_COPY);
        osg::ref_ptr<osgAnimation::Skeleton> skeleton = new osgAnimation::Skeleton;
        skeleton->addChild(holder);
        osg::VertexBufferObject* buffer = fixture.mSource->getVertexArray()->getVertexBufferObject();
        ASSERT_TRUE(buffer);
        const unsigned int count = buffer->getNumBufferData();

        osgUtil::UpdateVisitor update;
        for (int i = 0; i < 2; ++i)
        {
            SceneUtil::OsgaRigGeometry* geometry = holder->getGeometry(i);
            osg::ref_ptr<osg::Array> vertices = geometry->getVertexArray();
            osg::ref_ptr<osg::Array> normals = geometry->getNormalArray();
            holder->updateRigGeometry(geometry, &update);
            EXPECT_EQ(geometry->getVertexArray(), vertices.get());
            EXPECT_EQ(geometry->getNormalArray(), normals.get());
            EXPECT_EQ(buffer->getNumBufferData(), count);
            EXPECT_NE(geometry->getVertexArray()->getVertexBufferObject(), buffer);
            EXPECT_NE(geometry->getNormalArray()->getVertexBufferObject(), buffer);
        }
    }

    TEST(RigGeometryTest, GpuRaycastUsesPose)
    {
        RigFixture fixture;
        fixture.mBone->setMatrix(osg::Matrixf::translate(10.f, 0.f, 0.f));
        fixture.update(7);

        EXPECT_TRUE(intersects(*fixture.mSkeleton, { 10.25, 0.25, -1.0 }, { 10.25, 0.25, 1.0 }));

        osg::TriangleFunctor<CollectTriangles> triangles;
        fixture.mRig->accept(triangles);
        ASSERT_EQ(triangles.mVertices.size(), 3u);
        EXPECT_EQ(triangles.mVertices[0], osg::Vec3f(10.f, 0.f, 0.f));
        EXPECT_EQ(triangles.mVertices[1], osg::Vec3f(11.f, 0.f, 0.f));
        EXPECT_EQ(triangles.mVertices[2], osg::Vec3f(10.f, 1.f, 0.f));
    }

    TEST(RigGeometryTest, QueriesKeepSourceBuffers)
    {
        RigFixture fixture;
        fixture.mRig->setCpuSkinning(false);
        fixture.update(7);
        osg::ref_ptr<osg::VertexBufferObject> buffer = fixture.mSource->getVertexArray()->getVertexBufferObject();
        ASSERT_TRUE(buffer);
        const unsigned int count = buffer->getNumBufferData();

        osg::TriangleFunctor<CollectTriangles> triangles;
        fixture.mRig->accept(triangles);
        ASSERT_EQ(triangles.mVertices.size(), 3u);
        EXPECT_EQ(buffer->getNumBufferData(), count);
    }

    TEST(RigGeometryTest, QueryUsesGeometryFrame)
    {
        RigFixture fixture;
        // Skeleton space needs the skin path.
        fixture.mRig->setCpuSkinning(false);
        osg::ref_ptr<osg::MatrixTransform> offset = new osg::MatrixTransform(osg::Matrixf::translate(0.f, 0.f, 5.f));
        fixture.mSkeleton->removeChild(fixture.mRig);
        offset->addChild(fixture.mRig);
        fixture.mSkeleton->addChild(offset);
        fixture.mBone->setMatrix(osg::Matrixf::translate(10.f, 0.f, 0.f));
        fixture.update(7);

        EXPECT_FLOAT_EQ(fixture.mRig->getBoundingBox().zMin(), -6.f);
        EXPECT_FLOAT_EQ(fixture.mRig->getBoundingBox().zMax(), -4.f);

        osg::TriangleFunctor<CollectTriangles> triangles;
        fixture.mRig->accept(triangles);
        ASSERT_EQ(triangles.mVertices.size(), 3u);
        EXPECT_EQ(triangles.mVertices[0], osg::Vec3f(10.f, 0.f, -5.f));
        EXPECT_EQ(triangles.mVertices[1], osg::Vec3f(11.f, 0.f, -5.f));
        EXPECT_EQ(triangles.mVertices[2], osg::Vec3f(10.f, 1.f, -5.f));

        EXPECT_TRUE(intersects(*fixture.mSkeleton, { 10.25, 0.25, -1.0 }, { 10.25, 0.25, 1.0 }));

        unsigned int frame = 0;
        osg::BoundingBox bound;
        ASSERT_TRUE(fixture.mRig->getLatestBound(frame, bound));
        EXPECT_FLOAT_EQ(bound.zMin(), -1.f);
        EXPECT_FLOAT_EQ(bound.zMax(), 1.f);
    }

    TEST(RigGeometryTest, GpuSnapshotsMatchFrames)
    {
        RigFixture fixture;
        fixture.mBone->setMatrix(osg::Matrixf::translate(10.f, 0.f, 0.f));
        fixture.update(7);

        unsigned int latestFrame = 0;
        osg::BoundingBox latestBound;
        ASSERT_TRUE(fixture.mRig->getLatestBound(latestFrame, latestBound));
        EXPECT_EQ(latestFrame, 7u);
        EXPECT_FLOAT_EQ(latestBound.xMin(), 9.5f);
        EXPECT_FLOAT_EQ(latestBound.xMax(), 11.5f);

        fixture.mBone->setMatrix(osg::Matrixf::translate(20.f, 0.f, 0.f));
        fixture.update(8);

        std::vector<osg::Matrixf> palette;
        osg::BoundingBox bound;
        ASSERT_TRUE(fixture.mRig->getPalette(7, palette));
        ASSERT_EQ(palette.size(), 2u);
        EXPECT_EQ(palette[1].getTrans(), osg::Vec3f(10.f, 0.f, 0.f));
        ASSERT_TRUE(fixture.mRig->getBound(7, bound));
        EXPECT_FLOAT_EQ(bound.xMin(), 9.5f);

        ASSERT_TRUE(fixture.mRig->getLatestBound(latestFrame, latestBound));
        EXPECT_EQ(latestFrame, 8u);
        ASSERT_TRUE(fixture.mRig->getPalette(8, palette));
        EXPECT_EQ(palette[1].getTrans(), osg::Vec3f(20.f, 0.f, 0.f));
        EXPECT_FLOAT_EQ(latestBound.xMin(), 19.5f);

        fixture.update(9);
        fixture.update(10);
        EXPECT_FALSE(fixture.mRig->getPalette(7, palette));
        EXPECT_FALSE(fixture.mRig->getBound(7, bound));
    }

    TEST(RigGeometryTest, InactiveRigKeepsSnapshot)
    {
        RigFixture fixture;
        fixture.mBone->setMatrix(osg::Matrixf::translate(10.f, 0.f, 0.f));
        fixture.update(7);
        fixture.mSkeleton->setActive(SceneUtil::Skeleton::Inactive);
        fixture.mBone->setMatrix(osg::Matrixf::translate(20.f, 0.f, 0.f));
        fixture.update(8);

        unsigned int frame = 0;
        std::vector<osg::Matrixf> palette;
        ASSERT_TRUE(fixture.mRig->getLatestPalette(frame, palette));
        EXPECT_EQ(frame, 7u);
        ASSERT_EQ(palette.size(), 2u);
        EXPECT_EQ(palette[1].getTrans(), osg::Vec3f(10.f, 0.f, 0.f));
        osg::BoundingBox bound;
        ASSERT_TRUE(fixture.mRig->getLatestBound(frame, bound));
        EXPECT_EQ(frame, 7u);
        EXPECT_FLOAT_EQ(bound.xMin(), 9.5f);
    }

    TEST(RigGeometryTest, DetachedRigKeepsPath)
    {
        RigFixture fixture;
        osg::ref_ptr<osg::MatrixTransform> attachment = new osg::MatrixTransform;
        attachment->setMatrix(osg::Matrixf::translate(5.f, 0.f, 0.f));
        fixture.mSkeleton->removeChild(fixture.mRig);
        attachment->addChild(fixture.mRig);
        fixture.mSkeleton->addChild(attachment);
        fixture.mBone->setMatrix(osg::Matrixf::translate(10.f, 0.f, 0.f));
        fixture.mRig->setCpuSkinning(false);
        fixture.update(7);

        unsigned int frame = 0;
        osg::BoundingBox bound;
        ASSERT_TRUE(fixture.mRig->getLatestBound(frame, bound));
        EXPECT_EQ(frame, 7u);
        EXPECT_FLOAT_EQ(bound.xMin(), 9.5f);
        EXPECT_FLOAT_EQ(bound.xMax(), 11.5f);
    }

    TEST(RigGeometryTest, GpuRigDropsCopies)
    {
        RigFixture fixture;
        osg::Geometry* identity = fixture.mRig->getGpuGeometry();
        ASSERT_NE(identity, nullptr);
        EXPECT_EQ(identity->getVertexArray(), fixture.mSource->getVertexArray());
        EXPECT_EQ(identity->getNormalArray(), fixture.mSource->getNormalArray());
        EXPECT_EQ(identity->getTexCoordArray(7), fixture.mSource->getTexCoordArray(7));

        fixture.mBone->setMatrix(osg::Matrixf::translate(10.f, 0.f, 0.f));
        fixture.update(7);
        osg::ref_ptr<CullCapture> firstCull = new CullCapture;
        firstCull->setTraversalNumber(7);
        fixture.mRig->accept(*firstCull);
        ASSERT_TRUE(firstCull->mGeometry.valid());
        EXPECT_NE(firstCull->mGeometry->getVertexArray(), fixture.mSource->getVertexArray());
        EXPECT_NE(firstCull->mGeometry->getNormalArray(), fixture.mSource->getNormalArray());
        EXPECT_NE(firstCull->mGeometry->getTexCoordArray(7), fixture.mSource->getTexCoordArray(7));
        osg::ref_ptr<CullCapture> secondCull = new CullCapture;
        secondCull->setTraversalNumber(8);
        fixture.mRig->accept(*secondCull);
        ASSERT_TRUE(secondCull->mGeometry.valid());
        EXPECT_NE(secondCull->mGeometry.get(), firstCull->mGeometry.get());
        EXPECT_NE(secondCull->mGeometry->getVertexArray(), fixture.mSource->getVertexArray());
        EXPECT_NE(secondCull->mGeometry->getVertexArray(), firstCull->mGeometry->getVertexArray());
        const auto mutableBytes = [](const osg::Geometry& geometry) {
            return geometry.getVertexArray()->getTotalDataSize() + geometry.getNormalArray()->getTotalDataSize()
                + geometry.getTexCoordArray(7)->getTotalDataSize();
        };
        // Each cull copy mirrors the source arrays.
        const unsigned int sourceBytes = mutableBytes(*fixture.mSource);
        EXPECT_EQ(mutableBytes(*firstCull->mGeometry) + mutableBytes(*secondCull->mGeometry), 2 * sourceBytes);

        fixture.mRig->setCpuSkinning(false);
        EXPECT_FALSE(firstCull->mGeometry.valid());
        EXPECT_FALSE(secondCull->mGeometry.valid());
        EXPECT_EQ(fixture.mRig->getGpuGeometry(), identity);

        osg::TriangleFunctor<CollectTriangles> triangles;
        fixture.mRig->accept(triangles);
        ASSERT_EQ(triangles.mVertices.size(), 3u);
        EXPECT_EQ(triangles.mVertices[0], osg::Vec3f(10.f, 0.f, 0.f));

        fixture.mRig->setCpuSkinning(true);
        osg::ref_ptr<CullCapture> cull = new CullCapture;
        cull->setTraversalNumber(9);
        fixture.mRig->accept(*cull);
        ASSERT_TRUE(cull->mGeometry.valid());
        EXPECT_NE(cull->mGeometry->getVertexArray(), fixture.mSource->getVertexArray());
        EXPECT_NE(cull->mGeometry->getNormalArray(), fixture.mSource->getNormalArray());
        EXPECT_NE(cull->mGeometry->getTexCoordArray(7), fixture.mSource->getTexCoordArray(7));
        EXPECT_EQ(fixture.mRig->getGpuGeometry(), identity);

        fixture.mRig->setGpuSkinning(false);
        osg::TriangleFunctor<CollectTriangles> restored;
        fixture.mRig->accept(restored);
        ASSERT_EQ(restored.mVertices.size(), 3u);
        EXPECT_EQ(restored.mVertices[0], osg::Vec3f(10.f, 0.f, 0.f));

        fixture.mRig->setCpuSkinning(false);
        EXPECT_FALSE(cull->mGeometry.valid());
        EXPECT_EQ(fixture.mRig->getGpuGeometry(), identity);
    }

    TEST(RigGeometryTest, InfluencesCappedAndNormalized)
    {
        osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
        // Two vertices from the Triss outfit, #5125
        rig->setInfluences({
            { { 3, 0.434882f }, { 5, 0.00723172f }, { 6, 0.0961262f }, { 9, 0.121417f }, { 10, 0.00592406f } },
            { { 3, 0.227529f }, { 6, 0.46532f }, { 10, 0.61739f } },
        });

        std::vector<SceneUtil::RigGeometry::SkinRange> ranges;
        std::vector<SceneUtil::RigGeometry::SkinWeight> weights;
        rig->getSkinData(ranges, weights);
        const std::vector<std::uint32_t>& groups = rig->getSkinGroups();
        ASSERT_EQ(groups.size(), 2u);
        ASSERT_EQ(ranges.size(), 2u);

        const SceneUtil::RigGeometry::SkinRange& five = ranges[groups[0]];
        ASSERT_EQ(five.mCount, 4u);
        const std::vector<std::uint32_t> fiveBones = { 3, 9, 6, 5 };
        const std::vector<float> fiveWeights = { 0.65925f, 0.18406f, 0.14572f, 0.01096f };
        float sum = 0.f;
        for (std::uint32_t i = 0; i < five.mCount; ++i)
        {
            EXPECT_EQ(weights[five.mFirst + i].mBone, fiveBones[i]);
            EXPECT_NEAR(weights[five.mFirst + i].mWeight, fiveWeights[i], 1e-4f);
            sum += weights[five.mFirst + i].mWeight;
        }
        EXPECT_NEAR(sum, 1.f, 1e-5f);

        const SceneUtil::RigGeometry::SkinRange& three = ranges[groups[1]];
        ASSERT_EQ(three.mCount, 3u);
        const std::vector<std::uint32_t> threeBones = { 10, 6, 3 };
        const std::vector<float> threeWeights = { 0.47120f, 0.35514f, 0.17365f };
        sum = 0.f;
        for (std::uint32_t i = 0; i < three.mCount; ++i)
        {
            EXPECT_EQ(weights[three.mFirst + i].mBone, threeBones[i]);
            EXPECT_NEAR(weights[three.mFirst + i].mWeight, threeWeights[i], 1e-4f);
            sum += weights[three.mFirst + i].mWeight;
        }
        EXPECT_NEAR(sum, 1.f, 1e-5f);
    }
}
