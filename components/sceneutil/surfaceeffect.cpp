#include "surfaceeffect.hpp"

#include <utility>

namespace SceneUtil
{
    SurfaceEffect::SurfaceEffect(SurfaceEffectProfile profile)
        : mProfile(std::move(profile))
    {
        setName("surfaceEffect");
    }

    SurfaceEffect::SurfaceEffect(const SurfaceEffect& copy, const osg::CopyOp& copyOp)
        : osg::Object(copy, copyOp)
        , mProfile(copy.mProfile)
    {
    }

}
