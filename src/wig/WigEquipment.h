#pragma once

namespace RE
{
    class Actor;
    class TESObjectARMO;
}

namespace Tailor::Wigs
{
    // Game-thread only. Protect an already-worn instance in place, or request
    // an equip with the engine's PreventRemoval flag. Never adds inventory.
    bool EquipProtectedWig(RE::Actor* actor, RE::TESObjectARMO* armor);

    // Release the currently worn wig before a deliberate reset/replacement.
    // Unworn copies and other armor retain their existing extra data.
    void ReleaseWigProtection(RE::Actor* actor, RE::TESObjectARMO* armor);
}
