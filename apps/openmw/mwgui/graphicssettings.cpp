#include "graphicssettings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include <components/l10n/manager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/graphicssettingsvisibility.hpp>
#include <components/settings/shadowresolution.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "layout.hpp"

namespace
{
    constexpr std::array<std::string_view, 5> sShadowFilteringQualities{ "legacy", "low", "medium", "high", "ultra" };
    constexpr std::array<std::string_view, 4> sDistantShadowQualities{ "low", "medium", "high", "ultra" };
    constexpr std::array<std::string_view, 4> sWaterAppearances{ "openmw", "mge-xe", "enhanced", "vanilla" };
    constexpr std::array<std::string_view, 3> sEnhancedWaterColors{ "green-blue", "blue", "green" };
}

MWGui::GraphicsSettings::GraphicsSettings(Layout& layout, std::function<void()> apply)
    : mLayout(layout)
    , mApply(std::move(apply))
    , mCategorySelector(layout.getWidget("GraphicsCategory")->castType<MyGUI::ComboBox>())
    , mPages{ { { layout.getWidget("DisplayPage"), layout.getWidget("DisplayScrollView")->castType<MyGUI::ScrollView>(),
                    layout.getWidget("DisplayContent") },
          { layout.getWidget("VisibilityPage"), layout.getWidget("VisibilityScrollView")->castType<MyGUI::ScrollView>(),
              layout.getWidget("VisibilityContent") },
          { layout.getWidget("AtmospherePage"), layout.getWidget("AtmosphereScrollView")->castType<MyGUI::ScrollView>(),
              layout.getWidget("AtmosphereContent") },
          { layout.getWidget("WaterPage"), layout.getWidget("WaterScrollView")->castType<MyGUI::ScrollView>(),
              layout.getWidget("WaterContent") },
          { layout.getWidget("LightingPage"), layout.getWidget("LightingScrollView")->castType<MyGUI::ScrollView>(),
              layout.getWidget("LightingContent") },
          { layout.getWidget("ShadowsPage"), layout.getWidget("ShadowsScrollView")->castType<MyGUI::ScrollView>(),
              layout.getWidget("ShadowsContent") },
          { layout.getWidget("AdvancedPage"), layout.getWidget("AdvancedScrollView")->castType<MyGUI::ScrollView>(),
              layout.getWidget("AdvancedContent") } } }
    , mDistanceSettings(std::make_unique<DistanceSettings>(layout, mApply))
    , mWindowBorderRow(layout.getWidget("WindowBorderRow"))
    , mDisplayResolutionGroup(layout.getWidget("DisplayResolutionGroup"))
    , mWindowModeHint(layout.getWidget("WindowModeHint"))
    , mAtmosphereAppearanceGroup(layout.getWidget("AtmosphereAppearanceGroup"))
    , mAtmosphereQualityGroup(layout.getWidget("AtmosphereQualityGroup"))
    , mCloudShadowsButton(layout.getWidget("CloudShadowsButton")->castType<MyGUI::Button>())
    , mCloudShadowIntensity(layout.getWidget("CloudShadowIntensity")->castType<MyGUI::ScrollBar>())
    , mReflectCloudsButton(layout.getWidget("ReflectCloudsButton")->castType<MyGUI::Button>())
    , mVolumetricQuality(layout.getWidget("VolumetricQuality")->castType<MyGUI::ComboBox>())
    , mAtmosphereAppearance(layout.getWidget("AtmosphereAppearance")->castType<MyGUI::ComboBox>())
    , mWaterAppearance(layout.getWidget("WaterAppearance")->castType<MyGUI::ComboBox>())
    , mGeometricWaveHeight(layout.getWidget("GeometricWaveHeight")->castType<MyGUI::ScrollBar>())
    , mMeshWater(layout.getWidget("MeshWaterButton")->castType<MyGUI::Button>())
    , mMeshGeometricWaveHeight(layout.getWidget("MeshGeometricWaveHeight")->castType<MyGUI::ScrollBar>())
    , mWaterCaustics(layout.getWidget("WaterCausticsButton")->castType<MyGUI::Button>())
    , mWaterCausticsStrength(layout.getWidget("WaterCausticsStrength")->castType<MyGUI::ScrollBar>())
    , mInteriorWaterLights(layout.getWidget("InteriorWaterLights")->castType<MyGUI::ScrollBar>())
    , mExteriorWaterLights(layout.getWidget("ExteriorWaterLights")->castType<MyGUI::ScrollBar>())
    , mEnhancedWaterColor(layout.getWidget("EnhancedWaterColor")->castType<MyGUI::ComboBox>())
    , mEnhancedWaterButtons{ layout.getWidget("EnhancedWaterFoamButton")->castType<MyGUI::Button>(),
        layout.getWidget("EnhancedWaterDistantWavesButton")->castType<MyGUI::Button>(),
        layout.getWidget("EnhancedWaterSewerRipplesButton")->castType<MyGUI::Button>() }
    , mWaterShaderControls(layout.getWidget("WaterShaderControls"))
    , mMeshWaterControls(layout.getWidget("MeshWaterControls"))
    , mEnhancedWaterControls(layout.getWidget("EnhancedWaterControls"))
    , mWaterCausticsStrengthGroup(layout.getWidget("WaterCausticsStrengthGroup"))
    , mShadowFilteringQuality(layout.getWidget("ShadowFilteringQuality")->castType<MyGUI::ComboBox>())
    , mDistantShadowsButton(layout.getWidget("DistantShadowsButton")->castType<MyGUI::Button>())
    , mDistantShadowQuality(layout.getWidget("DistantShadowQuality")->castType<MyGUI::ComboBox>())
    , mShadowMapResolution(layout.getWidget("ShadowMapResolution")->castType<MyGUI::EditBox>())
    , mShadowResolutionDecrease(layout.getWidget("ShadowResolutionDecrease")->castType<MyGUI::Button>())
    , mShadowResolutionIncrease(layout.getWidget("ShadowResolutionIncrease")->castType<MyGUI::Button>())
    , mShadowFilteringGroup(layout.getWidget("ShadowFilteringGroup"))
    , mDistantShadowsGroup(layout.getWidget("DistantShadowsGroup"))
    , mDistantShadowQualityGroup(layout.getWidget("DistantShadowQualityGroup"))
    , mCloudShadowIntensityGroup(layout.getWidget("CloudShadowIntensityGroup"))
    , mGeometryShadowControls(layout.getWidget("GeometryShadowControls"))
    , mOptimizationsEnabled(layout.getWidget("OptimizationsEnabledButton")->castType<MyGUI::Button>())
    , mOptimizationButtons{ layout.getWidget("GpuDrivenPagingButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuSkinningButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuOcclusionButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuActorsButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuTextureArraysButton")->castType<MyGUI::Button>(),
        layout.getWidget("IndexedLightListsButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuSceneLightingButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuStorageBudgetButton")->castType<MyGUI::Button>(),
        layout.getWidget("LowEndIndirectBatchingButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuLightOcclusionButton")->castType<MyGUI::Button>(),
        layout.getWidget("GpuDistantPagingButton")->castType<MyGUI::Button>(),
        layout.getWidget("SkipDormantRipplesButton")->castType<MyGUI::Button>() }
{
    selectCategory(0);
    mCategorySelector->eventComboChangePosition += MyGUI::newDelegate(this, &GraphicsSettings::onCategoryChanged);
    mVolumetricQuality->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onVolumetricQualityChanged);
    mShadowFilteringQuality->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onShadowFilteringQualityChanged);
    mDistantShadowQuality->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onDistantShadowQualityChanged);
    mAtmosphereAppearance->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onAtmosphereAppearanceChanged);
    mWaterAppearance->eventComboChangePosition += MyGUI::newDelegate(this, &GraphicsSettings::onWaterAppearanceChanged);
    mMeshWater->eventMouseButtonClick += MyGUI::newDelegate(this, &GraphicsSettings::onMeshWaterChanged);
    mEnhancedWaterColor->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onEnhancedWaterColorChanged);
    mWaterCaustics->eventMouseButtonClick += MyGUI::newDelegate(this, &GraphicsSettings::onWaterCausticsChanged);

    for (const std::string_view name :
        { "ActorShadowsButton", "PlayerShadowsButton", "TerrainShadowsButton", "ObjectShadowsButton" })
        layout.getWidget(name)->eventMouseButtonClick
            += MyGUI::newDelegate(this, &GraphicsSettings::onShadowCasterChanged);
    mShadowMapResolution->eventEditSelectAccept
        += MyGUI::newDelegate(this, &GraphicsSettings::onShadowResolutionAccepted);
    mShadowMapResolution->eventKeyLostFocus += MyGUI::newDelegate(this, &GraphicsSettings::onShadowResolutionFocusLost);
    mShadowResolutionDecrease->eventMouseButtonClick
        += MyGUI::newDelegate(this, &GraphicsSettings::onShadowResolutionStep);
    mShadowResolutionIncrease->eventMouseButtonClick
        += MyGUI::newDelegate(this, &GraphicsSettings::onShadowResolutionStep);
    mOptimizationsEnabled->eventMouseButtonClick
        += MyGUI::newDelegate(this, &GraphicsSettings::onOptimizationPrerequisiteChanged);

    refresh();
}

