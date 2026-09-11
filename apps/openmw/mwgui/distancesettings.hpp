#ifndef OPENMW_MWGUI_DISTANCESETTINGS_H
#define OPENMW_MWGUI_DISTANCESETTINGS_H

#include <components/settings/distance.hpp>
#include <components/settings/settingvalue.hpp>

#include <array>
#include <functional>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ScrollView.h>

namespace MWGui
{
    class Layout;
    /// Bind video distance editors to canonical settings with simultaneous cell and world-unit entry.
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

    private:
        /// Retain one setting's paired numeric entries, increment buttons, and performance note.
        struct Row
        {
            MyGUI::Widget* mWidget;
            MyGUI::TextBox* mExplanation;
            int mInitialTop;
            Settings::SettingValue<float>* mSetting;
            std::array<MyGUI::Button*, 4> mArrows;
            MyGUI::TextBox* mPerformanceNote;
            MyGUI::EditBox* mCellEditor;
            MyGUI::EditBox* mWorldEditor;
            MyGUI::TextBox* mCellLabel;
            MyGUI::TextBox* mWorldLabel;
            bool mLegacyUnlimited;
            float mReferenceDistance;
            bool mAllowZero;
            std::string mDisplayedCells;
            std::string mDisplayedWorldUnits;
        };

        /// Discover layout-owned bindings without duplicating their configuration keys.
        void bind(Layout& layout, MyGUI::Widget& widget);
        /// Apply numeric entry on Enter and restore the canonical presentation on invalid input.
        void onAccept(MyGUI::EditBox* editor);
        /// Commit an edited field before keyboard focus moves to another control.
        void onFocusLost(MyGUI::Widget* editor, MyGUI::Widget* next);
        /// Adjust either numeric field without imposing a presentation maximum.
        void onStep(MyGUI::Widget* button);

        /// Stack distance sections within their owning containers and return the detail canvas height.
        int arrangeRows();

        MyGUI::ScrollView* mScroll;
        std::function<void()> mApply;
        std::vector<Row> mRows;
        bool mRefreshing = false;
    };
}

#endif
