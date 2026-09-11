#include "distancesettings.hpp"
#include "layout.hpp"

#include <algorithm>
#include <cmath>

#include <components/l10n/manager.hpp>
#include <components/settings/values.hpp>
#include <components/widgets/box.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    DistanceSettings::DistanceSettings(Layout& layout, std::function<void()> apply)
        : mScroll(layout.getWidget("DistanceScrollView")->castType<MyGUI::ScrollView>())
        , mApply(std::move(apply))
    {
        bind(layout, *layout.mMainWidget);
        refresh();
    }

    void DistanceSettings::bind(Layout& layout, MyGUI::Widget& widget)
    {
        if (!widget.getUserString("DistanceEditor").empty())
        {
            std::array<MyGUI::Button*, 4> arrows;
            const std::array<std::string_view, 4> arrowNames{ "DistanceWorldDecrease", "DistanceWorldIncrease",
                "DistanceCellDecrease", "DistanceCellIncrease" };
            for (std::size_t i = 0; i < arrows.size(); ++i)
                arrows[i] = layout.getWidget(widget.getUserString(arrowNames[i]))->castType<MyGUI::Button>();
            auto* note = layout.getWidget(widget.getUserString("DistancePerformanceNote"))->castType<MyGUI::TextBox>();
            auto* editor = layout.getWidget(widget.getUserString("DistanceEditor"))->castType<MyGUI::EditBox>();
            auto* worldEditor
                = layout.getWidget(widget.getUserString("DistanceWorldEditor"))->castType<MyGUI::EditBox>();
            auto* cellLabel = layout.getWidget(widget.getUserString("DistanceCellLabel"))->castType<MyGUI::TextBox>();
            auto* worldLabel = layout.getWidget(widget.getUserString("DistanceWorldLabel"))->castType<MyGUI::TextBox>();
            const std::string_view category = widget.getUserString("SettingCategory");
            const std::string_view name = widget.getUserString("SettingName");
            const auto& reference = widget.getUserString("DistanceReferenceDefault") == "true"
                ? Settings::Manager::mDefaultSettings.at({ std::string(category), std::string(name) })
                : widget.getUserString(
                    !Settings::terrain().mDistantTerrain && !widget.getUserString("DistanceShortReferenceMax").empty()
                        ? "DistanceShortReferenceMax"
                        : "DistanceReferenceMax");
            auto* explanation
                = layout.getWidget(widget.getUserString("DistanceExplanation"))->castType<MyGUI::TextBox>();
            note->setFontHeight(std::max(1, explanation->getFontHeight() * 4 / 5));
            mRows.push_back(
                { &widget, explanation, widget.getTop(), &Settings::get<float>(category, name), arrows, note, editor,
                    worldEditor, cellLabel, worldLabel, widget.getUserString("DistanceLegacyUnlimited") == "true",
                    Settings::parseDistance(reference, Settings::DistanceUnits::WorldUnits).value(),
                    widget.getUserString("DistanceAllowZero") == "true", {}, {} });
            for (auto* arrow : arrows)
                arrow->eventMouseButtonClick += MyGUI::newDelegate(this, &DistanceSettings::onStep);
            for (auto* field : { editor, worldEditor })
            {
                field->eventEditSelectAccept += MyGUI::newDelegate(this, &DistanceSettings::onAccept);
                field->eventKeyLostFocus += MyGUI::newDelegate(this, &DistanceSettings::onFocusLost);
            }
        }
        auto children = widget.getEnumerator();
        while (children.next())
            bind(layout, *children.current());
    }

    void DistanceSettings::refresh()
    {
        mRefreshing = true;
        for (Row& row : mRows)
        {
            const float value = row.mSetting->get();
            const bool unlimited = row.mLegacyUnlimited && value <= 0;
            for (auto* arrow : row.mArrows)
                arrow->setEnabled(!unlimited);
            row.mPerformanceNote->setCaptionWithReplacing(unlimited ? "#{OMWEngine:DistanceLegacyUnlimitedNote}"
                    : row.mSetting == &Settings::groundcover().mRenderingDistance
                    ? "#{OMWEngine:GrassDistancePerformanceNote}"
                    : "#{OMWEngine:DistancePerformanceNote}");
            row.mPerformanceNote->setVisible(unlimited || value > row.mReferenceDistance);
            row.mCellLabel->setVisible(!unlimited);
            row.mWorldLabel->setVisible(!unlimited);
            row.mCellLabel->setCaptionWithReplacing(value == Constants::CellSizeInUnits
                    ? "#{OMWEngine:DistanceCellSuffix}"
                    : "#{OMWEngine:DistanceCellsSuffix}");
            row.mWorldLabel->setCaptionWithReplacing(
                value == 1.f ? "#{OMWEngine:DistanceWorldUnitSuffix}" : "#{OMWEngine:DistanceWorldUnitsSuffix}");
            row.mDisplayedCells = Settings::formatDistance(value, Settings::DistanceUnits::Cells);
            row.mDisplayedWorldUnits = Settings::formatDistance(value, Settings::DistanceUnits::WorldUnits);
            if (unlimited)
            {
                row.mDisplayedCells = row.mDisplayedWorldUnits = MWBase::Environment::get()
                                                                     .getL10nManager()
                                                                     ->getContext("OMWEngine")
                                                                     ->formatMessage("DistanceLegacyUnlimited", {});
                row.mCellEditor->setCaptionWithReplacing("#{OMWEngine:DistanceLegacyUnlimited}");
                row.mWorldEditor->setCaptionWithReplacing("#{OMWEngine:DistanceLegacyUnlimited}");
            }
            else
            {
                row.mCellEditor->setCaption(row.mDisplayedCells);
                row.mWorldEditor->setCaption(row.mDisplayedWorldUnits);
            }
            row.mCellEditor->setTextCursor(0);
            row.mWorldEditor->setTextCursor(0);
        }
        updateLayout();
        mRefreshing = false;
    }

    int DistanceSettings::arrangeRows()
    {
        MyGUI::Widget* previousParent = nullptr;
        int previousBottom = 0;
        int detailHeight = 0;
        for (Row& row : mRows)
        {
            auto* parent = row.mWidget->getParent();
            const int top = parent == previousParent ? previousBottom + 12 : row.mInitialTop;
            row.mWidget->setPosition(row.mWidget->getLeft(), top);
            auto fitText = [&](MyGUI::TextBox* text, int y) {
                text->setCoord(0, y, row.mWidget->getWidth(), text->getHeight());
                const int height = text->getVisible() ? text->getTextSize().height : 0;
                text->setSize(text->getWidth(), height);
                return y + height;
            };
            int bottom = fitText(row.mExplanation, row.mWidget->getBottom() + 2);
            if (row.mPerformanceNote->getVisible())
                bottom = fitText(row.mPerformanceNote, bottom + 2);
            else
                fitText(row.mPerformanceNote, bottom);
            if (row.mLegacyUnlimited)
            {
                parent->setSize(parent->getWidth(), bottom + 4);
                if (auto* box = dynamic_cast<Gui::Box*>(parent->getParent()))
                    box->notifyChildrenSizeChanged();
            }
            else
                detailHeight = bottom + 4;
            previousParent = parent;
            previousBottom = bottom;
        }
        return detailHeight;
    }

    void DistanceSettings::updateLayout()
    {
        // Reflow again after scrollbar visibility changes the width available for word wrapping.
        for (int pass = 0; pass < 2; ++pass)
        {
            mScroll->setCanvasSize(mScroll->getViewCoord().width, mScroll->getCanvasSize().height);
            mScroll->setCanvasSize(mScroll->getViewCoord().width, arrangeRows());
        }
    }

    void DistanceSettings::onAccept(MyGUI::EditBox* editor)
    {
        if (mRefreshing)
            return;
        for (Row& row : mRows)
        {
            if ((row.mCellEditor != editor && row.mWorldEditor != editor) || !editor->getEnabled())
                continue;
            const std::string text = editor->getOnlyText();
            const bool cells = editor == row.mCellEditor;
            if (text == (cells ? row.mDisplayedCells : row.mDisplayedWorldUnits))
                return;
            const auto value = Settings::parseDistance(
                text, cells ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits);
            if (value && (*value > 0 || row.mAllowZero))
            {
                row.mSetting->set(*value);
                mApply();
            }
            else
            {
                // Restore the field before a modal message can dispatch another focus-loss event.
                refresh();
                MWBase::Environment::get().getWindowManager()->messageBox("#{OMWEngine:DistanceInvalidValue}");
                return;
            }
            refresh();
            return;
        }
    }

    void DistanceSettings::onFocusLost(MyGUI::Widget* editor, MyGUI::Widget*)
    {
        onAccept(editor->castType<MyGUI::EditBox>());
    }

    void DistanceSettings::onStep(MyGUI::Widget* button)
    {
        if (mRefreshing)
            return;
        for (Row& row : mRows)
            for (std::size_t i = 0; i < row.mArrows.size(); ++i)
                if (row.mArrows[i] == button && button->getEnabled())
                {
                    const auto units = i < 2 ? Settings::DistanceUnits::WorldUnits : Settings::DistanceUnits::Cells;
                    const float value = Settings::stepDistance(row.mSetting->get(), units, i % 2 != 0, row.mAllowZero);
                    if (value != row.mSetting->get())
                    {
                        row.mSetting->set(value);
                        mApply();
                    }
                    refresh();
                    return;
                }
    }

    void DistanceSettings::setValues(bool useCellEditor, float view, float grass, float shadow)
    {
        refresh();
        for (Row& row : mRows)
        {
            const float value = row.mSetting == &Settings::camera().mViewingDistance ? view
                : row.mSetting == &Settings::groundcover().mRenderingDistance        ? grass
                                                                                     : shadow;
            if (row.mLegacyUnlimited && value <= 0)
            {
                // Model an imported OpenMW configuration, which the menu must preserve until explicitly edited.
                row.mSetting->set(value);
                mApply();
                refresh();
                onAccept(row.mCellEditor);
                onAccept(row.mWorldEditor);
                if (row.mSetting->get() != value || !row.mPerformanceNote->getVisible())
                    throw std::runtime_error("legacy shadow distance was not preserved or explained");
            }
            else
            {
                auto* editor = useCellEditor ? row.mCellEditor : row.mWorldEditor;
                editor->setCaption(Settings::formatDistance(
                    value, useCellEditor ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits));
                editor->eventEditSelectAccept(editor);
                for (std::size_t direction = 0; direction < 2; ++direction)
                {
                    const auto units
                        = useCellEditor ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits;
                    const float expected = Settings::stepDistance(value, units, direction != 0, row.mAllowZero);
                    auto* arrow = row.mArrows[(useCellEditor ? 2 : 0) + direction];
                    arrow->eventMouseButtonClick(arrow);
                    if (row.mSetting->get() != expected)
                        throw std::runtime_error("distance arrow did not apply the expected increment");
                    editor->setCaption(Settings::formatDistance(value, units));
                    editor->eventEditSelectAccept(editor);
                }
                if (row.mExplanation->getTop() != row.mWidget->getBottom() + 2
                    || row.mPerformanceNote->getHeight()
                        != (row.mPerformanceNote->getVisible() ? row.mPerformanceNote->getTextSize().height : 0)
                    || row.mPerformanceNote->getFontHeight() >= row.mExplanation->getFontHeight())
                    throw std::runtime_error("distance help layout did not fit its text or use a smaller warning font");
                if (row.mPerformanceNote->getVisible() != (value > row.mReferenceDistance))
                    throw std::runtime_error("distance performance note does not match the current value");
                if (row.mCellEditor->getOnlyText()
                        != Settings::formatDistance(row.mSetting->get(), Settings::DistanceUnits::Cells)
                    || row.mWorldEditor->getOnlyText()
                        != Settings::formatDistance(row.mSetting->get(), Settings::DistanceUnits::WorldUnits))
                    throw std::runtime_error("distance controls did not refresh both numeric fields");
            }
        }
    }
}
