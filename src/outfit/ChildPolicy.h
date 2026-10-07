#pragma once

// Tailor never handles children. These are the kinds of state Tailor can hold for an actor; a child holding any of
// them was dressed by an earlier build and is released once (Tailor::Children::Skip). A child holding none is left
// alone, so another mod's outfit on them is never replaced.
namespace Tailor::Children
{
    struct Held
    {
        bool outfitRow = false;       // an outfit assignment row: outfits, random flags, armor type or a pending restore
        bool wigRow = false;          // a worn or assigned wig, or a hair color
        bool situationWigs = false;   // situation wigs
        bool modOverride = false;     // a mod's override
        bool returningToOwn = false;  // the mark that gives an unmanaged NPC their own outfit back
        bool helmets = false;         // helmets Hide Helmets took off
    };

    [[nodiscard]] constexpr bool NeedsRelease(const Held& held)
    {
        return held.outfitRow || held.wigRow || held.situationWigs || held.modOverride || held.returningToOwn ||
            held.helmets;
    }
}
