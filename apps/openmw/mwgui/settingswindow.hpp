#ifndef MWGUI_SETTINGS_H
#define MWGUI_SETTINGS_H

#include <optional>

#include <components/files/configurationmanager.hpp>
#include <components/lua_ui/adapter.hpp>

#include "distancesettings.hpp"
#include "windowbase.hpp"

namespace MWGui
{
    class SettingsWindow : public WindowBase
    {
    public:
        /// Bind video and gameplay controls to their persisted settings.
        SettingsWindow(Files::ConfigurationManager& cfgMgr);

        /// Refresh values, dependencies and the space required by localized tabs.
        void onOpen() override;

        /// Open a named video page using stable layout identities.
        void showVideoPage(std::string_view page);

        /// Apply isolated diagnostic input through the ordinary distance and shadow-resolution controls.
        void setDistanceValues(
            bool useCellEditor, float view, float grass, float shadow, std::optional<int> resolution);

        void onClose() override;

        void onFrame(float duration) override;

        void updateControlsBox();

        void updateLightSettings();
        /// Show effective atmosphere control states while retaining preferences for temporarily disabled features.
        void updateAtmosphereSettings();

        void updateVSyncModeSettings();

        void updateWindowModeSettings();

        void onResChange(int, int) override;

        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    protected:
        MyGUI::TabControl* mSettingsTab;
        MyGUI::Button* mOkButton;

        // graphics
        MyGUI::ListBox* mResolutionList;
        MyGUI::ComboBox* mWindowModeList;
        MyGUI::ComboBox* mVSyncModeList;
        MyGUI::Button* mWindowBorderButton;
        MyGUI::ComboBox* mTextureFilteringButton;
        MyGUI::ComboBox* mAnisotropy;

        MyGUI::Button* mWaterRefractionButton;
        MyGUI::ComboBox* mWaterTextureSize;
        MyGUI::ComboBox* mWaterReflectionDetail;
        MyGUI::ComboBox* mWaterRainRippleDetail;

        MyGUI::ComboBox* mMaxLights;
        MyGUI::Button* mClusteredLightingButton;
        MyGUI::Widget* mClassicFalloffWidget;
        MyGUI::Button* mLightsResetButton;
        MyGUI::Widget* mMinimumBrightnessText;
        MyGUI::Widget* mMinimumBrightnessScroll;

        MyGUI::Button* mCloudShadowsButton;
        MyGUI::ScrollBar* mCloudShadowIntensity;
        MyGUI::Button* mReflectCloudsButton;
        MyGUI::ComboBox* mVolumetricQuality;
        MyGUI::ComboBox* mAtmosphereAppearance;
        MyGUI::ComboBox* mShadowFilteringQuality;
        MyGUI::Button* mActorShadowsButton;
        MyGUI::Button* mPlayerShadowsButton;
        MyGUI::Button* mTerrainShadowsButton;
        MyGUI::Button* mObjectShadowsButton;
        MyGUI::EditBox* mShadowMapResolution;
        MyGUI::Button* mShadowResolutionDecrease;
        MyGUI::Button* mShadowResolutionIncrease;

        MyGUI::ComboBox* mPrimaryLanguage;
        MyGUI::ComboBox* mSecondaryLanguage;
        MyGUI::Button* mGmstOverridesL10n;

        MyGUI::Widget* mWindowModeHint;

        // controls
        MyGUI::ScrollView* mControlsBox;
        MyGUI::Button* mResetControlsButton;
        MyGUI::Button* mKeyboardSwitch;
        MyGUI::Button* mControllerSwitch;
        bool mKeyboardMode; // if true, setting up the keyboard. Otherwise, it's controller

        MyGUI::EditBox* mScriptFilter;
        MyGUI::ListBox* mScriptList;
        MyGUI::Widget* mScriptBox;
        MyGUI::Widget* mScriptDisabled;
        MyGUI::ScrollView* mScriptView;
        LuaUi::LuaAdapter* mScriptAdapter;
        size_t mCurrentPage;

