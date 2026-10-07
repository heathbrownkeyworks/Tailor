#pragma once

#include <algorithm>
#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace Tailor::Player
{
    // One inventory copy, identified the way HeadwearPreview does it: the base
    // object and the ExtraUniqueID Tailor gave the copy. A spell has no copy and
    // keeps owner and unique at 0.
    struct CopyKey
    {
        std::uint32_t form = 0;
        std::uint32_t owner = 0;
        std::uint16_t unique = 0;
        friend bool operator==(const CopyKey&, const CopyKey&) = default;
    };

    enum class Hand : std::uint8_t { Right, Left };

    // A weapon, torch or spell an outfit's shield pushed out of a hand.
    struct DisplacedItem
    {
        CopyKey copy;
        Hand hand = Hand::Left;
        friend bool operator==(const DisplacedItem&, const DisplacedItem&) = default;
    };

    // A piece of an outfit as outfits.json keeps it: plugin and local ID, never resolved, so a
    // plugin missing at load leaves it as it was.
    struct PieceKey
    {
        std::string plugin;
        std::uint32_t id = 0;
        friend auto operator<=>(const PieceKey&, const PieceKey&) = default;
    };

    // What Tailor has done to the player's gear. Saved in each save's co-save.
    struct WardrobeState
    {
        // True while one of Tailor's outfits is on: ownGear is what came off.
        bool ownGearRecorded = false;
        std::vector<CopyKey> ownGear;
        // Copies Tailor added; they go back when the outfit comes off.
        std::vector<CopyKey> tailorCopies;
        std::vector<DisplacedItem> displaced;
        // The Tailor outfit on the player; 0 is Own Gear.
        int wornOutfitId = 0;
        // The wig Tailor keeps on the player (0: none). Outfits never take it off and Own
        // Gear never records it: the wig follows the wig rules.
        std::uint32_t keptWig = 0;
        // Wig copies Tailor added; they go back when the wig changes or Default Hair runs.
        std::vector<CopyKey> wigCopies;
        // The player's own gear, noted by the poll while they wear it. Beast form takes all gear
        // off and gives nothing back, so turning back makes this note the Own Gear record.
        std::vector<CopyKey> ownGearNote;
        // The worn outfit's pieces when Tailor dressed the player in it; empty in Own Gear. A load
        // compares them with the outfit, which another save may have edited since.
        std::vector<PieceKey> wornOutfitPieces;
    };

    // Whether an outfit's pieces changed since Tailor dressed the player in it, compared as sets.
    // Without a recorded list (a save from a build that didn't record it) nothing is known to have changed.
    inline bool PiecesChanged(std::vector<PieceKey> recorded, std::vector<PieceKey> current)
    {
        if (recorded.empty()) return false;
        std::ranges::sort(recorded);
        std::ranges::sort(current);
        recorded.erase(std::ranges::unique(recorded).begin(), recorded.end());
        current.erase(std::ranges::unique(current).begin(), current.end());
        return recorded != current;
    }
}
