#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICCTL_REPLAYSTATE_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICCTL_REPLAYSTATE_H

#include <string_view>

namespace DiagnosticCtl
{
    /// Validate every recorded atmosphere field, allowing only the versioned addition of regional color history.
    void validateAtmosphereRuntimeParity(std::string_view expected, std::string_view actual);
}

#endif
