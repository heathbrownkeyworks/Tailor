#pragma once

namespace Tailor::Player
{
    // The game rebuilds an NPC's armor models after Tailor's immediate equips, but not the
    // player's while Tailor is open: the inventory says the pieces are worn while the body
    // still shows the old ones, or nothing. Call after every change to what the player wears.
    void RefreshPlayerModel(RE::Actor* player);
}
