#ifndef OPENMW_MWGUI_DISTANCESETTINGS_H
#define OPENMW_MWGUI_DISTANCESETTINGS_H

#include <components/settings/distance.hpp>
#include <components/settings/settingvalue.hpp>

#include <array>
#include <functional>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_ComboBox.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ScrollView.h>

namespace MWGui
{
    class Layout;
    /// Bind each video distance to one canonical editor with an independently remembered presentation unit.
    class DistanceSettings
    {
    public:
        /// Discover distance controls and dispatch edits through the settings window's live apply path.
        DistanceSettings(Layout& layout, std::function<void()> apply);

        /// Refresh presentation without writing settings, without imposing slider limits.
        void refresh();

        /// Fit explanations and visible warnings to their wrapped text and collapse unused vertical space.
        void updateLayout();

        /// Verify editor entry, arrow steps, and performance notes, then retain the requested isolated diagnostic
        /// values.
        void setValues(bool useCellEditor, float view, float grass, float shadow);

        /// Exercise the live exterior fog range and independent lock controls with isolated diagnostic values.
        void setFogValues(bool useCellEditor, bool enabled, float start, float end, bool startLocked, bool endLocked);

    private:
        /// Retain one setting's canonical editor, presentation choice, behavior choice, and performance note.
        struct Row
        {
            MyGUI::Widget* mWidget;
            MyGUI::TextBox* mExplanation;
            Settings::SettingValue<float>* mSetting;
            std::array<MyGUI::Button*, 2> mArrows;
            MyGUI::TextBox* mPerformanceNote;
            MyGUI::EditBox* mEditor;
            MyGUI::ComboBox* mUnitSelector;
            Settings::SettingValue<bool>* mUseCells;
            MyGUI::ComboBox* mBehaviorSelector;
            Settings::SettingValue<bool>* mLocked;
            bool mLegacyUnlimited;
            float mReferenceDistance;
            bool mAllowZero;
            std::string mDisplayedValue;
        };

        /// Discover layout-owned bindings without duplicating their configuration keys.
        void bind(Layout& layout, MyGUI::Widget& widget);
        /// Apply numeric entry on Enter and restore the canonical presentation on invalid input.
        void onAccept(MyGUI::EditBox* editor);
        /// Commit an edited field before keyboard focus moves to another control.
        void onFocusLost(MyGUI::Widget* editor, MyGUI::Widget* next);
        /// Adjust either numeric field without imposing a presentation maximum.
        void onStep(MyGUI::Widget* button);
        /// Change only one row's presentation units while preserving its canonical distance.
        void onUnitsChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Select proportional or fixed fog behavior without changing the current endpoint.
        void onBehaviorChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Refresh fog editor availability after its live prerequisite changes.
        void onFogEnabledChanged(MyGUI::Widget* button);

        /// Commit a validated distance and apply relational fog scaling when viewing distance changes.
        void applyValue(Row& row, float value);
        /// Return whether a proposed exterior fog endpoint keeps the base range strictly ordered.
        bool isValidFogValue(const Row& row, float value) const;
        /// Return the row bound to a canonical setting, or null when that setting is not displayed.
        Row* findRow(Settings::SettingValue<float>& setting);

        /// Return the row bound to a presentation selector, or null when it is not a distance control.
        Row* findRow(MyGUI::ComboBox* selector);
        /// Reflow a layout-owned group after changing a contextual child.
        static void updateGroupLayout(MyGUI::Widget& widget);

        MyGUI::ScrollView* mScroll;
        MyGUI::Widget* mContent;
        MyGUI::Widget* mFogControls;
        MyGUI::Button* mUseDistantFog;
        std::function<void()> mApply;
        std::vector<Row> mRows;
        bool mRefreshing = false;
    };
}

#endif
