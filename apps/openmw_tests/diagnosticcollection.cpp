#include <apps/openmw/diagnosticcollection.hpp>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <limits>
#include <optional>
#include <stdexcept>

TEST(DiagnosticCollectionTest, EnablesOnlyOneBoundedFutureIntervalAndRestoresPriorCategories)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    viewer.setSceneData(new osg::Group);
    viewer.getViewerStats()->collectStats("frame_rate", true);

    OMW::DiagnosticCollection collection(viewer);
    EXPECT_FALSE(collection.active());
    EXPECT_FALSE(viewer.getViewerStats()->collectStats("scene"));
    EXPECT_FALSE(viewer.getCamera()->getStats()->collectStats("rendering"));

    EXPECT_TRUE(collection.start(17, 4, 100));
    EXPECT_TRUE(collection.active());
    EXPECT_TRUE(viewer.getViewerStats()->collectStats("scene"));
    EXPECT_TRUE(viewer.getCamera()->getStats()->collectStats("rendering"));
    EXPECT_FALSE(collection.start(18, 1, 100));
    EXPECT_FALSE(collection.update(110).has_value());
    EXPECT_FALSE(collection.update(111).has_value());
    EXPECT_FALSE(collection.update(112).has_value());

    const std::optional<OMW::CompletedDiagnosticCollection> completed = collection.update(113);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->mRequestId, 17);
    EXPECT_EQ(completed->mFirstFrame, 100);
    EXPECT_EQ(completed->mLastFrame, 103);
    EXPECT_FALSE(collection.active());
    EXPECT_FALSE(viewer.getViewerStats()->collectStats("scene"));
    EXPECT_FALSE(viewer.getCamera()->getStats()->collectStats("rendering"));
    EXPECT_TRUE(viewer.getViewerStats()->collectStats("frame_rate"));
}

TEST(DiagnosticCollectionTest, RejectsInvalidAndOverflowingIntervals)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    OMW::DiagnosticCollection collection(viewer);

    EXPECT_THROW(collection.start(1, 0, 10), std::invalid_argument);
    EXPECT_THROW(collection.start(1, 601, 10), std::invalid_argument);
    EXPECT_THROW(collection.start(1, 2, std::numeric_limits<unsigned>::max()), std::overflow_error);
}
