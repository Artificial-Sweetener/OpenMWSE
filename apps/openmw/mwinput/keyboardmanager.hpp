#ifndef MWINPUT_MWKEYBOARDMANAGER_H
#define MWINPUT_MWKEYBOARDMANAGER_H

#include <components/sdlutil/events.hpp>

#include <functional>

namespace MWInput
{
    class BindingsManager;

    class KeyboardManager : public SDLUtil::KeyListener
    {
    public:
        /// Route keyboard events and reserve Shift+F11 for the optional diagnostic recording toggle.
        KeyboardManager(BindingsManager* bindingsManager, std::function<void()> diagnosticRouteToggle = {});

        virtual ~KeyboardManager() = default;

        void textInput(const SDL_TextInputEvent& arg) override;

        /// Route a key press, consuming the reserved diagnostic shortcut when enabled.
        void keyPressed(const SDL_KeyboardEvent& arg) override;

        /// Route a key release without leaking a consumed diagnostic shortcut into normal bindings.
        void keyReleased(const SDL_KeyboardEvent& arg) override;

    private:
        BindingsManager* mBindingsManager;
        std::function<void()> mDiagnosticRouteToggle;
        bool mDiagnosticRouteShortcutPressed = false;
    };
}
#endif