void MWGui::GraphicsSettings::refresh()
{
    mDistanceSettings->refresh();
    refreshDisplayControls();
    refreshShadowResolution();
    refreshAtmosphereControls();
    refreshOptimizationControls();
    refreshWaterControls();
    updateLayout();
}

void MWGui::GraphicsSettings::setDistanceValues(
    bool useCellEditor, float view, float grass, float shadow, std::optional<int> resolution)
{
    mDistanceSettings->setValues(useCellEditor, view, grass, shadow);
    if (!resolution)
        return;

    const int maximum = SceneUtil::ShadowManager::getMaximumShadowMapResolution();
    if (!Settings::parseShadowResolution(std::to_string(*resolution), maximum))
        throw std::invalid_argument("diagnostic shadow resolution exceeds graphics hardware limits");
    mShadowMapResolution->setCaption(std::to_string(*resolution));
    mShadowMapResolution->eventEditSelectAccept(mShadowMapResolution);
    for (auto* arrow : { mShadowResolutionDecrease, mShadowResolutionIncrease })
    {
        const int expected = Settings::stepShadowResolution(*resolution, arrow == mShadowResolutionIncrease, maximum);
        arrow->eventMouseButtonClick(arrow);
        if (Settings::shadows().mShadowMapResolution != expected)
            throw std::runtime_error("shadow resolution arrow did not apply the expected pixel step");
        mShadowMapResolution->setCaption(std::to_string(*resolution));
        mShadowMapResolution->eventEditSelectAccept(mShadowMapResolution);
    }
    if (Settings::shadows().mShadowMapResolution != *resolution)
        throw std::runtime_error("shadow resolution entry did not retain the requested size");
}

