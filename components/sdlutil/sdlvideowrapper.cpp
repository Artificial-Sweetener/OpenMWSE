#include "sdlvideowrapper.hpp"

#include <components/debug/debuglog.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/settings/settings.hpp>

#include <osgViewer/Viewer>

#include <SDL_video.h>

#include <algorithm>
#include <cmath>

namespace SDLUtil
{

    VideoWrapper::VideoWrapper(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer)
        : mWindow(window)
        , mViewer(std::move(viewer))
        , mGamma(1.f)
        , mContrast(1.f)
        , mHasSetGammaContrast(false)
    {
        if (mWindow)
            SDL_GetWindowGammaRamp(mWindow, mOldSystemGammaRamp, &mOldSystemGammaRamp[256], &mOldSystemGammaRamp[512]);
    }

    VideoWrapper::~VideoWrapper()
    {
        if (!mWindow)
            return;

        SDL_SetWindowFullscreen(mWindow, 0);

        // If user hasn't touched the defaults no need to restore
        if (mHasSetGammaContrast)
            SDL_SetWindowGammaRamp(mWindow, mOldSystemGammaRamp, &mOldSystemGammaRamp[256], &mOldSystemGammaRamp[512]);
    }

    void VideoWrapper::setSyncToVBlank(VSyncMode vsyncMode)
    {
        osgViewer::Viewer::Windows windows;
        mViewer->getWindows(windows);
        mViewer->stopThreading();
        for (osgViewer::Viewer::Windows::iterator it = windows.begin(); it != windows.end(); ++it)
        {
            osgViewer::GraphicsWindow* win = *it;
            if (GraphicsWindowSDL2* sdl2win = dynamic_cast<GraphicsWindowSDL2*>(win))
                sdl2win->setSyncToVBlank(vsyncMode);
            else
                win->setSyncToVBlank(vsyncMode != VSyncMode::Disabled);
        }
        mViewer->startThreading();
    }

    void VideoWrapper::setGammaContrast(float gamma, float contrast)
    {
        if (gamma == mGamma && contrast == mContrast)
            return;

        mGamma = gamma;
        mContrast = contrast;

        if (!mWindow)
            return;

        mHasSetGammaContrast = true;

        Uint16 red[256], green[256], blue[256];
        for (int i = 0; i < 256; i++)
        {
            float k = i / 256.0f;
            k = (k - 0.5f) * contrast + 0.5f;
            k = pow(k, 1.f / gamma);
            k *= 256;
            float value = k * 256;
            if (value > 65535)
                value = 65535;
            else if (value < 0)
                value = 0;

            red[i] = green[i] = blue[i] = static_cast<Uint16>(value);
        }
        if (SDL_SetWindowGammaRamp(mWindow, red, green, blue) < 0)
            Log(Debug::Warning) << "Couldn't set gamma: " << SDL_GetError();
    }

    void VideoWrapper::setVideoMode(int width, int height, Settings::WindowMode windowMode, bool windowBorder)
    {
        if (!mWindow)
            return;

        SDL_SetWindowFullscreen(mWindow, 0);

        if (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_MAXIMIZED)
            SDL_RestoreWindow(mWindow);

        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(mWindow, &dw, &dh);

        const float drawableScaleX = w > 0 ? static_cast<float>(dw) / w : 1.f;
        const float drawableScaleY = h > 0 ? static_cast<float>(dh) / h : 1.f;

        if (windowMode == Settings::WindowMode::Fullscreen)
        {
            SDL_DisplayMode mode;
            SDL_GetWindowDisplayMode(mWindow, &mode);
            mode.w = static_cast<int>(std::round(width / drawableScaleX));
            mode.h = static_cast<int>(std::round(height / drawableScaleY));
            SDL_SetWindowDisplayMode(mWindow, &mode);
            SDL_SetWindowFullscreen(mWindow, SDL_WINDOW_FULLSCREEN);
        }
        else
        {
            if (windowMode == Settings::WindowMode::WindowedFullscreen)
            {
                SDL_DisplayMode desktop{};
                const int display = SDL_GetWindowDisplayIndex(mWindow);
                if (display >= 0 && SDL_GetDesktopDisplayMode(display, &desktop) == 0)
                {
                    const auto clamped = Settings::clampWindowSize(windowMode, width, height, desktop.w, desktop.h);
                    width = clamped.first;
                    height = clamped.second;
                }
            }
            SDL_SetWindowSize(mWindow, std::max(1, static_cast<int>(std::round(width / drawableScaleX))),
                std::max(1, static_cast<int>(std::round(height / drawableScaleY))));
            const bool bordered = windowMode == Settings::WindowMode::Windowed && windowBorder;
            SDL_SetWindowBordered(mWindow, bordered ? SDL_TRUE : SDL_FALSE);

            centerWindow();
        }
    }

    void VideoWrapper::centerWindow()
    {
        if (!mWindow)
            return;

        // Resize breaks the sdl window in some cases; see issue: #5539
        SDL_Rect rect{};
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        auto index = SDL_GetWindowDisplayIndex(mWindow);
        SDL_GetDisplayBounds(index, &rect);
        SDL_GetWindowSize(mWindow, &w, &h);

        x = rect.x;
        y = rect.y;

        // Center dimensions that do not fill the screen
        if (w < rect.w)
        {
            x = rect.x + rect.w / 2 - w / 2;
        }
        if (h < rect.h)
        {
            y = rect.y + rect.h / 2 - h / 2;
        }

        SDL_SetWindowPosition(mWindow, x, y);
    }

}
