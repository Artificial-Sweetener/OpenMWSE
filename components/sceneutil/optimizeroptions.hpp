#ifndef OPENMW_COMPONENTS_SCENEUTIL_OPTIMIZEROPTIONS_H
#define OPENMW_COMPONENTS_SCENEUTIL_OPTIMIZEROPTIONS_H

#include <string_view>

namespace SceneUtil
{
    /// Apply developer optimizer controls to an existing option mask.
    unsigned parseOptimizationOptions(std::string_view control, unsigned defaults);
}

#endif