void MWGui::GraphicsSettings::setFogValues(
    bool useCellEditor, bool enabled, float start, float end, bool startLocked, bool endLocked)
{
    mDistanceSettings->setFogValues(useCellEditor, enabled, start, end, startLocked, endLocked);
}

void MWGui::GraphicsSettings::setGeometricWaveHeight(float height)
{
    if (!std::isfinite(height) || height < 0.f || height > 250.f || height != std::round(height))
        throw std::invalid_argument("diagnostic geometric wave height must be a whole number from 0 to 250");
    if (Settings::openmwseWater().mGeometricWaveHeight.get() == height)
        return;
    const std::size_t position = static_cast<std::size_t>(height);
    mGeometricWaveHeight->setScrollPosition(position);
    mGeometricWaveHeight->eventScrollChangePosition(mGeometricWaveHeight, position);
}

void MWGui::GraphicsSettings::setWaterValues(std::string_view appearance, float height, bool caustics,
    float causticsStrength, int interiorLocalLights, int exteriorLocalLights, bool meshWater, float meshHeight)
{
    if (!std::isfinite(causticsStrength) || causticsStrength < 0.f || causticsStrength > 2.f)
        throw std::invalid_argument("diagnostic caustics strength must be between 0 and 2");
    if (interiorLocalLights < 0 || interiorLocalLights > 4 || exteriorLocalLights < 0 || exteriorLocalLights > 4)
        throw std::invalid_argument("diagnostic local water-light budgets must be between 0 and 4");
    const auto appearanceIt = std::find(sWaterAppearances.begin(), sWaterAppearances.end(), appearance);
    if (appearanceIt == sWaterAppearances.end())
        throw std::invalid_argument("diagnostic water appearance is not recognized");
    const std::size_t appearancePosition = static_cast<std::size_t>(appearanceIt - sWaterAppearances.begin());
    auto& water = Settings::openmwseWater();
    if (water.mAppearance.get() != appearance)
    {
        mWaterAppearance->setIndexSelected(appearancePosition);
        mWaterAppearance->eventComboChangePosition(mWaterAppearance, appearancePosition);
    }
    setGeometricWaveHeight(height);
    if (water.mMeshGeometricWaveHeight.get() != meshHeight)
    {
        const std::size_t meshHeightPosition = static_cast<std::size_t>(meshHeight);
        mMeshGeometricWaveHeight->setScrollPosition(meshHeightPosition);
        mMeshGeometricWaveHeight->eventScrollChangePosition(mMeshGeometricWaveHeight, meshHeightPosition);
    }
    if (water.mMeshWater.get() != meshWater)
    {
        water.mMeshWater.set(meshWater);
        mMeshWater->setCaptionWithReplacing(meshWater ? "#{Interface:On}" : "#{Interface:Off}");
        mApply();
    }
    if (water.mCaustics.get() != caustics)
    {
        water.mCaustics.set(caustics);
        mWaterCaustics->setCaptionWithReplacing(caustics ? "#{Interface:On}" : "#{Interface:Off}");
        mApply();
    }
    if (water.mCausticsStrength.get() != causticsStrength)
    {
        const std::size_t strengthPosition = static_cast<std::size_t>(std::round(causticsStrength * 100.f));
        mWaterCausticsStrength->setScrollPosition(strengthPosition);
        mWaterCausticsStrength->eventScrollChangePosition(mWaterCausticsStrength, strengthPosition);
    }
    if (water.mInteriorLocalLights.get() != interiorLocalLights)
    {
        mInteriorWaterLights->setScrollPosition(static_cast<std::size_t>(interiorLocalLights));
        mInteriorWaterLights->eventScrollChangePosition(
            mInteriorWaterLights, static_cast<std::size_t>(interiorLocalLights));
    }
    if (water.mExteriorLocalLights.get() != exteriorLocalLights)
    {
        mExteriorWaterLights->setScrollPosition(static_cast<std::size_t>(exteriorLocalLights));
        mExteriorWaterLights->eventScrollChangePosition(
            mExteriorWaterLights, static_cast<std::size_t>(exteriorLocalLights));
    }
    refreshWaterControls();
}

