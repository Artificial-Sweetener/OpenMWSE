#ifndef MWGUI_SETTINGS_H
#define MWGUI_SETTINGS_H

#include <optional>

#include <components/files/configurationmanager.hpp>
#include <components/lua_ui/adapter.hpp>

#include "windowbase.hpp"

namespace MWGui
{
    class GraphicsSettings;

    class SettingsWindow : public WindowBase
    {
    public:
        /// Bind video and gameplay controls to their persisted settings.
        SettingsWindow(Files::ConfigurationManager& cfgMgr);

        /// Destroy graphics ownership where its complete type is available.
        ~SettingsWindow() override;

        /// Refresh values, dependencies and the space required by localized tabs.
        void onOpen() override;

        /// Open a named video page using stable layout identities.
        void showVideoPage(std::string_view page);

        /// Apply isolated diagnostic input through the ordinary distance and shadow-resolution controls.
        void setDistanceValues(
            bool useCellEditor, float view, float grass, float shadow, std::optional<int> resolution);

        /// Apply isolated diagnostic input through the ordinary geometric-wave slider.
        void setGeometricWaveHeight(float height);

        /// Exercise live water displacement and caustic controls for isolated diagnostics.
        void setWaterValues(float height, bool caustics, bool reflectedCaustics, float causticsStrength);

        void onClose() override;

        void onFrame(float duration) override;

        void updateControlsBox();

        void updateLightSettings();
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
        MyGUI::Widget* mMinimumBrightness;
        MyGUI::Widget* mMinimumLightRadius;

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
        void onSliderChangePosition(MyGUI::ScrollBar* scroller, size_t pos);
        void onButtonToggled(MyGUI::Widget* sender);
        void onResolutionSelected(MyGUI::ListBox* sender, size_t index);
        void onResolutionAccept();
        void onResolutionCancel();
        void highlightCurrentResolution();

        void onWaterTextureSizeChanged(MyGUI::ComboBox* sender, size_t pos);
        void onWaterReflectionDetailChanged(MyGUI::ComboBox* sender, size_t pos);
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
        std::unique_ptr<GraphicsSettings> mGraphicsSettings;
        void resetScrollbars();
        Files::ConfigurationManager& mCfgMgr;
    };
}

#endif
