#pragma once

#include "wig/HeadwearEquipment.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Hide Helmets: the helmets and hoods Tailor took off people outside Adventuring and combat, so the
// same copies go back on. Unlike weapons they come off (a hidden helmet would leave a bald head), so
// the game saves the change and the co-save keeps this ledger.
class SituationHelmets
{
public:
    static SituationHelmets& GetSingleton();

    // `hidden`: takes the actor's helmets and hoods off and puts their wig back on. Otherwise puts back
    // what it took off them, the wig stepping aside first. `look` is the outfit the actor wears now (0 for
    // none, and for the player's own gear): a copy goes back only while the look it came off is worn.
    void Update(RE::Actor* actor, bool hidden, int look);
    // Everyone holding pieces Tailor took off, so the poll reaches people it no longer tracks.
    [[nodiscard]] std::vector<RE::FormID> Actors() const;
    // The armor Hide Helmets has off this actor now, copies still there and off: headgear that is off on
    // purpose, so the wig may show.
    [[nodiscard]] std::vector<RE::TESObjectARMO*> TakenPieces(RE::Actor* actor) const;
    // Every copy the ledger holds for this actor, whatever became of it: the player's Own Gear keeps those
    // still there.
    [[nodiscard]] std::vector<Tailor::Wigs::HeadwearCopy> Copies(RE::FormID actorId) const;
    // Drops this actor's copies without putting them back: they are dead.
    void Forget(RE::FormID actorId);

    // The co-save. `Load` replaces the ledger, resolving form IDs for this load order and dropping what
    // no longer resolves; `Clear` empties it before any load and on a new game.
    [[nodiscard]] std::string Save() const;
    void Load(std::string_view payload, const SKSE::SerializationInterface* serialization);
    void Clear();

private:
    SituationHelmets() = default;
    // Whether `form` is a wig in Tailor's library. The set is rebuilt only when the library has changed
    // since, never on every poll.
    [[nodiscard]] bool IsLibraryWig(RE::FormID form);

    // One person's copies, and the look they came off.
    struct Taken
    {
        int look = 0;
        std::vector<Tailor::Wigs::HeadwearCopy> copies;
    };
    std::unordered_map<RE::FormID, Taken> _taken;
    // The library's wig forms as of WigLibrary revision `_wigRevision`; built outside the lock.
    std::unordered_set<RE::FormID> _libraryWigs;
    std::optional<std::uint64_t> _wigRevision;
    // WigManager's and PlayerWardrobe's mutexes come first: the wig equip asks TakenPieces, and the
    // wardrobe's Own Gear asks Copies. Nothing here calls WigManager, PlayerWardrobe or the inventory while
    // holding this one.
    mutable std::mutex _mutex;
};
