#include "graphicssettings.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

#include <MyGUI_TabControl.h>
#include <MyGUI_TabItem.h>

#include <components/settings/graphicssettingsvisibility.hpp>
#include <components/settings/values.hpp>
#include <components/widgets/box.hpp>

#include "layout.hpp"

void MWGui::DistanceSettings::updateGroupLayout(MyGUI::Widget& widget)
{
    for (MyGUI::Widget* current = &widget; current != nullptr; current = current->getParent())
        if (auto* box = dynamic_cast<Gui::Box*>(current))
            box->notifyChildrenSizeChanged();
}

void MWGui::DistanceSettings::updateLayout()
{
    // Reflow again after scrollbar visibility changes the width available for word wrapping.
    mScroll->setVisibleVScroll(true);
    for (int pass = 0; pass < 2; ++pass)
    {
        const int width = std::max(1, mScroll->getViewCoord().width);
        mContent->setSize(width, mContent->getHeight());
        updateGroupLayout(*mContent);
        for (Row& row : mRows)
        {
            for (MyGUI::TextBox* text : { row.mExplanation, row.mPerformanceNote })
            {
                if (text != nullptr && text->getVisible())
                    text->setSize(text->getWidth(), text->getTextSize().height);
            }
            updateGroupLayout(*row.mWidget);
        }
        auto* contentBox = dynamic_cast<Gui::Box*>(mContent);
        const int contentHeight = contentBox != nullptr ? contentBox->getRequestedSize().height : mContent->getHeight();
        mContent->setSize(width, contentHeight);
        mScroll->setCanvasSize(width, std::max(mScroll->getViewCoord().height, contentHeight));
    }
}

void MWGui::GraphicsSettings::refreshLightingControls(bool supportsClustered)
{
    const auto visibility
        = Settings::getLightingControlVisibility({ supportsClustered, Settings::optimizations().useGpuSceneLighting(),
            Settings::shaders().mClusteredLighting, Settings::shaders().mClassicFalloff });
    auto* clustered = mLayout.getWidget("ClusteredLightingButton")->castType<MyGUI::Button>();
    clustered->setEnabled(visibility.mClusteredConstraint == Settings::ClusteredLightingConstraint::None);
    clustered->setCaptionWithReplacing(visibility.mClusteredEffective ? "#{Interface:On}" : "#{Interface:Off}");

    auto* status = mLayout.getWidget("ClusteredLightingStatus")->castType<MyGUI::TextBox>();
    status->setCaptionWithReplacing(
        visibility.mClusteredConstraint == Settings::ClusteredLightingConstraint::ForcedByGpuSceneLighting
            ? "#{OMWEngine:ClusteredLightingForced}"
            : "#{OMWEngine:ClusteredLightingUnsupported}");
    setContextVisible(*status, visibility.mClusteredConstraint != Settings::ClusteredLightingConstraint::None);
    setContextVisible(*mLayout.getWidget("ClassicFalloffWidget"), visibility.mClassicFalloff);
    setContextVisible(*mLayout.getWidget("MaxLightsGroup"), visibility.mMaximumLights);
    setContextVisible(*mLayout.getWidget("MinimumLightRadius"), visibility.mMinimumLightRadius);
    setContextVisible(*mLayout.getWidget("MinimumBrightness"), visibility.mMinimumBrightness);
    updateLayout();
}

void MWGui::GraphicsSettings::refreshDisplayControls()
{
    const auto visibility = Settings::getDisplayControlVisibility(Settings::video().mWindowMode);
    setContextVisible(*mWindowBorderRow, visibility.mWindowBorder);
    setContextVisible(*mDisplayResolutionGroup, visibility.mResolution);
    setContextVisible(*mWindowModeHint, visibility.mBorderlessHint);
}