void MWGui::GraphicsSettings::onShadowCasterChanged(MyGUI::Widget*)
{
    auto& shadows = Settings::shadows();
    shadows.mEnableShadows.set(
        shadows.mActorShadows || shadows.mPlayerShadows || shadows.mTerrainShadows || shadows.mObjectShadows);
    mApply();
}

void MWGui::GraphicsSettings::onShadowResolutionAccepted(MyGUI::EditBox* sender)
{
    auto& setting = Settings::shadows().mShadowMapResolution;
    const std::string text = sender->getOnlyText();
    if (text == std::to_string(setting.get()))
        return;
    const int maximum = SceneUtil::ShadowManager::getMaximumShadowMapResolution();
    const auto value = Settings::parseShadowResolution(text, maximum);
    if (!value)
    {
        refreshShadowResolution();
        auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
        MWBase::Environment::get().getWindowManager()->messageBox(
            l10n->formatMessage("ShadowResolutionInvalid", { "maximum" }, { maximum }));
        return;
    }
    if (*value != setting.get())
    {
        setting.set(*value);
        mApply();
    }
    refreshShadowResolution();
}

void MWGui::GraphicsSettings::onShadowResolutionFocusLost(MyGUI::Widget* sender, MyGUI::Widget*)
{
    onShadowResolutionAccepted(sender->castType<MyGUI::EditBox>());
}

