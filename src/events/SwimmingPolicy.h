#pragma once

#include <cmath>

namespace Tailor::Situations
{
    inline bool IsInWater(float waterHeight, float actorZ, bool isSwimming)
    {
        // Keep the user's 40-unit threshold; the engine state can also detect
        // swimming when the water height is unavailable (reported as -infinity).
        return isSwimming || (std::isfinite(waterHeight) && std::isfinite(actorZ) && waterHeight > actorZ - 40.0f);
    }
}
