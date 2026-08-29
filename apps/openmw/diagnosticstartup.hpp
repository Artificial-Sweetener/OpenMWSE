#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICSTARTUP_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICSTARTUP_H

#include <optional>
#include <string>
#include <string_view>

namespace OMW
{
    /// Return whether this build starts the current-user diagnostic bridge without a launch override.
    bool diagnosticsEnabledByDefault();

    /// Resolve the diagnostic endpoint from the command line, environment override, and build policy.
    std::string resolveDiagnosticEndpoint(
        std::string_view requestedEndpoint, std::optional<std::string_view> environmentOverride, bool enabledByDefault);
}

#endif
