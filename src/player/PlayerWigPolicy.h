#pragma once

#include <algorithm>
#include <vector>

namespace Tailor::Player
{
    enum class PlayerWigAction { Skip, LeaveOff, PutBack, TakeOff };

    // What happens after armor on the player changes. Tailor's own screens and a player who
    // can't be dressed come first. A wig the player got rid of in a menu (dropped, sold,
    // stored or given away) stays off even under headgear. Otherwise headgear wins: the wig
    // comes off under it and waits. A wig the player only took off in an inventory menu
    // stays off until Tailor next changes it; anything else that took it off, a script or
    // another mod, gets it put back.
    [[nodiscard]] constexpr PlayerWigAction DecidePlayerWig(bool sameAssignment, bool dressable, bool tailorOpen,
        bool worn, bool hiddenByHeadgear, bool takenOffInMenu, bool inInventory) noexcept
    {
        if (!sameAssignment || !dressable || tailorOpen) return PlayerWigAction::Skip;
        if (takenOffInMenu && !inInventory) return PlayerWigAction::LeaveOff;
        if (hiddenByHeadgear) return worn ? PlayerWigAction::TakeOff : PlayerWigAction::Skip;
        if (worn) return PlayerWigAction::Skip;
        return takenOffInMenu ? PlayerWigAction::LeaveOff : PlayerWigAction::PutBack;
    }

    struct PlayerWigEvent
    {
        bool queue = false;           // check the wig once the event stack is left
        bool resume = false;          // the player's choice to leave the wig off ends
        bool takenOffInMenu = false;  // the change is the player's own choice
        bool leaveOff = false;        // Tailor's wig stays off now, even while it waits under headgear
        bool operator==(const PlayerWigEvent&) const = default;
    };

    // What an armor change on the player means, judged while the event fires. A wig the player
    // left off stays off until they put it back on themselves; nothing else is looked at.
    // Otherwise every change is checked. In an inventory menu, taking the wig off or getting
    // rid of it is the player's choice, and so is putting on another wig of Tailor's library,
    // which takes it off; the engine may send those two events in either order. That last choice
    // is settled here, since the later check can't tell it from headgear while Tailor's wig waits
    // under a helmet: a helmet equipped in the menu knocks the wig off with the same unequip.
    [[nodiscard]] constexpr PlayerWigEvent DecidePlayerWigEvent(bool leftOff, bool equipped, bool assignedWig,
        bool otherLibraryWig, bool inMenu) noexcept
    {
        if (leftOff) {
            const bool resume = equipped && assignedWig;
            return {resume, resume, false, false};
        }
        return {true, false, inMenu && (equipped ? otherLibraryWig : assignedWig), inMenu && equipped && otherLibraryWig};
    }

    // The library wigs that don't count as headgear over Tailor's wig on the player: all of them but
    // the pieces of the outfit the player wears, whose wig wins, as NPCs keep outfit wigs.
    template <class Armor>
    [[nodiscard]] std::vector<Armor*> WigsNotHeadgear(const std::vector<Armor*>& library, const std::vector<Armor*>& outfitPieces)
    {
        std::vector<Armor*> wigs;
        for (auto* wig : library) {
            if (std::ranges::find(outfitPieces, wig) == outfitPieces.end()) wigs.push_back(wig);
        }
        return wigs;
    }
}
