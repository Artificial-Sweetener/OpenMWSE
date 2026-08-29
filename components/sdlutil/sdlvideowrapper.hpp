#ifndef OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H
#define OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H

#include <osg/ref_ptr>

#include <SDL_types.h>

#include "vsyncmode.hpp"

struct SDL_Window;

namespace osgViewer
{
    class Viewer;
}

namespace Settings
{
    enum class WindowMode;
}

namespace SDLUtil
{

    class VideoWrapper
    {
    public:
        /// Control viewer synchronization and, when supplied, the interactive SDL window.
        VideoWrapper(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer);

        /// Restore interactive window state when a platform window is present.
        ~VideoWrapper();

        /// Apply the requested synchronization mode to the viewer graphics contexts.
        void setSyncToVBlank(VSyncMode vsyncMode);

        /// Apply display gamma and contrast when an interactive window is present.
        void setGammaContrast(float gamma, float contrast);

        /// Apply display mode changes when an interactive window is present.
        void setVideoMode(int width, int height, Settings::WindowMode windowMode, bool windowBorder);

        /// Center the interactive window when one is present.
        void centerWindow();

    private:
        SDL_Window* mWindow;
        osg::ref_ptr<osgViewer::Viewer> mViewer;

        float mGamma;
        float mContrast;
        bool mHasSetGammaContrast;

        // Store system gamma ramp on window creation. Restore system gamma ramp on exit
        Uint16 mOldSystemGammaRamp[256 * 3];
    };

}

#endif
