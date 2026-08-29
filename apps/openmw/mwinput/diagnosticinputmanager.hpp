#ifndef OPENMW_MWINPUT_DIAGNOSTICINPUTMANAGER_HPP
#define OPENMW_MWINPUT_DIAGNOSTICINPUTMANAGER_HPP

#include "../mwbase/inputmanager.hpp"

#include "controlswitch.hpp"

#include <filesystem>
#include <memory>

#include <osg/ref_ptr>
#include <osgViewer/ViewerEventHandlers>

namespace osgViewer
{
    class Viewer;
}

namespace MWInput
{
    class ActionManager;
    class BindingsManager;

    /// Supplies deterministic input state to isolated diagnostics without polling host input devices.
    class DiagnosticInputManager final : public MWBase::InputManager
    {
    public:
        /// Create device-free input state using the isolated diagnostic binding file.
        DiagnosticInputManager(osg::ref_ptr<osgViewer::Viewer> viewer,
            osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler, const std::filesystem::path& userFile,
            bool userFileExists);

        /// Destroy diagnostic input state without writing bindings implicitly.
        ~DiagnosticInputManager() final;

        /// Restore save-independent control switches.
        void clear() override;

        /// Advance deterministic binding and idle state without reading host events.
        void update(float dt, bool disableControls, bool disableEvents = false) override;

        /// Ignore pointer-mode changes because diagnostics own no input device.
        void changeInputMode(bool guiMode) override;

        /// Ignore device setting changes because diagnostics own no input device.
        void processChangedSettings(const std::set<std::pair<std::string, std::string>>& changed) override;

        /// Preserve drag-and-drop binding policy used by action execution.
        void setDragDrop(bool dragDrop) override;

        /// Report that no gamepad cursor exists in the diagnostic child.
        bool isGamepadGuiCursorEnabled() override;

        /// Ignore gamepad cursor requests because diagnostics own no gamepad.
        void setGamepadGuiCursorEnabled(bool enabled) override;

        /// Update one serialized control switch.
        void toggleControlSwitch(std::string_view control, bool value) override;

        /// Return one serialized control switch.
        bool getControlSwitch(std::string_view control) override;

        /// Return the configured action description.
        std::string_view getActionDescription(int action) const override;

        /// Return the configured keyboard binding name.
        std::string getActionKeyBindingName(int action) const override;

        /// Return an empty controller binding because diagnostics own no gamepad.
        std::string getActionControllerBindingName(int action) const override;

        /// Return whether the isolated binding state marks an action active.
        bool actionIsActive(int action) const override;

        /// Return the isolated binding value for an action.
        float getActionValue(int action) const override;

        /// Report that no controller button is pressed.
        bool isControllerButtonPressed(SDL_GameControllerButton button) const override;

        /// Report a neutral controller axis.
        float getControllerAxisValue(SDL_GameControllerAxis axis) const override;

        /// Report no mouse movement.
        int getMouseMoveX() const override;

        /// Report no mouse movement.
        int getMouseMoveY() const override;

        /// Ignore mouse warping because diagnostics own no pointer.
        void warpMouseToWidget(MyGUI::Widget* widget) override;

        /// Return the configured keyboard action order.
        const std::initializer_list<int>& getActionKeySorting() override;

        /// Return an empty controller action order.
        const std::initializer_list<int>& getActionControllerSorting() override;

        /// Return the number of engine actions.
        int getNumActions() override;

        /// Preserve binding detection state without polling devices.
        void enableDetectingBindingMode(int action, bool keyboard) override;

        /// Restore default keyboard bindings in isolated state.
        void resetToDefaultKeyBindings() override;

        /// Ignore controller defaults because diagnostics own no gamepad.
        void resetToDefaultControllerBindings() override;

        /// Report that no joystick supplied the last input.
        bool joystickLastUsed() override;

        /// Ignore device selection because diagnostics own no devices.
        void setJoystickLastUsed(bool enabled) override;

        /// Return no controller icon.
        std::string getControllerButtonIcon(int button) override;

        /// Return no controller icon.
        std::string getControllerAxisIcon(int axis) override;

        /// Return the serialized control-switch record count.
        std::size_t countSavedGameRecords() const override;

        /// Write serialized control switches.
        void write(ESM::ESMWriter& writer, Loading::Listener& progress) override;

        /// Read serialized control switches.
        void readRecord(ESM::ESMReader& reader, std::uint32_t type) override;

        /// Reset deterministic idle time.
        void resetIdleTime() override;

        /// Return whether the child has received no synthetic action for the idle interval.
        bool isIdle() const override;

        /// Execute an explicitly requested engine action through the normal action owner.
        void executeAction(int action) override;

        /// Return whether the current update disables controls.
        bool controlsDisabled() override;

        /// Save keyboard bindings only to isolated writable state.
        void saveBindings() override;

    private:
        bool mControlsDisabled = false;
        std::unique_ptr<BindingsManager> mBindingsManager;
        ControlSwitch mControlSwitch;
        std::unique_ptr<ActionManager> mActionManager;
    };
}

#endif
