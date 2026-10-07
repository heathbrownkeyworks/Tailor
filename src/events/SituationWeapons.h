#pragma once

#include "preview/VisibilityLedger.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Weapons, shields, quivers and torches hidden while their owner's Sleep or Swimming look is on, and
// with Hide Weapons all but the torch outside Adventuring and combat; for the player and NPCs alike.
// Only the models hide, through each actor's visibility ledger: nothing is unequipped, and showing
// puts back exactly the visibility the ledger changed.
class SituationWeapons
{
public:
    // What to hide: nothing, Hide Weapons' set (no torch), or the Sleep and Swimming set (with the torch).
    enum class Set { None, WithoutTorch, WithTorch };

    static SituationWeapons& GetSingleton();

    // Hides the actor's `set` models, hiding them again after a model rebuild; `Set::None` shows what
    // was hidden on them. A change of set shows the old set's models before hiding the new one.
    void Update(RE::Actor* actor, Set set);
    // A preview starting on this actor shows their real look, weapons included.
    void Show(RE::FormID actorId);
    // Anyone not just updated (unloaded, dead, or no longer with situations) gets their weapons back.
    void ShowAllExcept(const std::unordered_set<RE::FormID>& updated);
    // A game load: every ledger puts back what it hid and is forgotten.
    void ShowAll();

private:
    using Ledger = Tailor::Preview::VisibilityLedger<RE::NiPointer<RE::NiAVObject>>;

    SituationWeapons() = default;
    struct Hidden
    {
        Ledger ledger;
        Set set = Set::None;
    };
    void Hide(RE::Actor* actor, Hidden& hidden);
    bool ShowLocked(RE::FormID actorId);

    // Every caller runs on the game thread (the poll, a preview's Begin, the load reset); the lock
    // only keeps the map consistent.
    std::mutex _mutex;
    std::unordered_map<RE::FormID, Hidden> _ledgers;
};
