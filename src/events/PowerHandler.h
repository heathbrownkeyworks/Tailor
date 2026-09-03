#pragma once

class PowerHandler final : public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
    static PowerHandler& GetSingleton();
    static void Register();
    static void GrantTailorPower();

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESSpellCastEvent* event,
        RE::BSTEventSource<RE::TESSpellCastEvent>*) override;

private:
    PowerHandler() = default;

    static RE::SpellItem* LookupTailorPower();
};
