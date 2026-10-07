#pragma once

#include <cstdint>

namespace Tailor::Player
{
    // The player's placed reference (PlayerRef in Skyrim.esm). Every character
    // shares it, so the player's assignments live in each save's co-save.
    inline constexpr std::uint32_t kPlayerRef = 0x14;
}
