#include "optimizeroptions.hpp"

#include "optimizer.hpp"

#include <array>
#include <string>

namespace SceneUtil
{
    unsigned parseOptimizationOptions(std::string_view control, unsigned defaults)
    {
        unsigned options = defaults;
        if (control.find("OFF") != std::string_view::npos || control.find('0') != std::string_view::npos)
            options = 0;

        struct NamedOption
        {
            std::string_view mName;
            unsigned mValue;
        };
        constexpr std::array namedOptions{
            NamedOption{ "FLATTEN_STATIC_TRANSFORMS", Optimizer::FLATTEN_STATIC_TRANSFORMS },
            NamedOption{ "REMOVE_REDUNDANT_NODES", Optimizer::REMOVE_REDUNDANT_NODES },
            NamedOption{ "MERGE_GEOMETRY", Optimizer::MERGE_GEOMETRY },
            NamedOption{ "VERTEX_POSTTRANSFORM", Optimizer::VERTEX_POSTTRANSFORM },
            NamedOption{ "VERTEX_PRETRANSFORM", Optimizer::VERTEX_PRETRANSFORM },
        };
        for (const NamedOption& option : namedOptions)
        {
            if (control.find("~" + std::string(option.mName)) != std::string_view::npos)
                options &= ~option.mValue;
            else if (control.find(option.mName) != std::string_view::npos)
                options |= option.mValue;
        }
        return options;
    }
}
