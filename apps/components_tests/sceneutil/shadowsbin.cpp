#include <components/sceneutil/shadowsbin.hpp>

#include <osg/Geometry>
#include <osg/Matrix>
#include <osg/StateSet>
#include <osgUtil/RenderLeaf>
#include <osgUtil/StateGraph>

#include <gtest/gtest.h>

namespace
{
    TEST(ShadowsBinTest, OrdersEquivalentOpaqueCastersFrontToBack)
    {
        SceneUtil::ShadowsBin::CastingPrograms castingPrograms;
        osg::ref_ptr<SceneUtil::ShadowsBin> bin = new SceneUtil::ShadowsBin(castingPrograms, nullptr);

        osg::ref_ptr<osg::StateSet> rootStateSet = new osg::StateSet;
        rootStateSet->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        osg::ref_ptr<osgUtil::StateGraph> root = new osgUtil::StateGraph(nullptr, rootStateSet);
        osg::ref_ptr<osg::StateSet> casterStateSet = new osg::StateSet;
        osg::ref_ptr<osgUtil::StateGraph> casters = new osgUtil::StateGraph(root, casterStateSet);

        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::RefMatrix> projection = new osg::RefMatrix;
        osg::ref_ptr<osg::RefMatrix> modelView = new osg::RefMatrix;
        for (const float depth : { 10.f, 2.f, 5.f })
            casters->addLeaf(new osgUtil::RenderLeaf(geometry, projection, modelView, depth));
        bin->addStateGraph(casters);

        bin->sort();

        const osgUtil::RenderBin::StateGraphList& orderedGraphs = bin->getStateGraphList();
        ASSERT_EQ(orderedGraphs.size(), 1u);
        const osgUtil::StateGraph::LeafList& ordered = orderedGraphs.front()->_leaves;
        ASSERT_EQ(ordered.size(), 3u);
        EXPECT_FLOAT_EQ(ordered[0]->_depth, 2.f);
        EXPECT_FLOAT_EQ(ordered[1]->_depth, 5.f);
        EXPECT_FLOAT_EQ(ordered[2]->_depth, 10.f);
    }
}