        void onTabChanged(MyGUI::TabControl* sender, size_t index);
        void onOkButtonClicked(MyGUI::Widget* sender);
        void onTextureFilteringChanged(MyGUI::ComboBox* sender, size_t pos);
        void onAnisotropyChanged(MyGUI::ComboBox* sender, size_t pos);
        void onShadowsButtonClicked(MyGUI::Widget* sender);
        /// Apply a supported whole-pixel shadow resolution on Enter.
        void onShadowMapResolutionChanged(MyGUI::EditBox* sender);
        /// Commit resolution edits when keyboard focus leaves the field.
        void onShadowResolutionFocusLost(MyGUI::Widget* sender, MyGUI::Widget* next);
        /// Adjust both dimensions by 256 pixels within renderer and hardware limits.
        void onShadowResolutionStep(MyGUI::Widget* sender);
        /// Synchronize the pixel field and the supported increment boundaries.
        void refreshShadowResolution();
        void onSliderChangePosition(MyGUI::ScrollBar* scroller, size_t pos);
        void onButtonToggled(MyGUI::Widget* sender);
        void onResolutionSelected(MyGUI::ListBox* sender, size_t index);
        void onResolutionAccept();
        void onResolutionCancel();
        void highlightCurrentResolution();

        void onWaterTextureSizeChanged(MyGUI::ComboBox* sender, size_t pos);
        void onWaterReflectionDetailChanged(MyGUI::ComboBox* sender, size_t pos);
        /// Apply the selected quality preset immediately through the standard settings notification path.
        void onVolumetricQualityChanged(MyGUI::ComboBox* sender, size_t pos);
        /// Apply the selected geometry-shadow filtering policy immediately.
        void onShadowFilteringQualityChanged(MyGUI::ComboBox* sender, size_t pos);
        /// Apply the selected atmospheric color response immediately.
        void onAtmosphereAppearanceChanged(MyGUI::ComboBox* sender, size_t pos);
        void onWaterRainRippleDetailChanged(MyGUI::ComboBox* sender, size_t pos);

        void onLightsResetButtonClicked(MyGUI::Widget* sender);
        void onMaxLightsChanged(MyGUI::ComboBox* sender, size_t pos);

        void onPrimaryLanguageChanged(MyGUI::ComboBox* sender, size_t pos) { onLanguageChanged(0, sender, pos); }
        void onSecondaryLanguageChanged(MyGUI::ComboBox* sender, size_t pos) { onLanguageChanged(1, sender, pos); }
        void onLanguageChanged(size_t langPriority, MyGUI::ComboBox* sender, size_t pos);
        void onGmstOverridesL10nChanged(MyGUI::Widget* sender);

        void onWindowModeChanged(MyGUI::ComboBox* sender, size_t pos);
        void onVSyncModeChanged(MyGUI::ComboBox* sender, size_t pos);

        void onRebindAction(MyGUI::Widget* sender);
        void onInputTabMouseWheel(MyGUI::Widget* sender, int rel);
        void onResetDefaultBindings(MyGUI::Widget* sender);
        void onResetDefaultBindingsAccept();
        void onKeyboardSwitchClicked(MyGUI::Widget* sender);
        void onControllerSwitchClicked(MyGUI::Widget* sender);

        /// Resize scrollable video content and control lists with the settings window.
        void onWindowResize(MyGUI::Window* sender);

        void onScriptFilterChange(MyGUI::EditBox*);
        void onScriptListSelection(MyGUI::ListBox*, size_t index);

        /// Apply pending preferences live and refresh dependent controls.
        void apply();

        /// Bind or refresh settings widgets, then restore effective states for dependent atmosphere controls.
        void configureWidgets(MyGUI::Widget* widget, bool init);
        MyGUI::TextBox* getSliderLabel(MyGUI::ScrollBar* scroller) const;

        void layoutControlsBox();
        void renderScriptSettings();

        /// Fit both the main and nested video tab captions at the current localization.
        void computeMinimumWindowSize();

    private:
        std::unique_ptr<DistanceSettings> mDistanceSettings;
        void resetScrollbars();
        Files::ConfigurationManager& mCfgMgr;
    };
}

#endif