void MWGui::GraphicsSettings::onShadowResolutionStep(MyGUI::Widget* sender)
{
    const int value = Settings::stepShadowResolution(Settings::shadows().mShadowMapResolution,
        sender == mShadowResolutionIncrease, SceneUtil::ShadowManager::getMaximumShadowMapResolution());
    mShadowMapResolution->setCaption(std::to_string(value));
    onShadowResolutionAccepted(mShadowMapResolution);
}

void MWGui::GraphicsSettings::onVolumetricQualityChanged(MyGUI::ComboBox* sender, std::size_t position)
{
    if (position >= sender->getItemCount())
        return;
    Settings::shaders().mAtmosphericQuality.set(static_cast<int>(position));
    mApply();
}

void MWGui::GraphicsSettings::onShadowFilteringQualityChanged(MyGUI::ComboBox* sender, std::size_t position)
{
    if (position >= sender->getItemCount() || position >= sShadowFilteringQualities.size())
        return;
    Settings::openmwseShadows().mFilteringQuality.set(std::string(sShadowFilteringQualities[position]));
    mApply();
}

void MWGui::GraphicsSettings::onDistantShadowQualityChanged(MyGUI::ComboBox* sender, std::size_t position)
{
    if (position >= sender->getItemCount() || position >= sDistantShadowQualities.size())
        return;
    Settings::openmwseShadows().mDistantShadowQuality.set(std::string(sDistantShadowQualities[position]));
    mApply();
}

void MWGui::GraphicsSettings::onAtmosphereAppearanceChanged(MyGUI::ComboBox* sender, std::size_t position)
{
    if (position >= sender->getItemCount())
        return;
    Settings::shaders().mAtmosphereAppearance.set(static_cast<int>(position));
    mApply();
}

void MWGui::GraphicsSettings::onWaterAppearanceChanged(MyGUI::ComboBox* sender, std::size_t position)
{
    if (position >= sender->getItemCount() || position >= sWaterAppearances.size())
        return;
    Settings::openmwseWater().mAppearance.set(std::string(sWaterAppearances[position]));
    mApply();
    refreshWaterControls();
}

void MWGui::GraphicsSettings::onMeshWaterChanged(MyGUI::Widget*)
{
    refreshWaterControls();
}

void MWGui::GraphicsSettings::onEnhancedWaterColorChanged(MyGUI::ComboBox* sender, std::size_t position)
{
    if (position >= sender->getItemCount() || position >= sEnhancedWaterColors.size())
        return;
    Settings::openmwseWater().mEnhancedColor.set(std::string(sEnhancedWaterColors[position]));
    mApply();
}

void MWGui::GraphicsSettings::onWaterCausticsChanged(MyGUI::Widget*)
{
    refreshWaterControls();
}

void MWGui::GraphicsSettings::onOptimizationPrerequisiteChanged(MyGUI::Widget*)
{
    refreshOptimizationControls();
}

void MWGui::GraphicsSettings::refreshShadowResolution()
{
    const int value = Settings::shadows().mShadowMapResolution;
    const int maximum = SceneUtil::ShadowManager::getMaximumShadowMapResolution();
    mShadowMapResolution->setCaption(std::to_string(value));
    mShadowMapResolution->setTextCursor(0);
    mShadowResolutionDecrease->setEnabled(maximum > 0 && value > 1);
    mShadowResolutionIncrease->setEnabled(maximum > 0 && value < maximum);
}

