#ifndef OPENMW_DIAGNOSTICCTL_ISOLATEDRUN_H
#define OPENMW_DIAGNOSTICCTL_ISOLATEDRUN_H

#include <span>

namespace DiagnosticCtl
{
    /// Run the real engine against copied state, collect evidence, and shut the isolated child down.
    int runIsolated(std::span<char*> arguments);
}

#endif
