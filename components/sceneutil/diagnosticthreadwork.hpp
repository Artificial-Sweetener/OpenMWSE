#ifndef OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICTHREADWORK_H
#define OPENMW_COMPONENTS_SCENEUTIL_DIAGNOSTICTHREADWORK_H

#include <cstdint>
#include <optional>

namespace SceneUtil
{
    /// Sample hardware cycles consumed by the current thread when the platform exposes them.
    std::optional<std::uint64_t> diagnosticCurrentThreadCycleCount() noexcept;
}

#endif
