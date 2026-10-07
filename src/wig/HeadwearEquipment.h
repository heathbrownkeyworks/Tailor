#pragma once

#include "events/HideSettingsPolicy.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace Tailor::Wigs
{
    // ARMO controls equipment conflicts; compatible ARMA head slots also tell
    // us when two otherwise wearable items would hide each other's hair model.
    bool HeadwearConflicts(RE::Actor* actor, RE::TESObjectARMO* wig, RE::TESObjectARMO* armor);
    // `notHeadgear` names items that never hide the wig, worn or in an NPC's outfit: the player's own
    // library wigs, which Tailor's wig takes the place of, and helmets Hide Helmets took off.
    bool OutfitHidesWig(RE::Actor* actor, RE::TESObjectARMO* wig, RE::TESObjectARMO* previousWig = nullptr,
        const std::vector<RE::TESObjectARMO*>& notHeadgear = {});
    bool SuspendWig(RE::Actor* actor, RE::TESObjectARMO* wig);

    // One inventory copy: its armor and the owner and number of its ExtraUniqueID.
    struct HeadwearCopy
    {
        RE::FormID form;
        RE::FormID owner;
        std::uint16_t unique;
        bool operator==(const HeadwearCopy&) const = default;
    };

    // Hide Helmets: takes off every worn piece `takeOff` accepts, giving each copy a unique ID first so
    // the same one can go back on. Returns the copies it took off. A protected piece (ExtraCannotWear,
    // as Tailor's NPC wigs are) is skipped before `takeOff` is asked about it.
    std::vector<HeadwearCopy> TakeOffHeadwear(RE::Actor* actor, const std::function<bool(RE::TESObjectARMO*)>& takeOff);
    // Where `copy` is now: gone from the actor's inventory, there but off, or worn.
    Tailor::Situations::CopyState HeadwearCopyState(RE::Actor* actor, const HeadwearCopy& copy);

    enum class PutBack { Worn, Gone, Failed };
    // Puts `copy` back on. Gone when the actor no longer has it (sold, dropped, replaced): a copy is
    // never manufactured.
    PutBack PutHeadwearBack(RE::Actor* actor, const HeadwearCopy& copy);

    class HeadwearPreview
    {
    public:
        bool Hide(RE::Actor* actor, RE::TESObjectARMO* wig);
        bool Restore(RE::Actor* actor);
        bool Empty() const { return _displaced.empty(); }
        void Clear() { _displaced.clear(); } // world reverted; never touch new inventory

    private:
        using Copy = HeadwearCopy;
        std::vector<Copy> _displaced;
    };
}
