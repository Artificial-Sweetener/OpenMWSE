#ifndef OPENMW_MWGUI_GRAPHICSSETTINGS_H
#define OPENMW_MWGUI_GRAPHICSSETTINGS_H

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <MyGUI_ComboBox.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_Widget.h>

#include "distancesettings.hpp"

namespace MWGui
{
    class Layout;

    /// Owns fork-specific live graphics controls and their diagnostic exercise path.
    class GraphicsSettings
    {
    public:
        /// Bind graphics widgets and route accepted changes through the settings window apply callback.
        GraphicsSettings(Layout& layout, std::function<void()> apply);

        /// Refresh effective control state and canonical numeric presentation.
        void refresh();

        /// Refresh lighting controls whose effective availability depends on renderer support and GPU-scene policy.
        void refreshLightingControls(bool supportsClustered);

        /// Reflow distance-setting help after the owning window changes size.
        void updateLayout();

        /// Select a named page within the video settings tab.
        void showPage(std::string_view page);

        /// Exercise live distance and optional shadow-resolution controls with isolated diagnostic values.
        void setDistanceValues(
            bool useCellEditor, float view, float grass, float shadow, std::optional<int> resolution);

        /// Exercise the live exterior fog range and independent endpoint locks with isolated diagnostic values.
        void setFogValues(bool useCellEditor, bool enabled, float start, float end, bool startLocked, bool endLocked);

        /// Exercise the live geometric-wave slider with an isolated diagnostic value.
        void setGeometricWaveHeight(float height);

        /// Exercise only changed live water presentation and optical controls with isolated diagnostic values.
        void setWaterValues(std::string_view appearance, float height, bool caustics, float causticsStrength,
            int interiorLocalLights, int exteriorLocalLights, bool meshWater, float meshHeight,
            bool sharedPlanarReflections);

    private:
        /// Retain one category page and its responsive scrolling content.
        struct Page
        {
            MyGUI::Widget* mPage;
            MyGUI::ScrollView* mScroll;
            MyGUI::Widget* mContent;
        };

        /// Show the selected graphics category without exposing implementation pages as tabs.
        void onCategoryChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Reflow one category to the available viewport and reserve the scrollbar gutter when needed.
        void updatePageLayout(Page& page);
        /// Change the visible category and synchronize its selector.
        void selectCategory(std::size_t position);
        /// Preserve the aggregate shadow enable setting after a caster category changes.
        void onShadowCasterChanged(MyGUI::Widget* sender);
        /// Accept and apply a whole-pixel shadow-map resolution.
        void onShadowResolutionAccepted(MyGUI::EditBox* sender);
        /// Commit shadow resolution edits before focus moves to another control.
        void onShadowResolutionFocusLost(MyGUI::Widget* sender, MyGUI::Widget* next);
        /// Step shadow-map resolution within renderer and hardware limits.
        void onShadowResolutionStep(MyGUI::Widget* sender);
        /// Apply the selected volumetric quality immediately.
        void onVolumetricQualityChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Apply the selected geometry-shadow filtering policy immediately.
        void onShadowFilteringQualityChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Apply the selected distant-shadow quality immediately.
        void onDistantShadowQualityChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Apply the selected atmospheric appearance immediately.
        void onAtmosphereAppearanceChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Apply the selected water appearance immediately.
        void onWaterAppearanceChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Refresh mesh-water controls after the live replacement policy changes.
        void onMeshWaterChanged(MyGUI::Widget* sender);
        /// Apply the enhanced-water color treatment immediately.
        void onEnhancedWaterColorChanged(MyGUI::ComboBox* sender, std::size_t position);
        /// Refresh caustic-strength availability after either live prerequisite changes.
        void onWaterCausticsChanged(MyGUI::Widget* sender);
        /// Refresh subordinate optimization availability after the prerequisite changes.
        void onOptimizationPrerequisiteChanged(MyGUI::Widget* sender);

        /// Synchronize the shadow resolution field and increment boundaries.
        void refreshShadowResolution();
        /// Synchronize controls that only apply to the selected window mode.
        void refreshDisplayControls();
        /// Synchronize dependent atmosphere and shadow control states.
        void refreshAtmosphereControls();
        /// Synchronize subordinate optimization availability without changing saved selections.
        void refreshOptimizationControls();
        /// Synchronize water choices and dependent enhanced controls without discarding preferences.
        void refreshWaterControls();
        /// Show or collapse a contextual group and notify each owning box to reflow immediately.
        void setContextVisible(MyGUI::Widget& widget, bool visible);

        Layout& mLayout;
        std::function<void()> mApply;
        MyGUI::ComboBox* mCategorySelector;
        std::array<Page, 7> mPages;
        bool mShowFogReviewPosition = false;
        std::unique_ptr<DistanceSettings> mDistanceSettings;
        MyGUI::Widget* mWindowBorderRow;
        MyGUI::Widget* mDisplayResolutionGroup;
        MyGUI::Widget* mWindowModeHint;
        MyGUI::Widget* mAtmosphereAppearanceGroup;
        MyGUI::Widget* mAtmosphereQualityGroup;
        MyGUI::Button* mCloudShadowsButton;
        MyGUI::ScrollBar* mCloudShadowIntensity;
        MyGUI::Button* mReflectCloudsButton;
        MyGUI::ComboBox* mVolumetricQuality;
        MyGUI::ComboBox* mAtmosphereAppearance;
        MyGUI::ComboBox* mWaterAppearance;
        MyGUI::ScrollBar* mGeometricWaveHeight;
        MyGUI::Button* mMeshWater;
        MyGUI::Button* mSharedPlanarReflections;
        MyGUI::ScrollBar* mMeshGeometricWaveHeight;
        MyGUI::Button* mWaterCaustics;
        MyGUI::ScrollBar* mWaterCausticsStrength;
        MyGUI::ScrollBar* mInteriorWaterLights;
        MyGUI::ScrollBar* mExteriorWaterLights;
        MyGUI::ComboBox* mEnhancedWaterColor;
        std::array<MyGUI::Button*, 3> mEnhancedWaterButtons;
        MyGUI::Widget* mWaterShaderControls;
        MyGUI::Widget* mMeshWaterControls;
        MyGUI::Widget* mEnhancedWaterControls;
        MyGUI::Widget* mWaterCausticsStrengthGroup;
        MyGUI::ComboBox* mShadowFilteringQuality;
        MyGUI::Button* mDistantShadowsButton;
        MyGUI::ComboBox* mDistantShadowQuality;
        MyGUI::EditBox* mShadowMapResolution;
        MyGUI::Button* mShadowResolutionDecrease;
        MyGUI::Button* mShadowResolutionIncrease;
        MyGUI::Widget* mShadowFilteringGroup;
        MyGUI::Widget* mDistantShadowsGroup;
        MyGUI::Widget* mDistantShadowQualityGroup;
        MyGUI::Widget* mCloudShadowIntensityGroup;
        MyGUI::Widget* mGeometryShadowControls;
        MyGUI::Button* mOptimizationsEnabled;
        std::array<MyGUI::Button*, 12> mOptimizationButtons;
    };
}

#endif
