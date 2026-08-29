#include "diagnosticinputmanager.hpp"

#include "actionmanager.hpp"
#include "actions.hpp"
#include "bindingsmanager.hpp"

#include <components/esm/defs.hpp>

namespace MWInput
{
    DiagnosticInputManager::DiagnosticInputManager(osg::ref_ptr<osgViewer::Viewer> viewer,
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler, const std::filesystem::path& userFile,
        bool userFileExists)
        : mBindingsManager(std::make_unique<BindingsManager>(userFile, userFileExists))
        , mActionManager(std::make_unique<ActionManager>(
              mBindingsManager.get(), std::move(viewer), std::move(screenCaptureHandler)))
    {
    }

    DiagnosticInputManager::~DiagnosticInputManager() = default;

    void DiagnosticInputManager::clear()
    {
        mControlSwitch.clear();
    }

    void DiagnosticInputManager::update(float dt, bool disableControls, bool)
    {
        mControlsDisabled = disableControls;
        if (disableControls)
            return;
        mBindingsManager->update(dt);
        mActionManager->update(dt);
    }

    void DiagnosticInputManager::changeInputMode(bool) {}

    void DiagnosticInputManager::processChangedSettings(const std::set<std::pair<std::string, std::string>>&) {}

    void DiagnosticInputManager::setDragDrop(bool dragDrop)
    {
        mBindingsManager->setDragDrop(dragDrop);
    }

    bool DiagnosticInputManager::isGamepadGuiCursorEnabled()
    {
        return false;
    }

    void DiagnosticInputManager::setGamepadGuiCursorEnabled(bool) {}

    void DiagnosticInputManager::toggleControlSwitch(std::string_view control, bool value)
    {
        mControlSwitch.set(control, value);
    }

    bool DiagnosticInputManager::getControlSwitch(std::string_view control)
    {
        return mControlSwitch.get(control);
    }

    std::string_view DiagnosticInputManager::getActionDescription(int action) const
    {
        return mBindingsManager->getActionDescription(action);
    }

    std::string DiagnosticInputManager::getActionKeyBindingName(int action) const
    {
        return mBindingsManager->getActionKeyBindingName(action);
    }

    std::string DiagnosticInputManager::getActionControllerBindingName(int) const
    {
        return {};
    }

    bool DiagnosticInputManager::actionIsActive(int action) const
    {
        return mBindingsManager->actionIsActive(action);
    }

    float DiagnosticInputManager::getActionValue(int action) const
    {
        return mBindingsManager->getActionValue(action);
    }

    bool DiagnosticInputManager::isControllerButtonPressed(SDL_GameControllerButton) const
    {
        return false;
    }

    float DiagnosticInputManager::getControllerAxisValue(SDL_GameControllerAxis) const
    {
        return 0.f;
    }

    int DiagnosticInputManager::getMouseMoveX() const
    {
        return 0;
    }

    int DiagnosticInputManager::getMouseMoveY() const
    {
        return 0;
    }

    void DiagnosticInputManager::warpMouseToWidget(MyGUI::Widget*) {}

    const std::initializer_list<int>& DiagnosticInputManager::getActionKeySorting()
    {
        return mBindingsManager->getActionKeySorting();
    }

    const std::initializer_list<int>& DiagnosticInputManager::getActionControllerSorting()
    {
        static const std::initializer_list<int> empty;
        return empty;
    }

    int DiagnosticInputManager::getNumActions()
    {
        return A_Last;
    }

    void DiagnosticInputManager::enableDetectingBindingMode(int action, bool keyboard)
    {
        mBindingsManager->enableDetectingBindingMode(action, keyboard);
    }

    void DiagnosticInputManager::resetToDefaultKeyBindings()
    {
        mBindingsManager->loadKeyDefaults(true);
    }

    void DiagnosticInputManager::resetToDefaultControllerBindings() {}

    bool DiagnosticInputManager::joystickLastUsed()
    {
        return false;
    }

    void DiagnosticInputManager::setJoystickLastUsed(bool) {}

    std::string DiagnosticInputManager::getControllerButtonIcon(int)
    {
        return {};
    }

    std::string DiagnosticInputManager::getControllerAxisIcon(int)
    {
        return {};
    }

    std::size_t DiagnosticInputManager::countSavedGameRecords() const
    {
        return mControlSwitch.countSavedGameRecords();
    }

    void DiagnosticInputManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        mControlSwitch.write(writer, progress);
    }

    void DiagnosticInputManager::readRecord(ESM::ESMReader& reader, std::uint32_t type)
    {
        if (type == ESM::REC_INPU)
            mControlSwitch.readRecord(reader, type);
    }

    void DiagnosticInputManager::resetIdleTime()
    {
        mActionManager->resetIdleTime();
    }

    bool DiagnosticInputManager::isIdle() const
    {
        return mActionManager->getIdleTime() > 0.5f;
    }

    void DiagnosticInputManager::executeAction(int action)
    {
        mActionManager->executeAction(action);
    }

    bool DiagnosticInputManager::controlsDisabled()
    {
        return mControlsDisabled;
    }

    void DiagnosticInputManager::saveBindings()
    {
        mBindingsManager->saveBindings();
    }
}
