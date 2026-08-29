#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/optimizeroptions.hpp>

#include <gtest/gtest.h>

namespace
{
    constexpr unsigned defaults = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
        | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;
}

TEST(OptimizerOptionsTest, PreservesDefaultsWithoutControls)
{
    EXPECT_EQ(SceneUtil::parseOptimizationOptions({}, defaults), defaults);
}

TEST(OptimizerOptionsTest, EnablesVertexOrderingForQualification)
{
    const unsigned result
        = SceneUtil::parseOptimizationOptions("VERTEX_POSTTRANSFORM VERTEX_PRETRANSFORM", defaults);
    EXPECT_NE(result & SceneUtil::Optimizer::VERTEX_POSTTRANSFORM, 0u);
    EXPECT_NE(result & SceneUtil::Optimizer::VERTEX_PRETRANSFORM, 0u);
}

TEST(OptimizerOptionsTest, DisablesNamedDefaultsWithoutToggling)
{
    const unsigned result = SceneUtil::parseOptimizationOptions("~MERGE_GEOMETRY", defaults);
    EXPECT_EQ(result & SceneUtil::Optimizer::MERGE_GEOMETRY, 0u);
    EXPECT_NE(result & SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS, 0u);
}

TEST(OptimizerOptionsTest, ClearsDefaultsBeforeExplicitEnables)
{
    const unsigned result = SceneUtil::parseOptimizationOptions("OFF VERTEX_POSTTRANSFORM", defaults);
    EXPECT_EQ(result, SceneUtil::Optimizer::VERTEX_POSTTRANSFORM);
}
