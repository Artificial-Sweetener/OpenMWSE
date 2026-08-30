#include "diagnosticthreadwork.hpp"

#ifdef _WIN32
#include <components/misc/windows.hpp>
#endif

namespace SceneUtil
{
    std::optional<std::uint64_t> diagnosticCurrentThreadCycleCount() noexcept
    {
#ifdef _WIN32
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0)
            return static_cast<std::uint64_t>(cycles);
#endif
        return std::nullopt;
    }
}
