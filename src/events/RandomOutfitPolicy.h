#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace Tailor::Situations
{
    inline bool IsRandomOutfitCurrent(int outfitId, float selectedDay, float currentDay,
        const std::vector<int>& eligibleOutfits)
    {
        return outfitId > 0 && std::floor(selectedDay) == std::floor(currentDay) &&
            std::find(eligibleOutfits.begin(), eligibleOutfits.end(), outfitId) != eligibleOutfits.end();
    }
}
