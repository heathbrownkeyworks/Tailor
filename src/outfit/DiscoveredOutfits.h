// Part of Tailor (GPL-3.0-or-later).
//
// Auto-discovered outfit sets, porting Fitting Room's AutoPresets pipeline
// (scan -> SetDetector cluster -> build) into Tailor.
//
// Design notes:
//   - IN-MEMORY ONLY. Discovered sets are regenerated on every game load and
//     never written to outfits.json, so a changed load order can never leave
//     stale FormIDs behind (the same rule Fitting Room follows).
//   - READ-ONLY. The UI shows them in their own "Discovered" section; they
//     cannot be edited, deleted, or assigned. (Copy-to-custom-outfit is a
//     possible later step.)
//   - NEGATIVE IDS. Discovered outfits carry ids -1, -2, ... so they can
//     never collide with OutfitStore ids (positive ints from 1 up). The UI
//     model carries them unchanged; tailorPreviewDiscovered looks them up
//     here, never in OutfitStore.
//   - SEX TAGGING, not player-fit gating. Fitting Room dropped sets whose
//     pieces did not render on the PLAYER's body. Tailor dresses NPCs of
//     either sex, so each set is tagged Male/Female/Unisex from its pieces'
//     armor-addon models and Tailor's existing OutfitFits() gate decides at
//     preview time.
#pragma once

#include "outfit/CustomOutfit.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace Tailor::Discovered {

    class DiscoveredOutfits {
    public:
        static DiscoveredOutfits& GetSingleton();

        // Rebuild from the current load order: scan armors, cluster into sets
        // with SetDetector, complete from the game's outfit records, tag sex.
        // MAIN THREAD ONLY (reads game data). Called from the kPostLoadGame /
        // kNewGame task in main.cpp, and by the UI's Rescan button.
        void Regenerate();

        // Thread-safe snapshot for the UI model layer.
        std::vector<CustomOutfit> Snapshot() const;

        // Lookup by the negative id the UI model carries. Thread-safe copy.
        std::optional<CustomOutfit> GetById(int id) const;

        std::size_t Count() const;

    private:
        DiscoveredOutfits() = default;
        DiscoveredOutfits(const DiscoveredOutfits&) = delete;
        DiscoveredOutfits& operator=(const DiscoveredOutfits&) = delete;

        std::vector<CustomOutfit> _outfits;
        mutable std::mutex        _mutex;
    };

}  // namespace Tailor::Discovered