void MWGui::GraphicsSettings::refreshAtmosphereControls()
{
    const bool clouds = Settings::shaders().mVolumetricClouds;
    mCloudShadowsButton->setCaptionWithReplacing(
        Settings::shadows().mCloudShadows ? "#{Interface:On}" : "#{Interface:Off}");
    mCloudShadowsButton->setEnabled(true);
    mReflectCloudsButton->setEnabled(Settings::water().mShader);
    mVolumetricQuality->setIndexSelected(Settings::shaders().mAtmosphericQuality.get());
    const bool scattering = Settings::shaders().mAtmosphericScattering;
    const auto atmosphereVisibility
        = Settings::getAtmosphereControlVisibility({ scattering, clouds, Settings::shaders().mGodrays });
    mVolumetricQuality->setEnabled(atmosphereVisibility.mQuality);
    mAtmosphereAppearance->setIndexSelected(Settings::shaders().mAtmosphereAppearance.get());
    mAtmosphereAppearance->setEnabled(scattering);
    setContextVisible(*mAtmosphereAppearanceGroup, atmosphereVisibility.mAppearance);
    setContextVisible(*mAtmosphereQualityGroup, atmosphereVisibility.mQuality);

    const auto filteringQuality = Settings::openmwseShadows().mFilteringQuality.get();
    const auto filtering
        = std::find(sShadowFilteringQualities.begin(), sShadowFilteringQualities.end(), filteringQuality);
    mShadowFilteringQuality->setIndexSelected(filtering == sShadowFilteringQualities.end()
            ? 2
            : static_cast<std::size_t>(filtering - sShadowFilteringQualities.begin()));
    const auto distantQuality = Settings::openmwseShadows().mDistantShadowQuality.get();
    const auto distant = std::find(sDistantShadowQualities.begin(), sDistantShadowQualities.end(), distantQuality);
    mDistantShadowQuality->setIndexSelected(distant == sDistantShadowQualities.end()
            ? 1
            : static_cast<std::size_t>(distant - sDistantShadowQualities.begin()));
    const bool filteredShadows = filteringQuality != "legacy";
    const auto& shadows = Settings::shadows();
    const bool geometryShadows
        = shadows.mActorShadows || shadows.mPlayerShadows || shadows.mTerrainShadows || shadows.mObjectShadows;
    const auto shadowVisibility = Settings::getShadowControlVisibility(
        { geometryShadows, shadows.mCloudShadows, filteredShadows, Settings::openmwseShadows().mDistantShadows });
    setContextVisible(*mShadowFilteringGroup, shadowVisibility.mFiltering);
    setContextVisible(*mDistantShadowsGroup, shadowVisibility.mDistantShadows);
    setContextVisible(*mDistantShadowQualityGroup, shadowVisibility.mDistantQuality);
    setContextVisible(*mCloudShadowIntensityGroup, shadowVisibility.mCloudIntensity);
    setContextVisible(*mGeometryShadowControls, shadowVisibility.mGeometryControls);
}

void MWGui::GraphicsSettings::refreshWaterControls()
{
    const auto& water = Settings::openmwseWater();
    const auto appearance = std::find(sWaterAppearances.begin(), sWaterAppearances.end(), water.mAppearance.get());
    mWaterAppearance->setIndexSelected(
        appearance == sWaterAppearances.end() ? 0 : static_cast<std::size_t>(appearance - sWaterAppearances.begin()));
    const auto color = std::find(sEnhancedWaterColors.begin(), sEnhancedWaterColors.end(), water.mEnhancedColor.get());
    mEnhancedWaterColor->setIndexSelected(
        color == sEnhancedWaterColors.end() ? 0 : static_cast<std::size_t>(color - sEnhancedWaterColors.begin()));

    const bool shader = Settings::water().mShader;
    const bool enhanced = water.appearance() == Settings::WaterAppearance::Enhanced;
    const auto visibility
        = Settings::getWaterControlVisibility({ shader, water.mMeshWater, enhanced, water.mCaustics });
    setContextVisible(*mWaterShaderControls, visibility.mShaderControls);
    setContextVisible(*mMeshWaterControls, visibility.mMeshControls);
    setContextVisible(*mEnhancedWaterControls, visibility.mEnhancedControls);
    setContextVisible(*mWaterCausticsStrengthGroup, visibility.mCausticsStrength);
}
