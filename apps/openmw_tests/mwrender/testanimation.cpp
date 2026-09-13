#include <apps/openmw/mwrender/animation.hpp>
#include <apps/openmw/mwrender/vismask.hpp>

#include <components/nifosg/matrixtransform.hpp>

#include <gtest/gtest.h>

#include <osg/Geometry>
#include <osg/NodeCallback>
#include <osg/PrimitiveSet>
#include <osgUtil/UpdateVisitor>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace
{
    class AnimationFixture : public MWRender::Animation
    {
    public:
        explicit AnimationFixture(const osg::ref_ptr<osg::Group>& root)
            : Animation(MWWorld::Ptr(), new osg::Group, nullptr)
        {
            mObjectRoot = root;
            mInsert->addChild(mObjectRoot);
        }
    };

    osg::ref_ptr<osg::Geometry> makeTriangle()
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ 0.f, 0.f, 0.f });
        vertices->push_back({ 1.f, 0.f, 0.f });
        vertices->push_back({ 0.f, 1.f, 0.f });
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        return geometry;
    }

    class CountCallback : public osg::NodeCallback
    {
    public:
        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            ++mCount;
            traverse(node, visitor);
        }

        unsigned int mCount = 0;
    };

    class GpuSceneLifecycleFixture : public MWRender::Animation
    {
    public:
        GpuSceneLifecycleFixture()
            : Animation(MWWorld::Ptr(), new osg::Group, nullptr)
        {
        }

        const std::vector<std::string_view>& events() const { return mEvents; }

    protected:
        void detachGpuInstances() override
        {
            mEvents.push_back(mCaptureScene == nullptr ? "detach:null" : "detach:scene");
        }

    public:
        void refreshGpuInstances() override
        {
            mEvents.push_back(mCaptureScene == nullptr ? "refresh:null" : "refresh:scene");
        }

    private:
        std::vector<std::string_view> mEvents;
    };

    TEST(AnimationTest, PrunesGpuRigBranches)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<NifOsg::MatrixTransform> proxy = new NifOsg::MatrixTransform;
        osg::ref_ptr<osg::Geometry> proxyGeometry = makeTriangle();
        proxyGeometry->setNodeMask(MWRender::Mask_GpuProxy);
        proxy->addChild(proxyGeometry);
        root->addChild(proxy);

        osg::ref_ptr<NifOsg::MatrixTransform> bare = new NifOsg::MatrixTransform;
        root->addChild(bare);

        osg::ref_ptr<NifOsg::MatrixTransform> mixed = new NifOsg::MatrixTransform;
        osg::ref_ptr<NifOsg::MatrixTransform> nestedProxy = new NifOsg::MatrixTransform;
        osg::ref_ptr<osg::Geometry> nestedGeometry = makeTriangle();
        nestedGeometry->setNodeMask(MWRender::Mask_GpuProxy);
        nestedProxy->addChild(nestedGeometry);
        mixed->addChild(nestedProxy);
        mixed->addChild(makeTriangle());
        root->addChild(mixed);

        AnimationFixture animation(root);
        animation.maskBareBones();

        EXPECT_EQ(proxy->getNodeMask(), MWRender::Mask_GpuProxy);
        EXPECT_EQ(bare->getNodeMask(), MWRender::Mask_UpdateVisitor);
        EXPECT_EQ(mixed->getNodeMask(), ~0u);
        EXPECT_EQ(nestedProxy->getNodeMask(), MWRender::Mask_GpuProxy);

        proxyGeometry->setNodeMask(~0u);
        nestedGeometry->setNodeMask(~0u);
        animation.pruneBareBones();

        EXPECT_EQ(proxy->getNodeMask(), ~0u);
        EXPECT_EQ(nestedProxy->getNodeMask(), ~0u);
    }

    TEST(AnimationTest, GpuProxyKeepsUpdates)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<NifOsg::MatrixTransform> proxy = new NifOsg::MatrixTransform;
        osg::ref_ptr<osg::Geometry> geometry = makeTriangle();
        geometry->setNodeMask(MWRender::Mask_GpuProxy);
        osg::ref_ptr<CountCallback> callback = new CountCallback;
        proxy->setUpdateCallback(callback);
        proxy->addChild(geometry);
        root->addChild(proxy);

        AnimationFixture animation(root);
        animation.maskBareBones();

        osgUtil::UpdateVisitor update;
        EXPECT_EQ(update.getTraversalMask(), ~0u);
        root->accept(update);
        EXPECT_EQ(callback->mCount, 1u);

        update.setTraversalMask(~MWRender::Mask_GpuProxy);
        root->accept(update);
        EXPECT_EQ(callback->mCount, 1u);
    }

    TEST(AnimationTest, DetachesOldGpuSceneBeforeAttachingNewScene)
    {
        alignas(std::max_align_t) std::array<std::byte, 1> firstSceneStorage;
        alignas(std::max_align_t) std::array<std::byte, 1> secondSceneStorage;
        auto* firstScene = reinterpret_cast<MWRender::GpuScene*>(firstSceneStorage.data());
        auto* secondScene = reinterpret_cast<MWRender::GpuScene*>(secondSceneStorage.data());

        GpuSceneLifecycleFixture animation;
        animation.setGpuScene(firstScene);
        animation.setGpuScene(firstScene);
        animation.setGpuScene(secondScene);
        animation.setGpuScene(nullptr);

        const std::vector<std::string_view> expected{ "refresh:scene", "refresh:scene", "detach:scene", "refresh:scene",
            "detach:scene", "refresh:null" };
        EXPECT_EQ(animation.events(), expected);
    }
}
