#include "ripples.hpp"

#include <osg/Geometry>
#include <osg/Texture2D>
#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>

#include "../mwworld/ptr.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        // Enhanced Water Shader 2.2 sewer outlets by Krokantor, Hrnchamd, vtastek, phal, harnlarnm, and abot.
        // The original shader grants reuse with attribution; OpenMWSE drives its bounded ripple simulation instead of
        // evaluating all 70 radial waves in every water fragment.
        const std::array<osg::Vec2f, 70> sewerOutlets = { {
            { 19160, -86825 },
            { 46500, -81930 },
            { 31475, -104345 },
            { 32165, -76700 },
            { 19160, -81900 },
            { 20260, -80775 },
            { 20300, -87915 },
            { 24125, -80775 },
            { 24225, -87915 },
            { 25275, -86700 },
            { 25275, -81850 },
            { 25550, -95230 },
            { 25550, -91390 },
            { 25550, -88840 },
            { 25550, -84940 },
            { 26300, -81750 },
            { 26300, -77825 },
            { 26630, -89955 },
            { 26650, -96365 },
            { 26700, -83850 },
            { 27185, -90260 },
            { 27625, -82800 },
            { 27635, -76700 },
            { 31180, -96925 },
            { 31200, -103770 },
            { 31540, -83850 },
            { 31615, -96365 },
            { 31765, -90260 },
            { 31895, -89955 },
            { 32150, -82800 },
            { 32675, -95020 },
            { 32675, -91785 },
            { 32675, -88690 },
            { 32675, -85300 },
            { 32975, -95365 },
            { 32975, -91190 },
            { 32975, -89110 },
            { 32975, -84720 },
            { 33450, -81635 },
            { 33450, -77945 },
            { 34095, -96365 },
            { 34120, -90260 },
            { 34175, -104345 },
            { 34480, -83850 },
            { 34585, -89955 },
            { 34640, -96925 },
            { 34695, -103770 },
            { 38500, -89955 },
            { 38965, -96365 },
            { 38980, -83850 },
            { 39060, -90260 },
            { 40100, -95300 },
            { 40100, -91395 },
            { 40100, -88790 },
            { 40100, -84900 },
            { 40400, -86900 },
            { 40400, -81955 },
            { 41350, -87915 },
            { 41475, -80775 },
            { 45300, -87915 },
            { 45480, -80775 },
            { 46500, -86790 },
            { 107090, -63820 },
            { 114215, -59905 },
            { 113110, -64890 },
            { 107090, -59875 },
            { 108085, -58800 },
            { 108110, -64890 },
            { 113170, -58800 },
            { 114215, -63780 },
        } };
    }

    RipplesSurface::RipplesSurface(Resource::ResourceSystem* resourceSystem)
        : osg::Geometry()
        , mResourceSystem(resourceSystem)
    {
        setUseDisplayList(false);
        setUseVertexBufferObjects(true);

        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3f(-1, -1, 0));
        verts->push_back(osg::Vec3f(-1, 3, 0));
        verts->push_back(osg::Vec3f(3, -1, 0));

        setVertexArray(verts);

        setCullingActive(false);

        addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3));

#ifdef __APPLE__
        // we can not trust Apple :)
        mUseCompute = false;
#else
        constexpr float minimumGLVersionRequiredForCompute = 4.4f;
        osg::GLExtensions& exts = SceneUtil::getGLExtensions();
        mUseCompute = exts.glVersion >= minimumGLVersionRequiredForCompute
            && exts.glslLanguageVersion >= minimumGLVersionRequiredForCompute;
