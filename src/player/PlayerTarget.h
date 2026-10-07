#pragma once

namespace Tailor::Player
{
    // Werewolf, Vampire Lord and modded transformation races are not playable races.
    inline bool InBeastForm(RE::Actor* player)
    {
        auto* race = player ? player->GetRace() : nullptr;
        return race && !race->data.flags.all(RE::RACE_DATA::Flag::kPlayable);
    }

    // Tailor dresses the player only while they are loaded, alive and in a playable race.
    inline bool CanDress(RE::Actor* player)
    {
        return player && player->IsPlayerRef() && !player->IsDead() && player->Is3DLoaded() && !InBeastForm(player);
    }
}
