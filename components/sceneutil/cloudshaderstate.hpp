#ifndef OPENMW_COMPONENTS_SCENEUTIL_CLOUDSHADERSTATE_H
#define OPENMW_COMPONENTS_SCENEUTIL_CLOUDSHADERSTATE_H

/// @file Publish cloud shader specialization with the frame's optical weather state.

#include <cmath>

#include <osg/StateSet>
#include <osg/Vec4f>

#include <components/shader/cloudprogram.hpp>

namespace SceneUtil
{
    /// Select the steady variant only when the incoming episode has exactly zero crossfade weight.
    inline void updateCloudShaderState(osg::StateSet& state, const osg::Vec4f& scenario)
    {
        const bool steady = std::isfinite(scenario.z()) && std::isfinite(scenario.w())
            && (scenario.z() <= 0.f || scenario.w() < 0.5f);
        state.setDefine(Shader::sSteadyCloudDefine, steady ? "1" : "0", osg::StateAttribute::ON);
    }
}

#endif
