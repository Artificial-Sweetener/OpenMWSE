#ifndef OPENMW_MWGUI_GRAPHICSSETTINGS_H
#define OPENMW_MWGUI_GRAPHICSSETTINGS_H

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <MyGUI_ComboBox.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ScrollBar.h>

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

        /// Reflow distance-setting help after the owning window changes size.
        void updateLayout();

        /// Select a named page within the video settings tab.
        void showPage(std::string_view page);

        /// Exercise live distance and optional shadow-resolution controls with isolated diagnostic values.
        void setDistanceValues(
            bool useCellEditor, float view, float grass, float shadow, std::optional<int> resolution);

    private:
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

        /// Synchronize the shadow resolution field and increment boundaries.
        void refreshShadowResolution();
        /// Synchronize dependent atmosphere and shadow control states.
        void refreshAtmosphereControls();

        Layout& mLayout;
        std::function<void()> mApply;
        std::unique_ptr<DistanceSettings> mDistanceSettings;
        MyGUI::Button* mCloudShadowsButton;
        MyGUI::ScrollBar* mCloudShadowIntensity;
        MyGUI::Button* mReflectCloudsButton;
        MyGUI::ComboBox* mVolumetricQuality;
        MyGUI::ComboBox* mAtmosphereAppearance;
        MyGUI::ComboBox* mShadowFilteringQuality;
        MyGUI::Button* mDistantShadowsButton;
        MyGUI::ComboBox* mDistantShadowQuality;
        MyGUI::EditBox* mShadowMapResolution;
        MyGUI::Button* mShadowResolutionDecrease;
        MyGUI::Button* mShadowResolutionIncrease;
    };
}

#endif