void MWGui::GraphicsSettings::refreshOptimizationControls()
{
    const auto& optimizations = Settings::optimizations();
    const auto visible
        = Settings::getOptimizationControlVisibility({ optimizations.mEnabled, optimizations.mGpuDrivenPaging,
            optimizations.mGpuActors, optimizations.mGpuOcclusionCulling, optimizations.mGpuSceneLighting });
    for (std::size_t i = 0; i < mOptimizationButtons.size(); ++i)
    {
        mOptimizationButtons[i]->setEnabled(true);
        setContextVisible(*mOptimizationButtons[i]->getParent(), visible[i]);
    }
}

void MWGui::GraphicsSettings::setContextVisible(MyGUI::Widget& widget, bool visible)
{
    widget.setVisible(visible);
    widget.setUserString("Hidden", visible ? "false" : "true");
    for (MyGUI::Widget* current = widget.getParent(); current != nullptr; current = current->getParent())
        if (auto* box = dynamic_cast<Gui::Box*>(current))
            box->notifyChildrenSizeChanged();
}

void MWGui::GraphicsSettings::updateLayout()
{
    if (mPages[1].mPage->getVisible())
        mDistanceSettings->updateLayout();
    for (std::size_t i = 0; i < mPages.size(); ++i)
        if (i != 1 && mPages[i].mPage->getVisible())
            updatePageLayout(mPages[i]);
    if (mShowFogReviewPosition)
    {
        const MyGUI::IntCoord view = mPages[1].mScroll->getViewCoord();
        const MyGUI::IntSize canvas = mPages[1].mScroll->getCanvasSize();
        mPages[1].mScroll->setViewOffset(MyGUI::IntPoint(0, std::min(0, view.height - canvas.height)));
    }
}

void MWGui::GraphicsSettings::showPage(std::string_view page)
{
    const std::optional<std::size_t> category = page == "display" || page == "video" ? 0
        : page == "visibility" || page == "visibility-fog" || page == "detail"       ? 1
        : page == "atmosphere"                                                       ? 2
        : page == "water"                                                            ? 3
        : page == "lighting" || page == "lights"                                     ? 4
        : page == "shadows"                                                          ? 5
        : page == "advanced" || page == "optimizations"                              ? 6
                                                                                     : std::optional<std::size_t>{};
    if (!category)
        throw std::invalid_argument("unknown video settings page");
    auto* settingsTabs = mLayout.getWidget("SettingsTab")->castType<MyGUI::TabControl>();
    auto* videoPage = mLayout.getWidget("VideoPage")->castType<MyGUI::TabItem>();
    settingsTabs->setItemSelected(videoPage);
    selectCategory(*category);
    mShowFogReviewPosition = page == "visibility-fog";
    updateLayout();
}

void MWGui::GraphicsSettings::onCategoryChanged(MyGUI::ComboBox*, std::size_t position)
{
    mShowFogReviewPosition = false;
    selectCategory(position);
    updateLayout();
}

void MWGui::GraphicsSettings::selectCategory(std::size_t position)
{
    if (position >= mPages.size())
        return;
    for (std::size_t i = 0; i < mPages.size(); ++i)
        mPages[i].mPage->setVisible(i == position);
    if (mCategorySelector->getIndexSelected() != position)
        mCategorySelector->setIndexSelected(position);
}

void MWGui::GraphicsSettings::updatePageLayout(Page& page)
{
    page.mScroll->setVisibleVScroll(true);
    for (int pass = 0; pass < 2; ++pass)
    {
        const MyGUI::IntCoord view = page.mScroll->getViewCoord();
        page.mContent->setSize(view.width, page.mContent->getHeight());
        auto* box = dynamic_cast<Gui::Box*>(page.mContent);
        if (box != nullptr)
            box->notifyChildrenSizeChanged();
        const int contentHeight = box != nullptr ? box->getRequestedSize().height : page.mContent->getHeight();
        page.mContent->setSize(view.width, contentHeight);
        page.mScroll->setCanvasSize(view.width, std::max(view.height, contentHeight));
    }
}
