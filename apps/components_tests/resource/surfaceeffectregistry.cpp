#include <gtest/gtest.h>

#include <components/resource/surfaceeffectregistry.hpp>
#include <components/testing/util.hpp>

namespace
{
    using VFS::Path::Normalized;
    using VFS::Path::NormalizedView;

    /// Verify selectors combine across fields while each texture list remains an OR group.
    TEST(SurfaceEffectRegistryTest, MatchesCombinedMaterialSignalsCaseInsensitively)
    {
        TestingOpenMW::VFSTestFile rules(R"(
version: 1
profiles:
  calm:
    effect: water
    geometric-wave-height: 0
rules:
  - profile: calm
    mesh: meshes/x/ex_vivec_*.nif
    material: "*water_01*"
    textures: [textures/water/*.dds, textures/tr/env/tr_water*.dds]
)");
        const auto vfs = TestingOpenMW::createTestVFS({ { NormalizedView("material-effects/water.yaml"), &rules } });
        const Resource::SurfaceEffectRegistry registry(*vfs);

        const auto match = registry.match(NormalizedView("meshes/x/ex_vivec_p_water_01.nif"), "Tri Ex_Vivec_P_Water_01",
            { Normalized("textures/TR/env/TR_Water_Still.dds") });
        ASSERT_TRUE(match.has_value());
        EXPECT_EQ(match->mId, "calm");
        EXPECT_FALSE(registry.match(NormalizedView("meshes/x/other.nif"), "Tri Ex_Vivec_P_Water_01",
            { Normalized("textures/tr/env/tr_water_still.dds") }));
    }

    /// Verify later rules can deliberately refine a broad community convention.
    TEST(SurfaceEffectRegistryTest, LastMatchingRuleWins)
    {
        TestingOpenMW::VFSTestFile rules(R"(
version: 1
profiles:
  broad: {effect: water, geometric-wave-height: 0}
  local: {effect: water, geometric-wave-height: 12}
rules:
  - {profile: broad, textures: [textures/water/*.dds]}
  - {profile: local, mesh: meshes/custom/*.nif, textures: [textures/water/*.dds]}
)");
        const auto vfs = TestingOpenMW::createTestVFS({ { NormalizedView("material-effects/water.yaml"), &rules } });
        const Resource::SurfaceEffectRegistry registry(*vfs);

        const auto match = registry.match(
            NormalizedView("meshes/custom/canal.nif"), "surface", { Normalized("textures/water/canal.dds") });
        ASSERT_TRUE(match.has_value());
        EXPECT_EQ(match->mId, "local");
        ASSERT_TRUE(match->mGeometricWaveHeight.has_value());
        EXPECT_FLOAT_EQ(*match->mGeometricWaveHeight, 12.f);
    }

    /// Verify profiles without an override defer to the renderer-owned mesh-water wave setting.
    TEST(SurfaceEffectRegistryTest, LeavesMeshWaveHeightUnspecifiedForGlobalControl)
    {
        TestingOpenMW::VFSTestFile rules(R"(
version: 1
profiles:
  inherited: {effect: water}
rules:
  - {profile: inherited, textures: [textures/water/*.dds]}
)");
        const auto vfs = TestingOpenMW::createTestVFS({ { NormalizedView("material-effects/water.yaml"), &rules } });
        const Resource::SurfaceEffectRegistry registry(*vfs);

        const auto match
            = registry.match(NormalizedView("meshes/canal.nif"), "surface", { Normalized("textures/water/canal.dds") });
        ASSERT_TRUE(match.has_value());
        EXPECT_FALSE(match->mGeometricWaveHeight.has_value());
    }

    /// Verify malformed documents cannot create an unbounded match-all classification rule.
    TEST(SurfaceEffectRegistryTest, RejectsRulesWithoutSelectors)
    {
        TestingOpenMW::VFSTestFile rules(R"(
version: 1
profiles:
  calm: {effect: water}
rules:
  - {profile: calm}
)");
        const auto vfs = TestingOpenMW::createTestVFS({ { NormalizedView("material-effects/invalid.yaml"), &rules } });
        const Resource::SurfaceEffectRegistry registry(*vfs);

        EXPECT_EQ(registry.ruleCount(), 0u);
        EXPECT_FALSE(registry.match(NormalizedView("meshes/anything.nif"), "anything", {}));
    }

    /// Verify VFS document paths, rather than iterator construction order, define cross-mod precedence.
    TEST(SurfaceEffectRegistryTest, SortsDocumentsBeforeApplyingLastMatchPrecedence)
    {
        TestingOpenMW::VFSTestFile early(R"(
version: 1
profiles:
  early: {effect: water, geometric-wave-height: 0}
rules:
  - {profile: early, textures: [textures/water/*.dds]}
)");
        TestingOpenMW::VFSTestFile late(R"(
version: 1
profiles:
  late: {effect: water, geometric-wave-height: 8}
rules:
  - {profile: late, textures: [textures/water/*.dds]}
)");
        const auto vfs = TestingOpenMW::createTestVFS({ { NormalizedView("material-effects/z-last.yaml"), &late },
            { NormalizedView("material-effects/a-first.yaml"), &early } });
        const Resource::SurfaceEffectRegistry registry(*vfs);

        const auto match
            = registry.match(NormalizedView("meshes/canal.nif"), "surface", { Normalized("textures/water/canal.dds") });
        ASSERT_TRUE(match.has_value());
        EXPECT_EQ(match->mId, "late");
    }

    /// Verify unsupported documents contribute neither profiles nor otherwise-valid rules.
    TEST(SurfaceEffectRegistryTest, RejectsUnsupportedDocumentVersions)
    {
        TestingOpenMW::VFSTestFile rules(R"(
version: 2
profiles:
  future: {effect: water}
rules:
  - {profile: future, textures: [textures/water/*.dds]}
)");
        const auto vfs = TestingOpenMW::createTestVFS({ { NormalizedView("material-effects/future.yaml"), &rules } });
        const Resource::SurfaceEffectRegistry registry(*vfs);

        EXPECT_EQ(registry.profileCount(), 0u);
        EXPECT_EQ(registry.ruleCount(), 0u);
    }
}
