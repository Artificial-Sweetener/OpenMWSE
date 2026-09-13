#ifndef OPENMW_MWRENDER_GROUNDCOVERDISTANCE_H
#define OPENMW_MWRENDER_GROUNDCOVERDISTANCE_H

#include "groundcoverlod.hpp"

#include <osg/StateSet>
#include <osg/Uniform>

namespace MWRender
{
    /// Share world-unit distance and GPU fading across existing and newly created grass chunks.
    class GroundcoverDistance final : public osg::Referenced
    {
    public:
        /// Validate the distance and create the fade uniform used by every groundcover pass.
        explicit GroundcoverDistance(float distance)
            : mFade(new osg::Uniform("groundcoverFade", osg::Vec2f()))
        {
            // Changes happen with rendering stopped, so this needs no per-frame draw/update barrier.
            mFade->setDataVariance(osg::Object::STATIC);
            set(distance);
        }

        /// Update while render threads and terrain preloading are quiescent; retain all instance geometry.
        void set(float distance)
        {
            mFarLodDistance = getGroundcoverFarLodDistance(distance);
            mDistance = distance;
            mFade->set(osg::Vec2f(distance * 0.9f, distance));
        }

        /// Return the current world-unit visibility limit.
        float get() const { return mDistance; }
        /// Return the current distance at which reduced geometry may be selected.
        float getFarLodDistance() const { return mFarLodDistance; }
        /// Attach the shared live fade interval to a legacy or GPU material state.
        void attachFadeUniform(osg::StateSet& stateSet) const { stateSet.addUniform(mFade); }

    private:
        float mDistance = 0;
        float mFarLodDistance = 0;
        osg::ref_ptr<osg::Uniform> mFade;
    };
}

#endif
