#include <gtest/gtest.h>

#include <osg/Group>

#include <apps/openmw/mwrender/objectpaging.hpp>

namespace
{
    /// Keep classified material effects on their live renderer path at every paging distance.
    TEST(ObjectPagingTest, RejectsSurfaceEffectTemplates)
    {
        osg::ref_ptr<osg::Group> node = new osg::Group;
        EXPECT_TRUE(MWRender::objectPagingCanRepresent(*node));

        node->setUserValue("surfaceEffects", true);
        EXPECT_FALSE(MWRender::objectPagingCanRepresent(*node));

        node->setUserValue("surfaceEffects", false);
        EXPECT_TRUE(MWRender::objectPagingCanRepresent(*node));
    }
}
