#include "graphicssettings.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#include <MyGUI_TabControl.h>
#include <MyGUI_TabItem.h>

#include <components/l10n/manager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/shadowresolution.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "layout.hpp"

namespace
{
    constexpr std::array<std::string_view, 5> sShadowFilteringQualities{ "legacy", "low", "medium", "high", "ultra" };
    constexpr std::array<std::string_view, 4> sDistantShadowQualities{ "low", "medium", "high", "ultra" };
}

MWGui::GraphicsSettings::GraphicsSettings(Layout& layout, std::function<void()> apply)
    : mLayout(layout)
    , mApply(std::move(apply))
    , mDistanceSettings(std::make_unique<DistanceSettings>(layout, mApply))
    , mCloudShadowsButton(layout.getWidget("CloudShadowsButton")->castType<MyGUI::Button>())
    , mCloudShadowIntensity(layout.getWidget("CloudShadowIntensity")->castType<MyGUI::ScrollBar>())
    , mReflectCloudsButton(layout.getWidget("ReflectCloudsButton")->castType<MyGUI::Button>())
    , mVolumetricQuality(layout.getWidget("VolumetricQuality")->castType<MyGUI::ComboBox>())
    , mAtmosphereAppearance(layout.getWidget("AtmosphereAppearance")->castType<MyGUI::ComboBox>())
    , mShadowFilteringQuality(layout.getWidget("ShadowFilteringQuality")->castType<MyGUI::ComboBox>())
    , mDistantShadowsButton(layout.getWidget("DistantShadowsButton")->castType<MyGUI::Button>())
    , mDistantShadowQuality(layout.getWidget("DistantShadowQuality")->castType<MyGUI::ComboBox>())
    , mShadowMapResolution(layout.getWidget("ShadowMapResolution")->castType<MyGUI::EditBox>())
    , mShadowResolutionDecrease(layout.getWidget("ShadowResolutionDecrease")->castType<MyGUI::Button>())
    , mShadowResolutionIncrease(layout.getWidget("ShadowResolutionIncrease")->castType<MyGUI::Button>())
{
    mVolumetricQuality->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onVolumetricQualityChanged);
    mShadowFilteringQuality->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onShadowFilteringQualityChanged);
    mDistantShadowQuality->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onDistantShadowQualityChanged);
    mAtmosphereAppearance->eventComboChangePosition
        += MyGUI::newDelegate(this, &GraphicsSettings::onAtmosphereAppearanceChanged);

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

    refresh();
}

void MWGui::GraphicsSettings::refresh()
{
    mDistanceSettings->refresh();
    refreshShadowResolution();
    refreshAtmosphereControls();
}

void MWGui::GraphicsSettings::updateLayout()
{
    mDistanceSettings->updateLayout();
}

void MWGui::GraphicsSettings::showPage(std::string_view page)
{
    const std::string_view pageWidget = page == "atmosphere" ? "AtmospherePage"
        : page == "detail"                                   ? "DetailPage"
        : page == "water"                                    ? "WaterPage"
        : page == "shadows"                                  ? "ShadowsPage"
                                                             : "";
    if (pageWidget.empty())
        throw std::invalid_argument("unknown video settings page");
    auto* settingsTabs = mLayout.getWidget("SettingsTab")->castType<MyGUI::TabControl>();
    auto* videoPage = mLayout.getWidget("VideoPage")->castType<MyGUI::TabItem>();
    auto* videoTabs = mLayout.getWidget("VideoTabs")->castType<MyGUI::TabControl>();
    auto* selectedPage = mLayout.getWidget(pageWidget)->castType<MyGUI::TabItem>();
    settingsTabs->setItemSelected(videoPage);
    videoTabs->setItemSelected(selectedPage);
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
    mVolumetricQuality->setEnabled(
        clouds || Settings::shaders().mAtmosphericScattering || Settings::shaders().mGodrays);
    mAtmosphereAppearance->setIndexSelected(Settings::shaders().mAtmosphereAppearance.get());
    mAtmosphereAppearance->setEnabled(Settings::shaders().mAtmosphericScattering);

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
    mDistantShadowsButton->setEnabled(filteredShadows);
    mDistantShadowQuality->setEnabled(filteredShadows && Settings::openmwseShadows().mDistantShadows);
    mCloudShadowIntensity->setEnabled(Settings::shadows().mCloudShadows && filteredShadows);
}
