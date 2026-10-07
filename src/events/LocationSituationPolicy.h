#pragma once

namespace Tailor::Situations
{
    // The situation by location. Home is the player's house, and for NPCs any house; otherwise a city,
    // town, settlement, dwelling or inn is Town (so a house in town is Town for the player), and the rest
    // is Adventuring.
    template<class Situation>
    [[nodiscard]] constexpr Situation LocationSituation(bool hasLocation, bool isPlayer, bool playerHouse, bool house,
        bool town) noexcept
    {
        if (!hasLocation) return Situation::Adventuring;
        if (playerHouse || (house && !isPlayer)) return Situation::Home;
        return town ? Situation::Town : Situation::Adventuring;
    }
}
