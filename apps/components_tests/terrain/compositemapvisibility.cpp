#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include <gtest/gtest.h>

TEST(CompositeMapVisibilityTest, PromotesAQueuedMapAtMostOnce)
{
    const osg::ref_ptr<Terrain::CompositeMapRenderer> renderer = new Terrain::CompositeMapRenderer;
    const osg::ref_ptr<Terrain::CompositeMap> map = new Terrain::CompositeMap;
    osg::ref_ptr<Terrain::TerrainDrawable> drawable = new Terrain::TerrainDrawable;
    drawable->setCompositeMap(map);
    drawable->setCompositeMapRenderer(renderer);
    renderer->addCompositeMap(map, false);

    ASSERT_EQ(renderer->getCompileSetSize(), 1u);
    drawable->requireCompositeMap();
    EXPECT_EQ(renderer->getCompileSetSize(), 0u);
    drawable->requireCompositeMap();
    EXPECT_EQ(renderer->getCompileSetSize(), 0u);
}
