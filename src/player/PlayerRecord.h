#pragma once

#include "outfit/OutfitAssignments.h"
#include "player/PlayerSituationPolicy.h"
#include "player/PlayerWardrobeState.h"
#include "wig/WigAssignments.h"

#include <optional>

namespace Tailor::Player
{
    // Everything Tailor saves about the player in one save's co-save.
    struct PlayerRecord
    {
        std::optional<SituationalAssignment> outfits;
        WardrobeState wardrobe;
        PlayerWigRow wigs;
        // Where the player woke up in their Sleep outfit, while that still holds.
        PlayerWakeUp wakeUp;
    };

    // Forms are written as plugin + local ID, so a load-order change keeps them.
    nlohmann::json ToJson(const PlayerRecord& record);
    // Inventory entries whose plugin is gone are dropped, as the game drops those items; wig choices
    // stay dormant until the plugin returns; everything else loads.
    PlayerRecord FromJson(const nlohmann::json& json);
}
