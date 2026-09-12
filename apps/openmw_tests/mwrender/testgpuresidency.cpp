#include <apps/openmw/mwrender/animation.hpp>

#include <components/misc/constants.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <gtest/gtest.h>

#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/StateSet>
#include <osg/Switch>
#include <osg/Uniform>

namespace
{
    using MWRender::hasGeometryUpdaters;

    class NoopCallback : public osg::NodeCallback
    {
    public:
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override { traverse(node, nv); }
    };

    class NoopUniformCallback : public osg::UniformCallback
    {
    public:
        void operator()(osg::Uniform*, osg::NodeVisitor*) override {}
    };

    osg::ref_ptr<osg::Group> makeStatic()
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
        transform->addChild(new osg::Geometry);
        root->addChild(transform);
        return root;
    }

    TEST(MWRenderGpuResidency, staticGeometryIsSafe)
    {
        EXPECT_FALSE(hasGeometryUpdaters(*makeStatic()));
    }

    TEST(MWRenderGpuResidency, rigGeometryBlocksResidency)
    {
        osg::ref_ptr<SceneUtil::Skeleton> skeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform;
        bone->addChild(new SceneUtil::RigGeometry);
        skeleton->addChild(bone);
        EXPECT_TRUE(hasGeometryUpdaters(*skeleton));
    }

    TEST(MWRenderGpuResidency, boneControllerBlocksResidency)
    {
        osg::ref_ptr<osg::Group> root = makeStatic();
        root->getChild(0)->addUpdateCallback(new NoopCallback);
        EXPECT_TRUE(hasGeometryUpdaters(*root));
    }

    TEST(MWRenderGpuResidency, updatingStateBlocksResidency)
    {
        osg::ref_ptr<osg::Group> root = makeStatic();
        osg::ref_ptr<osg::Uniform> uniform = new osg::Uniform("value", 0.f);
        uniform->setUpdateCallback(new NoopUniformCallback);
        root->getChild(0)->getOrCreateStateSet()->addUniform(uniform);
        EXPECT_TRUE(hasGeometryUpdaters(*root));
    }

    TEST(MWRenderGpuResidency, lightControllerAllowsResidency)
    {
        osg::ref_ptr<osg::Group> root = makeStatic();
        osg::ref_ptr<SceneUtil::LightSource> light = new SceneUtil::LightSource;
        light->addUpdateCallback(new SceneUtil::LightController);
        root->addChild(light);
        EXPECT_FALSE(hasGeometryUpdaters(*root));
    }

    TEST(MWRenderGpuResidency, trackedSwitchAllowsResidency)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Switch> sw = new osg::Switch;
        sw->setName(Constants::NightDayLabel);
        sw->addChild(new osg::Geometry);
        sw->addUpdateCallback(new NoopCallback);
        root->addChild(sw);
        EXPECT_FALSE(hasGeometryUpdaters(*root));
    }

    TEST(MWRenderGpuResidency, otherSwitchBlocksResidency)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Switch> sw = new osg::Switch;
        sw->addChild(new osg::Geometry);
        sw->addUpdateCallback(new NoopCallback);
        root->addChild(sw);
        EXPECT_TRUE(hasGeometryUpdaters(*root));
    }

    TEST(MWRenderGpuResidency, trackedRigBlocksResidency)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Switch> sw = new osg::Switch;
        sw->setName(Constants::NightDayLabel);
        sw->addUpdateCallback(new NoopCallback);
        sw->addChild(new SceneUtil::RigGeometry);
        root->addChild(sw);
        EXPECT_TRUE(hasGeometryUpdaters(*root));
    }
}
