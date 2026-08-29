#include "engine.hpp"

#include <cerrno>
#include <chrono>
#include <future>
#include <system_error>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgViewer/Renderer>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/session.hpp>
#include <components/files/hash.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/offscreencontext.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include <yaml-cpp/yaml.h>

#include <iomanip>

#include "mwinput/diagnosticinputmanager.hpp"
#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/weather.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/camera.hpp"
#include "mwrender/postprocessor.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    /// Convert UTF-8 text into the protocol's byte payload without an implicit terminator.
    std::vector<std::byte> diagnosticBytes(std::string_view text)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        return { begin, begin + text.size() };
    }

    /// Convert a protocol payload to text after rejecting embedded null bytes.
    std::string diagnosticText(std::span<const std::byte> payload)
    {
        std::string result(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (result.find('\0') != std::string::npos)
            throw std::invalid_argument("diagnostic text payload contains an embedded null byte");
        return result;
    }

    /// Quote a scalar for the small YAML manifests emitted by core diagnostics.
    std::string quoteDiagnosticYaml(std::string_view value)
    {
        std::string result = "\"";
        for (const char character : value)
        {
            if (character == '\\' || character == '"')
                result.push_back('\\');
            if (character == '\n')
                result += "\\n";
            else if (character != '\r')
                result.push_back(character);
        }
        result.push_back('"');
        return result;
    }

    /// Format the repository's 128-bit content identity without ambiguous variable-width fields.
    std::string formatDiagnosticHash(const std::array<std::uint64_t, 2>& hash)
    {
        std::ostringstream output;
        output << std::hex << std::setfill('0') << std::setw(16) << hash[0] << std::setw(16) << hash[1];
        return output.str();
    }

    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }

    /// Enable the engine, resource, frame-pacing, and graphics-context measurements used by diagnostic providers.
    void enableDiagnosticStatistics(osgViewer::Viewer& viewer)
    {
        osg::Stats* viewerStats = viewer.getViewerStats();
        for (const char* category : { "engine", "resource", "frame_rate", "event", "update", "scene" })
            viewerStats->collectStats(category, true);

        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
        {
            camera->getStats()->collectStats("gpu", true);
            camera->getStats()->collectStats("rendering", true);
            camera->getStats()->collectStats("scene", true);
        }
    }

    /// Select the newest frame whose asynchronous render measurements have reached the viewer statistics store.
    unsigned diagnosticStatisticsFrame(unsigned currentFrame) noexcept
    {
        constexpr unsigned renderStatisticsLatency = 3;
        return currentFrame > renderStatisticsLatency ? currentFrame - renderStatisticsLatency : 0;
    }
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    mEnvironment.setFrameDuration(frametime);

    try
    {
        // Stop the background GC started at the previous frame's end.
        // Input handling can run Lua (the menu key does), so the state
        // must not be collected from this point on.
        mLuaWorker->finishGc();

        processDiagnosticCommands(frameNumber);

        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mDiagnosticChild && !mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else if (!mDiagnosticChild)
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
        stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    mViewer->eventTraversal();
    mViewer->updateTraversal();

    // update focus object for GUI
    {
        ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
        mWorld->updateFocusObject();
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    mViewer->renderingTraversals();

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);

    // The Lua state is unused until the next frame starts: the worker collects
    // garbage through the frame tail and the framerate-limiter sleep.
    mLuaWorker->gc();

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mDrawableScale(1.f)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createGraphicsContext()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    osg::ref_ptr<osg::GraphicsContext> graphicsContext;
    if (mDiagnosticChild)
    {
        while (!graphicsContext || !graphicsContext->valid())
        {
            graphicsContext
                = SceneUtil::createOffscreenContext({ .mWidth = width, .mHeight = height, .mSamples = antialiasing });
            if (graphicsContext && graphicsContext->valid())
                break;
            if (antialiasing == 0)
                throw std::runtime_error("Failed to create offscreen diagnostic graphics context");
            Log(Debug::Warning) << "Warning: " << antialiasing << "x offscreen antialiasing not supported, trying "
                                << antialiasing / 2;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
        }
        Log(Debug::Info) << "OpenMWSE diagnostic child uses a windowless pbuffer graphics context";
    }
    else
    {
        int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
        {
            posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
            posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        }

        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
        if (windowMode == Settings::WindowMode::Fullscreen)
            flags |= SDL_WINDOW_FULLSCREEN;
        else if (windowMode == Settings::WindowMode::WindowedFullscreen)
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

        // Allows for Windows snapping features to properly work in borderless window
        SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
        SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
        if (!windowBorder)
            flags |= SDL_WINDOW_BORDERLESS;
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

        checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
        if (Debug::shouldDebugOpenGL())
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));
        if (antialiasing > 0)
        {
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
        }

        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> windowContext;
        while (!windowContext || !windowContext->valid())
        {
            while (!mWindow)
            {
                mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
                if (mWindow)
                    break;
                if (antialiasing == 0)
                    throw std::runtime_error("Failed to create SDL window: " + std::string(SDL_GetError()));
                Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                    << antialiasing / 2;
                antialiasing /= 2;
                Settings::video().mAntialiasing.set(antialiasing);
                checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            }

            int windowWidth = 0;
            int windowHeight = 0;
            SDL_GetWindowSize(mWindow, &windowWidth, &windowHeight);
            int drawableWidth = 0;
            int drawableHeight = 0;
            SDL_GL_GetDrawableSize(mWindow, &drawableWidth, &drawableHeight);
            mDrawableScale = static_cast<float>(drawableWidth / windowWidth);
            if (drawableWidth != windowWidth || drawableHeight != windowHeight)
                SDL_SetWindowSize(
                    mWindow, width / (drawableWidth / windowWidth), height / (drawableHeight / windowHeight));

            setWindowIcon();

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
            SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
            traits->windowName = SDL_GetWindowTitle(mWindow);
            traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
            traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
            traits->vsync = 0;
            traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

            windowContext = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
            if (!windowContext->valid())
                throw std::runtime_error("Failed to create GraphicsContext");
            if (traits->samples < antialiasing)
            {
                Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                    << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
                windowContext->closeImplementation();
                SDL_DestroyWindow(mWindow);
                mWindow = nullptr;
                antialiasing /= 2;
                Settings::video().mAntialiasing.set(antialiasing);
                checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                continue;
            }
        }
        graphicsContext = windowContext;
    }

    const osg::GraphicsContext::Traits* traits = graphicsContext->getTraits();
    if (traits->red < 8)
        Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
    if (traits->green < 8)
        Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
    if (traits->blue < 8)
        Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
    if (traits->depth < 24)
        Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";
    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsContext);
    camera->setViewport(0, 0, graphicsContext->getTraits()->width, graphicsContext->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = leftEyeOffset,
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = rightEyeOffset,
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsContext->getTraits()->width, graphicsContext->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(
        mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createGraphicsContext();

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
        static_cast<float>(Settings::general().mAnisotropy));
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mDrawableScale, mViewer, guiRoot,
        mResourceSystem.get(), mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage,
        mEncoding, mExportFonts, Version::getOpenmwVersionDescription(), mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    if (mDiagnosticChild)
    {
        mInputManager = std::make_unique<MWInput::DiagnosticInputManager>(
            mViewer, mScreenCaptureHandler, keybinderUser, keybinderUserExists);
    }
    else
    {
        mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
            keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    }
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mWindowManager->setStore(mWorld->getStore());

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue);
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);
    mWindowManager->initUI();
    mLuaManager->initPostLoad();

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->getCamera()->getOrCreateStateSet()->removeAttribute(osg::StateAttribute::MATERIAL);
    SceneUtil::disableFFPStateForRenderer(static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer()));
    mViewer->setReleaseContextAtEndOfFrameHint(false);

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    prepareEngine();

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

    // Start the game
    if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    if (!mDiagnosticEndpoint.empty())
    {
        std::filesystem::create_directories(mDiagnosticArtifactRoot);
        enableDiagnosticStatistics(*mViewer);
        mDiagnosticSession = std::make_unique<Diagnostics::Session>(mDiagnosticEndpoint);
        Log(Debug::Info) << "OpenMWSE diagnostic bridge ready at " << mDiagnosticEndpoint;
    }

    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        const double realtimeDt = std::chrono::duration_cast<std::chrono::duration<double>>(
                                      std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                                      .count()
            * timeManager.getSimulationTimeScale();
        const double dt
            = mDiagnosticChild ? (mDiagnosticFramesRemaining > 0 ? mDiagnosticFixedFrameDuration : 0.0) : realtimeDt;

        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, static_cast<float>(dt)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }
        if (mDiagnosticChild && mDiagnosticFramesRemaining > 0)
            --mDiagnosticFramesRemaining;

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

        frameRateLimiter.limit();
    }

    mLuaWorker->join();
    if (mDiagnosticSession)
        mDiagnosticSession->stop();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}

