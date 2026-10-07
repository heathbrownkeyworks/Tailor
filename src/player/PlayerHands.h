#pragma once

#include "player/PlayerWardrobeState.h"

#include <vector>

namespace Tailor::Player
{
    // An outfit shield takes the left hand and, with a two-handed weapon or bow,
    // the right hand too. Call before the shield goes on: records (and tags) what
    // it is about to push aside.
    std::vector<DisplacedItem> TakeHandsForShield(RE::Actor* player);
    // Puts displaced items back into their hands. Returns the ones that must wait
    // because a shield is still on. An item whose hand now holds something else,
    // or that the player no longer has, is dropped.
    std::vector<DisplacedItem> ReturnHands(RE::Actor* player, const std::vector<DisplacedItem>& items);
}
