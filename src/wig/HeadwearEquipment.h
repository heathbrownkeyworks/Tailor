#pragma once

#include <cstdint>
#include <vector>

namespace Tailor::Wigs
{
    // ARMO controls equipment conflicts; compatible ARMA head slots also tell
    // us when two otherwise wearable items would hide each other's hair model.
    bool HeadwearConflicts(RE::Actor* actor, RE::TESObjectARMO* wig, RE::TESObjectARMO* armor);
    bool OutfitHidesWig(RE::Actor* actor, RE::TESObjectARMO* wig, RE::TESObjectARMO* previousWig = nullptr);
    bool SuspendWig(RE::Actor* actor, RE::TESObjectARMO* wig);

    class HeadwearPreview
    {
    public:
        bool Hide(RE::Actor* actor, RE::TESObjectARMO* wig);
        bool Restore(RE::Actor* actor);
        bool Empty() const { return _displaced.empty(); }
        void Clear() { _displaced.clear(); } // world reverted; never touch new inventory

    private:
        struct Copy
        {
            RE::FormID form;
            RE::FormID owner;
            std::uint16_t unique;
            bool operator==(const Copy&) const = default;
        };
        std::vector<Copy> _displaced;
    };
}