void OMW::Engine::configureDiagnostics(
    std::string endpoint, std::filesystem::path artifactRoot, bool child, double fixedFrameDuration)
{
    if (endpoint.empty() || artifactRoot.empty())
        throw std::invalid_argument("diagnostic session requires an endpoint and artifact root");
    if (!(fixedFrameDuration > 0.0 && fixedFrameDuration <= 0.2))
        throw std::invalid_argument("diagnostic fixed frame duration must be within (0, 0.2] seconds");
    mDiagnosticEndpoint = std::move(endpoint);
    mDiagnosticArtifactRoot = std::move(artifactRoot);
    mDiagnosticChild = child;
    mDiagnosticFixedFrameDuration = fixedFrameDuration;
    if (child)
    {
        mUseSound = false;
        mGrab = false;
    }
}

void OMW::Engine::processDiagnosticCommands(unsigned frameNumber)
{
    if (!mDiagnosticSession)
        return;

    for (const Diagnostics::Command& command : mDiagnosticSession->drain())
    {
        Diagnostics::Response response{ command.mRequestId, Diagnostics::ErrorCode::None, {} };
        try
        {
            std::ostringstream output;
            MWWorld::DateTimeManager& time = *mWorld->getTimeManager();
            const MWRender::Camera* camera = mWorld->getCamera();
            const osg::Vec3d cameraPosition = camera->getPosition();
            const MWWorld::Weather& weather = mWorld->getCurrentWeather();

            switch (command.mType)
            {
                case Diagnostics::CommandType::Hello:
                case Diagnostics::CommandType::InspectSession:
                    if (!command.mPayload.empty())
                    {
                        const YAML::Node expected = YAML::Load(diagnosticText(command.mPayload));
                        const YAML::Node expectedContent = expected["content"];
                        if (!expectedContent.IsSequence() || expectedContent.size() != mContentFiles.size())
                        {
                            response.mError = Diagnostics::ErrorCode::ContentMismatch;
                            response.mPayload = diagnosticBytes("content list length does not match the capsule\n");
                            break;
                        }
                        bool mismatch = false;
                        for (std::size_t index = 0; index < mContentFiles.size(); ++index)
                        {
                            const std::string expectedName = expectedContent[index]["name"].as<std::string>();
                            const std::string expectedHash = expectedContent[index]["hash"].as<std::string>("");
                            if (expectedName != mContentFiles[index])
                            {
                                mismatch = true;
                                output << "content order mismatch at " << index << ": expected " << expectedName
                                       << ", loaded " << mContentFiles[index] << '\n';
                                break;
                            }
                            const Files::IStreamPtr stream = mVFS->find(VFS::Path::Normalized(mContentFiles[index]));
                            if (!stream
                                || formatDiagnosticHash(Files::getHash(mContentFiles[index], *stream)) != expectedHash)
                            {
                                mismatch = true;
                                output << "content hash mismatch for " << mContentFiles[index] << '\n';
                                break;
                            }
                        }
                        if (mismatch)
                        {
                            response.mError = Diagnostics::ErrorCode::ContentMismatch;
                            response.mPayload = diagnosticBytes(output.str());
                            break;
                        }
                    }
                    output << "protocol: " << Diagnostics::protocolVersion << '\n'
                           << "build: " << quoteDiagnosticYaml(Version::getOpenmwVersionDescription()) << '\n'
                           << "mode: " << (mDiagnosticChild ? "isolated-child" : "live") << '\n'
                           << "graphics-context: " << (mDiagnosticChild ? "offscreen-pbuffer" : "interactive-window")
                           << '\n'
                           << "native-window: " << (mDiagnosticChild ? "absent" : "present") << '\n'
                           << "input-source: " << (mDiagnosticChild ? "device-free" : "host-devices") << '\n'
                           << "frame: " << frameNumber << '\n'
                           << "graphics-capture: "
                           << (mWorld->getPostProcessor()->atmosphereDiagnosticCaptureInProgress() ? "in-progress"
                                                                                                   : "idle")
                           << '\n'
                           << "providers: [core@1, graphics@1, gameplay@1, performance@1]\n";
                    response.mPayload = diagnosticBytes(output.str());
                    break;

                case Diagnostics::CommandType::StepFrames:
                {
                    if (!mDiagnosticChild)
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }
                    if (command.mPayload.size() != sizeof(std::uint32_t))
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        break;
                    }
                    std::uint32_t count = 0;
                    for (std::size_t byte = 0; byte < sizeof(count); ++byte)
                        count |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(command.mPayload[byte]))
                            << (byte * 8);
                    if (count == 0 || count > 10000)
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        break;
                    }
                    mDiagnosticFramesRemaining = count;
                    output << "scheduled-frames: " << count << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::CaptureGraphics:
                {
                    const std::filesystem::path outputDirectory
                        = mDiagnosticArtifactRoot / ("graphics-" + std::to_string(frameNumber));
                    mWorld->getPostProcessor()->requestAtmosphereDiagnosticCapture(outputDirectory);
                    output << "capture-root: " << quoteDiagnosticYaml(outputDirectory.filename().string()) << '\n'
                           << "frame: " << frameNumber << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::TraceGameplay:
                    output << "provider: gameplay@1\nframe: " << frameNumber << '\n'
                           << "state: " << static_cast<int>(mStateManager->getState()) << '\n'
                           << "cell: " << quoteDiagnosticYaml(mWorld->getCellName()) << '\n'
                           << "camera: [" << cameraPosition.x() << ", " << cameraPosition.y() << ", "
                           << cameraPosition.z() << "]\n"
                           << "weather: " << quoteDiagnosticYaml(weather.mId.serializeText()) << '\n'
                           << "game-time-seconds: " << time.getGameTime() << '\n'
                           << "simulation-time: " << time.getSimulationTime() << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;

                case Diagnostics::CommandType::TracePerformance:
                {
                    const unsigned sampledFrame = diagnosticStatisticsFrame(frameNumber);
                    output << "provider: performance@1\nframe: " << frameNumber << '\n'
                           << "sampled-frame: " << sampledFrame << '\n'
                           << "fixed-step: " << (mDiagnosticChild ? "true" : "false") << '\n'
                           << "simulation-dt: " << mEnvironment.getFrameDuration() << '\n';
                    reportStats(sampledFrame, *mViewer, output);
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::CaptureCapsule:
                {
                    const std::string requestedName = diagnosticText(command.mPayload);
                    if (requestedName.empty()
                        || requestedName != std::filesystem::path(requestedName).filename().string())
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }

                    Diagnostics::Capsule capsule;
                    std::ostringstream manifest;
                    manifest << "capsule-version: " << Diagnostics::capsuleVersion << '\n'
                             << "build: " << quoteDiagnosticYaml(Version::getOpenmwVersionDescription()) << '\n'
                             << "frame: " << frameNumber << '\n'
                             << "providers: [core@1, graphics@1, gameplay@1, performance@1]\n"
                             << "content:\n";
                    for (const std::string& content : mContentFiles)
                    {
                        manifest << "  - name: " << quoteDiagnosticYaml(content) << '\n';
                        if (const Files::IStreamPtr stream = mVFS->find(VFS::Path::Normalized(content)))
                        {
                            const auto hash = Files::getHash(content, *stream);
                            manifest << "    hash: " << formatDiagnosticHash(hash) << '\n';
                        }
                    }
                    capsule.add(
                        { "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest, diagnosticBytes(manifest.str()) });

                    std::ostringstream configuration;
                    configuration << "video:\n  resolution: [" << Settings::video().mResolutionX << ", "
                                  << Settings::video().mResolutionY
                                  << "]\n  antialiasing: " << Settings::video().mAntialiasing
                                  << "\ncamera:\n  near-clip: " << Settings::camera().mNearClip
                                  << "\n  viewing-distance: " << Settings::camera().mViewingDistance
                                  << "\n  field-of-view: " << Settings::camera().mFieldOfView
                                  << "\n  reverse-z: " << (Settings::camera().mReverseZ ? "true" : "false")
                                  << "\nshaders:\n  atmospheric-effects: "
                                  << (Settings::shaders().mAtmosphericEffects ? "true" : "false")
                                  << "\n  atmospheric-quality: " << Settings::shaders().mAtmosphericQuality << '\n';
                    capsule.add({ "configuration/effective.yaml", Diagnostics::CapsuleEntryType::Configuration,
                        diagnosticBytes(configuration.str()) });

                    std::ostringstream world;
                    world << "cell: " << quoteDiagnosticYaml(mWorld->getCellName()) << '\n'
                          << "frame: " << frameNumber << '\n'
                          << "camera: [" << cameraPosition.x() << ", " << cameraPosition.y() << ", "
                          << cameraPosition.z() << "]\n"
                          << "camera-angles: [" << camera->getPitch() << ", " << camera->getYaw() << ", "
                          << camera->getRoll() << "]\n"
                          << "weather: " << quoteDiagnosticYaml(weather.mId.serializeText()) << '\n'
                          << "game-time-seconds: " << time.getGameTime() << '\n'
                          << "simulation-time: " << time.getSimulationTime() << '\n'
                          << "rendering-time: " << time.getRenderingSimulationTime() << '\n';
                    capsule.add({ "state/world.yaml", Diagnostics::CapsuleEntryType::GameplayTrace,
                        diagnosticBytes(world.str()) });

                    if (mStateManager->getState() == MWBase::StateManager::State_Running)
                    {
                        capsule.add({ "state/world.omwsave", Diagnostics::CapsuleEntryType::WorldSnapshot,
                            mStateManager->captureDiagnosticSnapshot("OpenMWSE diagnostic capsule") });
                    }

                    std::ostringstream performance;
                    const unsigned sampledFrame = diagnosticStatisticsFrame(frameNumber);
                    performance << "sampled-frame: " << sampledFrame << '\n';
                    reportStats(sampledFrame, *mViewer, performance);
                    capsule.add({ "performance/frame.txt", Diagnostics::CapsuleEntryType::PerformanceTrace,
                        diagnosticBytes(performance.str()) });

                    const std::filesystem::path destination = mDiagnosticArtifactRoot / requestedName;
                    Diagnostics::writeCapsuleAtomically(destination, capsule);
                    output << "capsule: " << quoteDiagnosticYaml(destination.filename().string()) << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::ReloadShaders:
                    mWorld->getPostProcessor()->triggerShaderReload();
                    break;

                case Diagnostics::CommandType::ShutdownChild:
                    if (!mDiagnosticChild)
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                    else
                        mStateManager->requestQuit();
                    break;

                case Diagnostics::CommandType::SetWorldState:
                {
                    if (!mDiagnosticChild)
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }
                    const YAML::Node state = YAML::Load(diagnosticText(command.mPayload));
                    mViewer->getFrameStamp()->setFrameNumber(state["frame"].as<unsigned>());
                    time.setSimulationTime(state["simulation-time"].as<double>());
                    time.setRenderingSimulationTime(state["rendering-time"].as<double>());
                    const YAML::Node angles = state["camera-angles"];
                    if (!angles.IsSequence() || angles.size() != 3)
                        throw std::invalid_argument("diagnostic world state has invalid camera angles");
                    MWRender::Camera* mutableCamera = mWorld->getCamera();
                    mutableCamera->setPitch(angles[0].as<float>(), true);
                    mutableCamera->setYaw(angles[1].as<float>(), true);
                    mutableCamera->setRoll(angles[2].as<float>());
                    mWorld->getRenderingManager()->invalidateAtmosphereHistory();
                    break;
                }

                case Diagnostics::CommandType::SetCamera:
                case Diagnostics::CommandType::Cancel:
                    response.mError = Diagnostics::ErrorCode::UnsupportedCommand;
                    break;
            }
        }
        catch (const std::exception& error)
        {
            response.mError = Diagnostics::ErrorCode::InternalFailure;
            response.mPayload = diagnosticBytes(error.what());
        }
        mDiagnosticSession->respond(std::move(response));
    }
}
