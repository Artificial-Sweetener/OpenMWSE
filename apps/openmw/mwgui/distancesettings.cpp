#include "distancesettings.hpp"
#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include <components/l10n/manager.hpp>
#include <components/settings/values.hpp>
#include <components/widgets/box.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    DistanceSettings::DistanceSettings(Layout& layout, std::function<void()> apply)
        : mScroll(layout.getWidget("VisibilityScrollView")->castType<MyGUI::ScrollView>())
        , mContent(layout.getWidget("VisibilityContent"))
        , mFogControls(layout.getWidget("FogDistanceControls"))
        , mUseDistantFog(layout.getWidget("UseDistantFogButton")->castType<MyGUI::Button>())
        , mApply(std::move(apply))
    {
        bind(layout, *layout.mMainWidget);
        mUseDistantFog->eventMouseButtonClick += MyGUI::newDelegate(this, &DistanceSettings::onFogEnabledChanged);
        refresh();
    }

    void DistanceSettings::bind(Layout& layout, MyGUI::Widget& widget)
    {
        if (!widget.getUserString("DistanceEditor").empty())
        {
            std::array<MyGUI::Button*, 2> arrows;
            const std::array<std::string_view, 2> arrowNames{ "DistanceDecrease", "DistanceIncrease" };
            for (std::size_t i = 0; i < arrows.size(); ++i)
                arrows[i] = layout.getWidget(widget.getUserString(arrowNames[i]))->castType<MyGUI::Button>();
            auto* note = layout.getWidget(widget.getUserString("DistancePerformanceNote"))->castType<MyGUI::TextBox>();
            auto* editor = layout.getWidget(widget.getUserString("DistanceEditor"))->castType<MyGUI::EditBox>();
            auto* units = layout.getWidget(widget.getUserString("DistanceUnitSelector"))->castType<MyGUI::ComboBox>();
            auto* useCells = &Settings::get<bool>("GUI", widget.getUserString("DistanceUnitSetting"));
            MyGUI::ComboBox* behavior = nullptr;
            Settings::SettingValue<bool>* locked = nullptr;
            if (!widget.getUserString("DistanceBehaviorSelector").empty())
            {
                behavior
                    = layout.getWidget(widget.getUserString("DistanceBehaviorSelector"))->castType<MyGUI::ComboBox>();
                locked = &Settings::get<bool>("Fog", widget.getUserString("DistanceLockedSetting"));
            }
            const std::string_view category = widget.getUserString("SettingCategory");
            const std::string_view name = widget.getUserString("SettingName");
            const auto& reference = widget.getUserString("DistanceReferenceDefault") == "true"
                ? Settings::Manager::mDefaultSettings.at({ std::string(category), std::string(name) })
                : widget.getUserString(
                    !Settings::terrain().mDistantTerrain && !widget.getUserString("DistanceShortReferenceMax").empty()
                        ? "DistanceShortReferenceMax"
                        : "DistanceReferenceMax");
            MyGUI::TextBox* explanation = nullptr;
            if (!widget.getUserString("DistanceExplanation").empty())
                explanation = layout.getWidget(widget.getUserString("DistanceExplanation"))->castType<MyGUI::TextBox>();
            if (explanation != nullptr)
                note->setFontHeight(std::max(1, explanation->getFontHeight() * 4 / 5));
            mRows.push_back({ &widget, explanation, &Settings::get<float>(category, name), arrows, note, editor, units,
                useCells, behavior, locked, widget.getUserString("DistanceLegacyUnlimited") == "true",
                Settings::parseDistance(reference, Settings::DistanceUnits::WorldUnits).value(),
                widget.getUserString("DistanceAllowZero") == "true", {} });
            for (auto* arrow : arrows)
                arrow->eventMouseButtonClick += MyGUI::newDelegate(this, &DistanceSettings::onStep);
            editor->eventEditSelectAccept += MyGUI::newDelegate(this, &DistanceSettings::onAccept);
            editor->eventKeyLostFocus += MyGUI::newDelegate(this, &DistanceSettings::onFocusLost);
            units->eventComboChangePosition += MyGUI::newDelegate(this, &DistanceSettings::onUnitsChanged);
            if (behavior != nullptr)
                behavior->eventComboChangePosition += MyGUI::newDelegate(this, &DistanceSettings::onBehaviorChanged);
        }
        auto children = widget.getEnumerator();
        while (children.next())
            bind(layout, *children.current());
    }

    void DistanceSettings::refresh()
    {
        mRefreshing = true;
        const auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
        for (Row& row : mRows)
        {
            const float value = row.mSetting->get();
            const bool unlimited = row.mLegacyUnlimited && value <= 0;
            for (auto* arrow : row.mArrows)
                arrow->setEnabled(!unlimited);
            row.mEditor->setEnabled(!unlimited);
            row.mUnitSelector->setEnabled(!unlimited);
            row.mUnitSelector->setIndexSelected(*row.mUseCells ? 0 : 1);
            if (row.mBehaviorSelector != nullptr)
                row.mBehaviorSelector->setIndexSelected(*row.mLocked ? 1 : 0);
            row.mPerformanceNote->setCaptionWithReplacing(unlimited ? "#{OMWEngine:DistanceLegacyUnlimitedNote}"
                    : row.mSetting == &Settings::groundcover().mRenderingDistance
                    ? "#{OMWEngine:GrassDistancePerformanceNote}"
                    : "#{OMWEngine:DistancePerformanceNote}");
            const bool showNote = unlimited || value > row.mReferenceDistance;
            row.mPerformanceNote->setVisible(showNote);
            row.mPerformanceNote->setUserString("Hidden", showNote ? "false" : "true");
            const auto units = *row.mUseCells ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits;
            row.mDisplayedValue = Settings::formatDistance(value, units);
            if (unlimited)
            {
                row.mDisplayedValue = l10n->formatMessage("DistanceLegacyUnlimited", {});
                row.mEditor->setCaptionWithReplacing("#{OMWEngine:DistanceLegacyUnlimited}");
            }
            else
                row.mEditor->setCaption(row.mDisplayedValue);
            row.mEditor->setTextCursor(0);
            if (row.mExplanation != nullptr && row.mLocked != nullptr)
            {
                if (*row.mLocked)
                    row.mExplanation->setCaption(l10n->formatMessage("FogDistanceFixedExplanation", {}));
                else
                {
                    std::ostringstream percentage;
                    const float viewingDistance = Settings::camera().mViewingDistance;
                    percentage << std::fixed << std::setprecision(1)
                               << (viewingDistance > 0.f ? 100.0 * value / viewingDistance : 0.0);
                    row.mExplanation->setCaption(l10n->formatMessage(
                        "FogDistanceScalesExplanation", { "percent" }, { L10n::toUnicode(percentage.str()) }));
                }
                row.mExplanation->setSize(row.mExplanation->getWidth(), row.mExplanation->getTextSize().height);
            }
            updateGroupLayout(*row.mWidget);
        }
        const bool showFog = Settings::fog().mUseDistantFog;
        mFogControls->setVisible(showFog);
        mFogControls->setUserString("Hidden", showFog ? "false" : "true");
        updateGroupLayout(*mFogControls);
        updateLayout();
        mRefreshing = false;
    }

    DistanceSettings::Row* DistanceSettings::findRow(Settings::SettingValue<float>& setting)
    {
        const auto found
            = std::find_if(mRows.begin(), mRows.end(), [&](const Row& row) { return row.mSetting == &setting; });
        return found == mRows.end() ? nullptr : &*found;
    }

    DistanceSettings::Row* DistanceSettings::findRow(MyGUI::ComboBox* selector)
    {
        const auto found = std::find_if(mRows.begin(), mRows.end(),
            [&](const Row& row) { return row.mUnitSelector == selector || row.mBehaviorSelector == selector; });
        return found == mRows.end() ? nullptr : &*found;
    }

    bool DistanceSettings::isValidFogValue(const Row& row, float value) const
    {
        if (row.mSetting == &Settings::fog().mDistantLandFogStart)
            return value < Settings::fog().mDistantLandFogEnd;
        if (row.mSetting == &Settings::fog().mDistantLandFogEnd)
            return value > Settings::fog().mDistantLandFogStart;
        return true;
    }

    void DistanceSettings::applyValue(Row& row, float value)
    {
        if (row.mSetting == &Settings::camera().mViewingDistance)
        {
            const auto fog = Settings::scaleFogDistanceRange(
                { Settings::fog().mDistantLandFogStart, Settings::fog().mDistantLandFogEnd }, row.mSetting->get(),
                value, Settings::fog().mDistantLandFogStartLocked, Settings::fog().mDistantLandFogEndLocked);
            Settings::fog().mDistantLandFogStart.set(fog.mStart);
            Settings::fog().mDistantLandFogEnd.set(fog.mEnd);
        }
        row.mSetting->set(value);
        mApply();
    }

    void DistanceSettings::onAccept(MyGUI::EditBox* editor)
    {
        if (mRefreshing)
            return;
        for (Row& row : mRows)
        {
            if (row.mEditor != editor || !editor->getEnabled())
                continue;
            const std::string text = editor->getOnlyText();
            if (text == row.mDisplayedValue)
                return;
            const auto units = *row.mUseCells ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits;
            const auto value = Settings::parseDistance(text, units);
            if (value && (*value > 0 || row.mAllowZero) && isValidFogValue(row, *value))
                applyValue(row, *value);
            else
            {
                // Restore the field before a modal message can dispatch another focus-loss event.
                refresh();
                const bool fogDistance = row.mSetting == &Settings::fog().mDistantLandFogStart
                    || row.mSetting == &Settings::fog().mDistantLandFogEnd;
                MWBase::Environment::get().getWindowManager()->messageBox(
                    fogDistance ? "#{OMWEngine:FogDistanceInvalidValue}" : "#{OMWEngine:DistanceInvalidValue}");
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
                    const auto units
                        = *row.mUseCells ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits;
                    float value = Settings::stepDistance(row.mSetting->get(), units, i % 2 != 0, row.mAllowZero);
                    if (row.mSetting == &Settings::fog().mDistantLandFogStart
                        && value >= Settings::fog().mDistantLandFogEnd)
                        value = std::nextafter(Settings::fog().mDistantLandFogEnd.get(), 0.f);
                    else if (row.mSetting == &Settings::fog().mDistantLandFogEnd
                        && value <= Settings::fog().mDistantLandFogStart)
                        value = std::nextafter(
                            Settings::fog().mDistantLandFogStart.get(), std::numeric_limits<float>::infinity());
                    if (value != row.mSetting->get())
                        applyValue(row, value);
                    refresh();
                    return;
                }
    }

    void DistanceSettings::onUnitsChanged(MyGUI::ComboBox* sender, std::size_t position)
    {
        if (mRefreshing || position > 1)
            return;
        Row* row = findRow(sender);
        if (row == nullptr)
            return;
        row->mUseCells->set(position == 0);
        mApply();
        refresh();
    }

    void DistanceSettings::onBehaviorChanged(MyGUI::ComboBox* sender, std::size_t position)
    {
        if (mRefreshing || position > 1)
            return;
        Row* row = findRow(sender);
        if (row == nullptr || row->mLocked == nullptr)
            return;
        row->mLocked->set(position == 1);
        mApply();
        refresh();
    }

    void DistanceSettings::onFogEnabledChanged(MyGUI::Widget*)
    {
        refresh();
    }

    void DistanceSettings::setValues(bool useCellEditor, float view, float grass, float shadow)
    {
        refresh();
        for (Row& row : mRows)
        {
            const float value = row.mSetting == &Settings::camera().mViewingDistance ? view
                : row.mSetting == &Settings::groundcover().mRenderingDistance        ? grass
                : row.mSetting == &Settings::shadows().mMaximumShadowMapDistance     ? shadow
                                                                                     : row.mSetting->get();
            if (row.mSetting == &Settings::fog().mDistantLandFogStart
                || row.mSetting == &Settings::fog().mDistantLandFogEnd)
                continue;
            if (row.mLegacyUnlimited && value <= 0)
            {
                // Model an imported OpenMW configuration, which the menu must preserve until explicitly edited.
                row.mSetting->set(value);
                mApply();
                refresh();
                onAccept(row.mEditor);
                if (row.mSetting->get() != value || !row.mPerformanceNote->getVisible())
                    throw std::runtime_error("legacy shadow distance was not preserved or explained");
            }
            else
            {
                const std::size_t unitsPosition = useCellEditor ? 0 : 1;
                row.mUnitSelector->setIndexSelected(unitsPosition);
                row.mUnitSelector->eventComboChangePosition(row.mUnitSelector, unitsPosition);
                const auto units = useCellEditor ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits;
                row.mEditor->setCaption(Settings::formatDistance(value, units));
                row.mEditor->eventEditSelectAccept(row.mEditor);
                for (std::size_t direction = 0; direction < 2; ++direction)
                {
                    const float expected = Settings::stepDistance(value, units, direction != 0, row.mAllowZero);
                    auto* arrow = row.mArrows[direction];
                    arrow->eventMouseButtonClick(arrow);
                    if (row.mSetting->get() != expected)
                        throw std::runtime_error("distance arrow did not apply the expected increment");
                    row.mEditor->setCaption(Settings::formatDistance(value, units));
                    row.mEditor->eventEditSelectAccept(row.mEditor);
                }
                if (row.mPerformanceNote->getVisible() != (value > row.mReferenceDistance))
                    throw std::runtime_error("distance performance note does not match the current value");
                if (row.mEditor->getOnlyText() != Settings::formatDistance(row.mSetting->get(), units)
                    || row.mUnitSelector->getIndexSelected() != unitsPosition)
                    throw std::runtime_error("distance control did not retain its value and presentation units");
            }
        }
    }

    void DistanceSettings::setFogValues(
        bool useCellEditor, bool enabled, float start, float end, bool startLocked, bool endLocked)
    {
        if (!std::isfinite(start) || !std::isfinite(end) || start < 0.f || !(end > start))
            throw std::invalid_argument("diagnostic fog range must be finite, non-negative, and strictly ordered");
        const auto toggle = [](MyGUI::Button* button, bool current, bool requested) {
            if (current != requested)
                button->eventMouseButtonClick(button);
        };
        if (!Settings::fog().mUseDistantFog)
            mUseDistantFog->eventMouseButtonClick(mUseDistantFog);

        Row* startRow = findRow(Settings::fog().mDistantLandFogStart);
        Row* endRow = findRow(Settings::fog().mDistantLandFogEnd);
        if (startRow == nullptr || endRow == nullptr)
            throw std::runtime_error("fog distance controls are not bound");
        const auto selectBehavior = [](Row& row, bool locked) {
            const std::size_t position = locked ? 1 : 0;
            row.mBehaviorSelector->setIndexSelected(position);
            row.mBehaviorSelector->eventComboChangePosition(row.mBehaviorSelector, position);
        };
        selectBehavior(*startRow, startLocked);
        selectBehavior(*endRow, endLocked);
        const auto enter = [useCellEditor](Row& row, float value) {
            const std::size_t position = useCellEditor ? 0 : 1;
            row.mUnitSelector->setIndexSelected(position);
            row.mUnitSelector->eventComboChangePosition(row.mUnitSelector, position);
            const auto units = useCellEditor ? Settings::DistanceUnits::Cells : Settings::DistanceUnits::WorldUnits;
            row.mEditor->setCaption(Settings::formatDistance(value, units));
            row.mEditor->eventEditSelectAccept(row.mEditor);
        };
        if (start >= Settings::fog().mDistantLandFogEnd)
            enter(*endRow, end);
        enter(*startRow, start);
        enter(*endRow, end);
        toggle(mUseDistantFog, Settings::fog().mUseDistantFog, enabled);
        if (Settings::fog().mDistantLandFogStart != start || Settings::fog().mDistantLandFogEnd != end
            || Settings::fog().mDistantLandFogStartLocked != startLocked
            || Settings::fog().mDistantLandFogEndLocked != endLocked || Settings::fog().mUseDistantFog != enabled)
            throw std::runtime_error("fog distance controls did not retain the requested range and behavior modes");
    }
}
