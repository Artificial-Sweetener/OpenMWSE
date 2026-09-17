#include "openmwsewater.hpp"

#include <stdexcept>

namespace Settings
{
    WaterAppearance OpenMWSEWaterCategory::appearance() const
    {
        const std::string& value = mAppearance.get();
        if (value == "openmw")
            return WaterAppearance::OpenMW;
        if (value == "mge-xe")
            return WaterAppearance::MgeXe;
        if (value == "enhanced")
            return WaterAppearance::Enhanced;
        if (value == "vanilla")
            return WaterAppearance::Vanilla;
        throw std::logic_error("validated water appearance is not recognized");
    }

    EnhancedWaterColor OpenMWSEWaterCategory::enhancedColor() const
    {
        const std::string& value = mEnhancedColor.get();
        if (value == "green-blue")
            return EnhancedWaterColor::GreenBlue;
        if (value == "blue")
            return EnhancedWaterColor::Blue;
        if (value == "green")
            return EnhancedWaterColor::Green;
        throw std::logic_error("validated enhanced water color is not recognized");
    }
}
