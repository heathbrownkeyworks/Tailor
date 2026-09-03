#include "pch.h"

#include "events/PowerHandler.h"
#include "Settings.h"
#include "ui/TailorUI.h"

namespace
{
    constexpr auto kTailorPowerEditorID = "Tailor_PowerUI";
}

PowerHandler& PowerHandler::GetSingleton()
{
    static PowerHandler singleton;
    return singleton;
}

void PowerHandler::Register()
{
    auto* sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!sourceHolder) {
        logger::warn("PowerHandler: ScriptEventSourceHolder unavailable; Lesser Power cast handling disabled");
        return;
    }

    sourceHolder->AddEventSink<RE::TESSpellCastEvent>(&GetSingleton());
    logger::info("PowerHandler registered");
}

void PowerHandler::GrantTailorPower()
{
    if (!Settings::GetSingleton().GetGrantPower()) {
        logger::info("PowerHandler: GrantPower=0; skipping Tailor Lesser Power grant");
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::warn("PowerHandler: Player unavailable; Tailor Lesser Power was not granted");
        return;
    }

    auto* spell = LookupTailorPower();
    if (!spell) {
        logger::error("PowerHandler: '{}' not found; make sure Tailor.esp is enabled", kTailorPowerEditorID);
        return;
    }

    // Favorite only when the spell is newly granted, so removing it from
    // Favorites later is respected instead of being undone on every load.
    if (player->HasSpell(spell)) {
        return;
    }

    player->AddSpell(spell);
    logger::info("PowerHandler: Granted {}", kTailorPowerEditorID);

    if (!Settings::GetSingleton().GetAutoFavorite()) {
        return;
    }

    auto* magicFavorites = RE::MagicFavorites::GetSingleton();
    if (!magicFavorites) {
        logger::warn("PowerHandler: MagicFavorites unavailable; {} was not added to favorites", kTailorPowerEditorID);
        return;
    }

    bool alreadyFavorite = false;
    for (auto* favorite : magicFavorites->spells) {
        if (favorite && favorite->GetFormID() == spell->GetFormID()) {
            alreadyFavorite = true;
            break;
        }
    }

    if (!alreadyFavorite) {
        magicFavorites->SetFavorite(spell);
        logger::info("PowerHandler: Added {} to favorites", kTailorPowerEditorID);
    }
}

RE::BSEventNotifyControl PowerHandler::ProcessEvent(
    const RE::TESSpellCastEvent* event,
    RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
    if (!event) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* caster = event->object.get();
    if (!caster || caster != RE::PlayerCharacter::GetSingleton()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* spell = LookupTailorPower();
    if (!spell || event->spell != spell->GetFormID()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    SKSE::GetTaskInterface()->AddTask([]() {
        TailorUI::GetSingleton().Toggle();
    });

    return RE::BSEventNotifyControl::kContinue;
}

RE::SpellItem* PowerHandler::LookupTailorPower()
{
    return RE::TESForm::LookupByEditorID<RE::SpellItem>(kTailorPowerEditorID);
}
