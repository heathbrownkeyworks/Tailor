#pragma once

#include "player/PlayerWardrobeState.h"

#include <mutex>
#include <optional>
#include <vector>

namespace Tailor::Player
{
    // Dresses the player through the inventory. Pieces the player owns are equipped
    // as they are; a missing piece is added as a tagged Tailor copy that goes back
    // when the outfit comes off. Every worn copy Tailor touches carries an
    // ExtraUniqueID, so tempered and enchanted copies come back as themselves.
    class PlayerWardrobe
    {
    public:
        static PlayerWardrobe& GetSingleton();

        // Takes off all worn armor (weapons and non-playable body-mod items stay)
        // and puts these pieces on as outfit `outfitId`, recording Own Gear first
        // when no Tailor outfit is on, and the outfit's pieces as saved. False when
        // a piece could not be worn.
        bool Dress(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items, int outfitId,
            std::vector<PieceKey> pieces = {});
        // Takes the outfit off, takes Tailor's copies back and re-equips Own Gear.
        bool RestoreOwnGear(RE::Actor* player);
        // Beast form takes all of the player's gear off and gives nothing back. While their own
        // gear is what they wear, the poll notes it here, tagged as an Own Gear record would be.
        void NoteOwnGear(RE::Actor* player);
        // Turning back: the note becomes the Own Gear record, for the redress to put back on or to
        // keep while an outfit is on. False when there is nothing to adopt.
        bool AdoptOwnGearNote();

        // Makes `wig` the wig Tailor keeps on the player and, when `wear`, puts it on; a wig
        // headgear hides is kept but taken off, its copies staying. The previous kept wig comes
        // off, and goes back when it was Tailor's copy. A wig the player owns is worn as it
        // is; otherwise one tagged Tailor copy is added. False when it could not be worn.
        bool SetWig(RE::Actor* player, RE::TESObjectARMO* wig, bool wear);
        // Takes the kept wig off, and back when it is Tailor's copy.
        void RemoveWig(RE::Actor* player);
        // Default Hair: every worn wig of these forms comes off and every Tailor wig copy goes
        // back; wigs the player owns stay in the inventory. False while one is still worn.
        bool RemoveWigs(RE::Actor* player, const std::vector<std::uint32_t>& wigForms);

        // A preview records the player's state first. EndPreview puts it back
        // (keep = false) or keeps the pieces on as outfit `outfitId`, with its pieces as saved.
        bool BeginPreview(RE::Actor* player);
        bool PreviewItems(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items);
        bool EndPreview(RE::Actor* player, bool keep, int outfitId = 0, std::vector<PieceKey> pieces = {});
        // Before a load: the world reverts, so forget the preview without equipment work.
        void DropPreview();
        bool IsPreviewing() const;

        int WornOutfitId() const;
        WardrobeState State() const;
        void SetState(WardrobeState state);
        void Clear();

        // The copies the last dressing took off, and which of some copies are worn now: the
        // equipment check names a piece that is on again as one that would not come off.
        std::vector<CopyKey> TakenOff() const;
        std::vector<std::uint32_t> WornForms(RE::Actor* player, const std::vector<CopyKey>& copies) const;
        // The forms the last dressing put on, a preview's too: a library wig among them wins over Tailor's.
        std::vector<std::uint32_t> ChosenForms() const;

    private:
        PlayerWardrobe() = default;
        struct Snapshot
        {
            WardrobeState state;
            std::vector<CopyKey> worn;
        };
        bool DressItems(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items);
        void RetireWig(RE::Actor* player, std::uint32_t form);

        mutable std::recursive_mutex _mutex;
        WardrobeState _state;
        std::optional<Snapshot> _preview;
        std::vector<CopyKey> _chosen;  // the copies the last dressing put on
        std::vector<CopyKey> _takenOff;  // the copies the last dressing took off
    };
}