#endif

        for (size_t i = 0; i < mState.size(); ++i)
        {
            osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
            // bindings are set in the compute shader
            if (!mUseCompute)
                stateset->addUniform(new osg::Uniform("imageIn", 0));

            stateset->addUniform(new osg::Uniform("offset", osg::Vec2f()));
            stateset->addUniform(new osg::Uniform("positionCount", 0));
            stateset->addUniform(new osg::Uniform(osg::Uniform::Type::FLOAT_VEC3, "positions", 100));
            stateset->setAttributeAndModes(new osg::Viewport(0, 0, RipplesSurface::sRTTSize, RipplesSurface::sRTTSize));
            mState[i].mStateset = stateset;
        }

        for (size_t i = 0; i < mTextures.size(); ++i)
        {
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
            texture->setSourceFormat(GL_RGBA);
            texture->setInternalFormat(GL_RGBA16F);
            texture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);
            texture->setBorderColor(osg::Vec4(0, 0, 0, 0));
            texture->setTextureSize(sRTTSize, sRTTSize);

            mTextures[i] = texture;

            mFBOs[i] = new osg::FrameBufferObject;
            mFBOs[i]->setAttachment(osg::Camera::COLOR_BUFFER0, osg::FrameBufferAttachment(mTextures[i]));
        }

        if (mUseCompute)
            setupComputePipeline();
        else
            setupFragmentPipeline();

        if (mProgramBlobber != nullptr)
        {
            [[maybe_unused]] static const bool pipelineLogged = [&] {
                if (mUseCompute)
                    Log(Debug::Info) << "Initialized compute shader pipeline for water ripples";
                else
                    Log(Debug::Info) << "Initialized fallback fragment shader pipeline for water ripples";
                return true;
            }();
        }

        setCullCallback(new osg::NodeCallback);
        setUpdateCallback(new osg::NodeCallback);
    }

    void RipplesSurface::setupFragmentPipeline()
    {
        auto& shaderManager = mResourceSystem->getSceneManager()->getShaderManager();

        Shader::ShaderManager::DefineMap defineMap = { { "rippleMapSize", std::to_string(sRTTSize) + ".0" } };

        osg::ref_ptr<osg::Shader> vertex = shaderManager.getShader("fullscreen_tri.vert", {}, osg::Shader::VERTEX);
        osg::ref_ptr<osg::Shader> blobber
            = shaderManager.getShader("ripples_blobber.frag", defineMap, osg::Shader::FRAGMENT);
        osg::ref_ptr<osg::Shader> simulate
            = shaderManager.getShader("ripples_simulate.frag", defineMap, osg::Shader::FRAGMENT);
        if (vertex == nullptr || blobber == nullptr || simulate == nullptr)
        {
            Log(Debug::Error) << "Failed to load shaders required for fragment shader ripple pipeline";
            return;
        }

        mProgramBlobber = shaderManager.getProgram(vertex, std::move(blobber));
        mProgramSimulation = shaderManager.getProgram(std::move(vertex), std::move(simulate));
    }

    void RipplesSurface::setupComputePipeline()
    {
        auto& shaderManager = mResourceSystem->getSceneManager()->getShaderManager();

        osg::ref_ptr<osg::Shader> blobber
            = shaderManager.getShader("core/ripples_blobber.comp", {}, osg::Shader::COMPUTE);
        osg::ref_ptr<osg::Shader> simulate
            = shaderManager.getShader("core/ripples_simulate.comp", {}, osg::Shader::COMPUTE);
        if (blobber == nullptr || simulate == nullptr)
        {
            Log(Debug::Error) << "Failed to load shaders required for compute shader ripple pipeline";
            return;
        }

        mProgramBlobber = shaderManager.getProgram(nullptr, std::move(blobber));
        mProgramSimulation = shaderManager.getProgram(nullptr, std::move(simulate));
    }

    void RipplesSurface::updateState(const osg::FrameStamp& frameStamp, State& state)
    {
        state.mPaused = mPaused;

        if (mPaused)
            return;

        constexpr double updateFrequency = 60.0;
        constexpr double updatePeriod = 1.0 / updateFrequency;

        const double simulationTime = frameStamp.getSimulationTime();
        const double frameDuration = simulationTime - mLastSimulationTime;
        mLastSimulationTime = simulationTime;

        mRemainingWaveTime += frameDuration;
        const double ticks = std::floor(mRemainingWaveTime * updateFrequency);
        mRemainingWaveTime -= ticks * updatePeriod;

        if (ticks == 0)
        {
            state.mPaused = true;
            return;
        }

        const MWWorld::Ptr player = MWMechanics::getPlayer();
        const ESM::Position& playerPos = player.getRefData().getPosition();

        mCurrentPlayerPos = osg::Vec2f(
            std::floor(playerPos.pos[0] / sWorldScaleFactor), std::floor(playerPos.pos[1] / sWorldScaleFactor));
        const osg::Vec2f offset = mCurrentPlayerPos - mLastPlayerPos;
        mLastPlayerPos = mCurrentPlayerPos;

        state.mHasPositions = mPositionCount != 0;
        state.mStateset->getUniform("positionCount")->set(static_cast<int>(mPositionCount));
        state.mStateset->getUniform("offset")->set(offset);

        osg::Uniform* const positions = state.mStateset->getUniform("positions");

        for (std::size_t i = 0; i < mPositionCount; ++i)
        {
            osg::Vec3f pos = mPositions[i]
                - osg::Vec3f(mCurrentPlayerPos.x() * sWorldScaleFactor, mCurrentPlayerPos.y() * sWorldScaleFactor, 0.0)
                + osg::Vec3f(sRTTSize * sWorldScaleFactor / 2, sRTTSize * sWorldScaleFactor / 2, 0.0);
            pos /= sWorldScaleFactor;
            positions->setElement(static_cast<unsigned>(i), pos);
        }
        positions->dirty();

        mPositionCount = 0;
    }

    void RipplesSurface::traverse(osg::NodeVisitor& nv)
    {
        const osg::FrameStamp* const frameStamp = nv.getFrameStamp();

        if (frameStamp == nullptr)
            return;

        if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
            updateState(*frameStamp, mState[frameStamp->getFrameNumber() % 2]);

        osg::Geometry::traverse(nv);
    }

    void RipplesSurface::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        if (mProgramBlobber == nullptr || mProgramSimulation == nullptr)
            return;

        osg::State& state = *renderInfo.getState();
        const std::size_t currentFrame = state.getFrameStamp()->getFrameNumber() % 2;
        const State& frameState = mState[currentFrame];
        const unsigned int contextID = state.getContextID();
        Activity& activity = mActivity[contextID];
        const bool skipDormant = Settings::optimizations().useSkipDormantRipples();
        if (!skipDormant)
            activity = Activity::Active;
        else
        {
            if (activity == Activity::Empty || !mTextures[0]->getTextureObject(contextID))
            {
                mFBOs[0]->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                glClearColor(0.f, 0.f, 0.f, 0.f);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                state.haveAppliedAttribute(osg::StateAttribute::COLORMASK);
                state.applyMode(GL_SCISSOR_TEST, false);
                glClear(GL_COLOR_BUFFER_BIT);
                activity = Activity::Idle;
            }
            if (frameState.mHasPositions)
                activity = Activity::Active;
            if (activity == Activity::Idle)
                return;
        }
        if (frameState.mPaused)
        {
            return;
        }

        osg::GLExtensions& ext = *state.get<osg::GLExtensions>();
        const auto bindImage = [&](osg::Texture2D* texture, GLuint index, GLenum access) {
            osg::Texture::TextureObject* to = texture->getTextureObject(contextID);
            if (!to || texture->isDirty(contextID))
            {
                state.applyTextureAttribute(index, texture);
                to = texture->getTextureObject(contextID);
            }
            ext.glBindImageTexture(index, to->id(), 0, GL_FALSE, 0, access, GL_RGBA16F);
        };

        // PASS: Blot in all ripple spawners
        state.pushStateSet(frameState.mStateset);
        state.apply();
        state.applyAttribute(mProgramBlobber);
        for (const auto& [name, stack] : state.getUniformMap())
        {
            if (!stack.uniformVec.empty())
                state.getLastAppliedProgramObject()->apply(*(stack.uniformVec.back().first));
        }

        if (mUseCompute)
        {
            bindImage(mTextures[1], 0, GL_WRITE_ONLY_ARB);
            bindImage(mTextures[0], 1, GL_READ_ONLY_ARB);

            ext.glDispatchCompute(sRTTSize / 16, sRTTSize / 16, 1);
            ext.glMemoryBarrier(GL_ALL_BARRIER_BITS);
        }
        else
        {
            mFBOs[1]->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            state.applyTextureAttribute(0, mTextures[0]);
            osg::Geometry::drawImplementation(renderInfo);
        }

        // PASS: Wave simulation
        state.applyAttribute(mProgramSimulation);
        for (const auto& [name, stack] : state.getUniformMap())
        {
            if (!stack.uniformVec.empty())
                state.getLastAppliedProgramObject()->apply(*(stack.uniformVec.back().first));
        }

        if (mUseCompute)
        {
            bindImage(mTextures[0], 0, GL_WRITE_ONLY_ARB);
            bindImage(mTextures[1], 1, GL_READ_ONLY_ARB);

            ext.glDispatchCompute(sRTTSize / 16, sRTTSize / 16, 1);
            ext.glMemoryBarrier(GL_ALL_BARRIER_BITS);
        }
        else
        {
            mFBOs[0]->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
            state.applyTextureAttribute(0, mTextures[1]);
            osg::Geometry::drawImplementation(renderInfo);
        }

        state.popStateSet();
    }

    osg::Texture* RipplesSurface::getColorTexture() const
    {
        return mTextures[0];
    }

    void RipplesSurface::emit(const osg::Vec3f pos, float sizeInCellUnits)
    {
        // Emitted positions are reset every frame, don't bother wrapping around when out of buffer space
        if (mPositionCount >= mPositions.size())
        {
            return;
        }

        mPositions[mPositionCount] = osg::Vec3f(pos.x(), pos.y(), sizeInCellUnits);

        mPositionCount++;
    }

    void RipplesSurface::releaseGLObjects(osg::State* state) const
    {
        if (state != nullptr)
            mActivity[state->getContextID()] = Activity::Empty;
        else
            mActivity.setAllElementsTo(Activity::Empty);
        for (const auto& tex : mTextures)
            tex->releaseGLObjects(state);
        for (const auto& fbo : mFBOs)
            fbo->releaseGLObjects(state);

        if (mProgramBlobber)
            mProgramBlobber->releaseGLObjects(state);
        if (mProgramSimulation)
            mProgramSimulation->releaseGLObjects(state);
    }

    Ripples::Ripples(Resource::ResourceSystem* resourceSystem)
        : osg::Camera()
        , mRipples(new RipplesSurface(resourceSystem))
    {
        setName(std::string(SceneUtil::ripplesCameraName));
        getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        setRenderOrder(osg::Camera::PRE_RENDER);
        setReferenceFrame(osg::Camera::ABSOLUTE_RF);
        setNodeMask(Mask_RenderToTexture);
        setClearMask(GL_NONE);
        setViewport(0, 0, RipplesSurface::sRTTSize, RipplesSurface::sRTTSize);
        addChild(mRipples);
        setCullingActive(false);
        setImplicitBufferAttachmentMask(0, 0);
    }

    osg::Texture* Ripples::getColorTexture() const
    {
        return mRipples->getColorTexture();
    }

    void Ripples::emit(const osg::Vec3f pos, float sizeInCellUnits)
    {
        mRipples->emit(pos, sizeInCellUnits);
    }

    void Ripples::emitNearbySewerRipples()
    {
        const ESM::Position& position = MWMechanics::getPlayer().getRefData().getPosition();
        const osg::Vec2f playerPosition(position.pos[0], position.pos[1]);
        constexpr float activeRadius = RipplesSurface::sRTTSize * RipplesSurface::sWorldScaleFactor * 0.45f;
        constexpr float activeRadiusSquared = activeRadius * activeRadius;
        for (const osg::Vec2f& outlet : sewerOutlets)
        {
            if ((outlet - playerPosition).length2() <= activeRadiusSquared)
                emit(osg::Vec3f(outlet.x(), outlet.y(), 0.f), 18.f);
        }
    }

    void Ripples::setPaused(bool paused)
    {
        mRipples->setPaused(paused);
    }
}
