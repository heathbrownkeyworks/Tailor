#include "wig/WigManager.h"
#include "events/SituationHandler.h"
#include "outfit/OutfitStore.h"
#include "player/PlayerModel.h"
#include "player/PlayerTarget.h"
#include "player/PlayerWardrobe.h"
#include "player/PlayerWigPolicy.h"
#include "preview/TailorPreviewSession.h"
#include "ui/TailorUI.h"

#include <chrono>
#include <thread>

namespace
{
    // The menus where the player handles their own gear.
    bool InInventoryMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        for (const auto name : {RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::BarterMenu::MENU_NAME,
                 RE::GiftMenu::MENU_NAME, RE::FavoritesMenu::MENU_NAME}) {
            if (ui->IsMenuOpen(name)) return true;
        }
        return false;
    }

    // Tailor's wig library, as forms. A worn wig of the player's own from it is not headgear
    // over Tailor's wig, which takes its place.
    std::vector<RE::TESObjectARMO*> LibraryWigs()
    {
        return WigLibrary::GetSingleton().ResolvedWigs();
    }

    // The library wigs that don't hide Tailor's wig on the player: all of them but the pieces of the
    // outfit they wear, whose wig wins, as NPCs keep outfit wigs.
    std::vector<RE::TESObjectARMO*> PlayerNotHeadgear()
    {
        std::vector<RE::TESObjectARMO*> outfitPieces;
        const int worn = Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId();
        if (const auto* outfit = worn > 0 ? OutfitStore::GetSingleton().GetOutfitById(worn) : nullptr) {
            for (const auto& item : outfit->items) {
                if (auto* armor = item.Resolve()) outfitPieces.push_back(armor);
            }
        }
        // A preview's pieces are what the player wears until Cancel or Set, so the re-equip after each
        // preview step must not push the previewed outfit's wig off.
        for (const auto form : Tailor::Player::PlayerWardrobe::GetSingleton().ChosenForms()) {
            if (auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(form)) outfitPieces.push_back(armor);
        }
        return Tailor::Player::WigsNotHeadgear(LibraryWigs(), outfitPieces);
    }
}

// WigManager's player half. The player's wig goes on and comes off through PlayerWardrobe,
// which adds a tagged copy only when the player owns none and takes back only that copy.
// The wig is never locked on the player, so they may take it off in their inventory. The
// NPC code in WigManager.cpp only branches here.

bool WigManager::EquipPlayerWig(RE::Actor* player, const WigEntry& wig)
{
    if (!Tailor::Player::CanDress(player)) {
        logger::warn("EquipWig: the player can't be dressed now");
        return false;
    }
    auto* armor = wig.Resolve();
    if (!armor) {
        logger::warn("Failed to resolve wig '{}' from plugin '{}'", wig.name, wig.plugin);
        return false;
    }
    std::lock_guard lock(_mutex);
    const auto wigChange = _wigRecovery.BeginChange(player->GetFormID());
    _playerWigLeftOff = false;  // Tailor changing the wig ends the player's choice to leave it off
    auto& assignments = WigAssignments::GetSingleton();
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    const auto existing = assignments.GetState(player->GetFormID());
    auto* oldArmor = existing ? existing->currentWig.Resolve() : nullptr;
    const bool wigScreen = _wigScreen && player->GetHandle() == _headwearActor;
    if (wigScreen && !_headwearPreview.Hide(player, oldArmor ? oldArmor : armor)) return false;
    // A helmet or hood wins: the wig is kept for when the headgear comes off. A library wig of the
    // player's own does not, unless it is part of the outfit they wear: Tailor's wig takes its place.
    const bool hidden = !wigScreen && Tailor::Wigs::OutfitHidesWig(player, armor, oldArmor, PlayerNotHeadgear());
    if (!wigScreen && !hidden && oldArmor == armor && wardrobe.State().keptWig == armor->GetFormID() &&
        player->GetWornArmor(armor->GetFormID())) {
        return true;  // already on: nothing to rebuild
    }
    if (auto* npc = player->GetActorBase()) EnsureArmorAddonRace(armor, npc->GetRace(), npc->GetSex());
    const bool worn = wardrobe.SetWig(player, armor, !hidden);
    assignments.SetAssignment(player->GetFormID(), wig, false);
    Tailor::Player::RefreshPlayerModel(player);
    Tailor::Preview::TailorPreviewSession::GetSingleton().NotifyAppearanceChanged(player);
    if (const auto state = assignments.GetState(player->GetFormID()); state && state->HasHairColor()) {
        // A new wig model arrives in the player's own color; Tailor's tint follows it.
        ScheduleActorHairRetint(player->GetHandle(), {0, 1, 5});
    }
    logger::info("Assigned wig '{}' to the player ({})", wig.name, hidden ? "waiting under headgear" : worn ? "worn" : "refused");
    return hidden || worn;
}

bool WigManager::ResetPlayerWig(RE::Actor* player)
{
    std::lock_guard lock(_mutex);
    const auto wigChange = _wigRecovery.BeginChange(player->GetFormID());
    _playerWigLeftOff = false;
    Tailor::Player::PlayerWardrobe::GetSingleton().RemoveWig(player);
    WigAssignments::GetSingleton().ClearAssignment(player->GetFormID());
    Tailor::Player::RefreshPlayerModel(player);
    Tailor::Preview::TailorPreviewSession::GetSingleton().NotifyAppearanceChanged(player);
    logger::info("Reset the player's wig");
    return true;
}

void WigManager::ReEquipPlayerWig(RE::Actor* player)
{
    std::lock_guard lock(_mutex);
    const auto state = WigAssignments::GetSingleton().GetState(player->GetFormID());
    if (!state) return;
    // A wig the player took off stays off through outfit changes.
    if (state->currentWig.formId != 0 && !_playerWigLeftOff) {
        // A dormant wig (its plugin missing) doesn't go on, and EquipPlayerWig already says so.
        if (EquipPlayerWig(player, state->currentWig)) {
            logger::info("Reconciled the player's wig '{}' with headgear after an outfit change", state->currentWig.name);
        }
    } else if (state->HasHairColor()) {
        // The player's own hair shows; Tailor's tint goes back on it after the model changes.
        ScheduleActorHairRetint(player->GetHandle(), {0, 1, 5});
    }
}

void WigManager::ConfirmPlayerSituationCycle(RE::Actor* player, OutfitSituation situation, const WigEntry& wig)
{
    // The wig from before the Hair Dresser comes back first, so a situation with no wig keeps
    // it; then the current situation decides, as for NPCs.
    auto& assignments = WigAssignments::GetSingleton();
    const auto original = *_cycleState;
    _cycleState.reset();
    const auto worn = assignments.GetState(player->GetFormID());
    assignments.AssignSituation(player->GetFormID(), situation, wig);
    // AssignSituation forgets the current wig; keep the tried one tracked so the restore
    // below takes it off, and back when it is Tailor's copy.
    if (worn && worn->currentWig.formId != 0) assignments.SetAssignment(player->GetFormID(), worn->currentWig, false);
    const bool restored = original.hadOriginal ? EquipPlayerWig(player, original.originalWig) : ResetPlayerWig(player);
    if (const auto state = assignments.GetState(player->GetFormID()); state && state->HasHairColor()) {
        ScheduleActorHairRetint(player->GetHandle(), {0, 1, 5});
    }
    // Then, as for NPCs, the wig the player's current situation calls for goes on now.
    const bool keepWigScreen = IsWigScreen(player);
    SituationHandler::GetSingleton()->ForceApplyForSituation(player);
    if (keepWigScreen) SetWigScreen(true);
    logger::info("Wig cycling confirmed for the player's situation {}: '{}' (the wig from before is back: {})",
        static_cast<int>(situation), wig.name, restored);
}

bool WigManager::ApplyPlayerHairColor(RE::Actor* player)
{
    if (!player->Is3DLoaded()) return false;
    // ResolveHairTint reads the player's color row, which the caller has already set.
    RetintActorHair(player);
    logger::info("Applied the player's hair tint");
    return true;
}

bool WigManager::ResetPlayerHairColor(RE::Actor* player)
{
    {
        std::lock_guard lock(_mutex);
        ++_hairColorGeneration[player->GetFormID()];  // a delayed custom color must not undo Reset
    }
    // The caller cleared the player's color row, so the retint resolves their own color again.
    if (player->Is3DLoaded()) RetintActorHair(player);
    logger::info("Restored the player's own hair color");
    return true;
}

void WigManager::SchedulePlayerHairRetint()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    const auto state = WigAssignments::GetSingleton().GetState(player->GetFormID());
    if (state && state->HasHairColor()) ScheduleActorHairRetint(player->GetHandle(), {1, 5, 12});
}

bool WigManager::ResetPlayerToDefaultHair(RE::Actor* player)
{
    if (!Tailor::Player::CanDress(player)) return false;
    std::lock_guard lock(_mutex);
    const auto playerId = player->GetFormID();
    const auto wigChange = _wigRecovery.BeginChange(playerId);
    auto& assignments = WigAssignments::GetSingleton();
    // Every wig NPC Default Hair finds: Add Wigs' detection, the library, the player's wig
    // choices and the wigs of a cycle or preview still open.
    std::vector<std::uint32_t> wigForms;
    const auto remember = [&](const WigEntry& entry) {
        if (auto* armor = entry.Resolve()) wigForms.push_back(armor->GetFormID());
    };
    for (const auto& mod : ScanAllModWigs()) {
        for (const auto& wig : mod.wigs) remember({wig.formId, wig.plugin, wig.name});
    }
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        for (const auto& wig : WigLibrary::GetSingleton().GetCategory(static_cast<WigCategory>(i))) remember(wig);
    }
    if (const auto state = assignments.GetState(playerId)) remember(state->currentWig);
    for (const auto situation : {OutfitSituation::Adventuring, OutfitSituation::Town,
            OutfitSituation::Home, OutfitSituation::Sleep}) {
        remember(assignments.GetSituationWig(playerId, situation));
    }
    if (_cycleState) {
        remember(_cycleState->originalWig);
        for (const auto& wig : _cycleState->wigs) remember(wig);
    }
    if (_previewState) remember(_previewState->originalWig);
    // Wigs the player owns stay in the inventory; only Tailor's copies go back.
    if (!Tailor::Player::PlayerWardrobe::GetSingleton().RemoveWigs(player, wigForms)) {
        logger::warn("Default Hair: a wig stayed on the player");
        return false;
    }
    // Default Hair replaces the original of a cycle or preview too.
    _cycleState.reset();
    _previewState.reset();
    _playerWigLeftOff = false;
    assignments.ClearAssignment(playerId);
    assignments.SetAssignedWig(playerId, {});
    assignments.ClearAllSituations(playerId);
    // Default Hair doesn't reach ForceApplyForSituation: with no outfit situations left either, their state ends here.
    auto* situations = SituationHandler::GetSingleton();
    if (!situations->PlayerHasSituations()) situations->ForgetPlayerSituationState();
    Tailor::Player::RefreshPlayerModel(player);
    Tailor::Preview::TailorPreviewSession::GetSingleton().NotifyAppearanceChanged(player);
    if (const auto state = assignments.GetState(playerId); state && state->HasHairColor()) {
        ScheduleActorHairRetint(player->GetHandle(), {0, 1, 5});
    }
    logger::info("Default Hair: took the player's wigs off and cleared their wig choices");
    return true;
}

void WigManager::QueuePlayerWigRecovery(RE::Actor* player, RE::FormID changedArmorId, bool equipped, const char* eventName)
{
    auto* changed = RE::TESForm::LookupByID<RE::TESObjectARMO>(changedArmorId);
    if (!changed) return;
    std::lock_guard lock(_mutex);
    const auto state = WigAssignments::GetSingleton().GetState(player->GetFormID());
    auto* armor = state ? state->currentWig.Resolve() : nullptr;
    if (!armor) return;
    const bool wigChanged = changedArmorId == armor->GetFormID();
    // Judged now, while the menu the player acted in is still open. Only an equip there needs
    // the library, which costs a lookup per wig.
    const bool inMenu = InInventoryMenu();
    const bool otherLibraryWig = equipped && inMenu && !wigChanged && std::ranges::count(LibraryWigs(), changed) > 0;
    const auto decision = Tailor::Player::DecidePlayerWigEvent(_playerWigLeftOff, equipped, wigChanged, otherLibraryWig, inMenu);
    if (decision.resume) {
        _playerWigLeftOff = false;
        logger::info("Wig recovery: the player put their wig back on; Tailor looks after it again");
    }
    if (decision.leaveOff) {
        _playerWigLeftOff = true;
        logger::info("Wig recovery: the player put on another library wig in a menu; Tailor's stays off until Tailor next changes it");
    }
    if (!decision.queue) return;
    const auto request = _wigRecovery.Queue(player->GetFormID(), armor->GetFormID());
    if (!request) return;
    logger::info("Wig recovery: queued the player's wig {:08X} from {} of armor {:08X} (token {}, in a menu {})",
        armor->GetFormID(), eventName, changedArmorId, request->token, decision.takenOffInMenu);
    // Leave the equipment event stack first, as for NPCs.
    SKSE::GetTaskInterface()->AddTask([this, request = *request, takenOffInMenu = decision.takenOffInMenu]() { RecoverPlayerWig(request, takenOffInMenu); });
}

void WigManager::RecoverPlayerWig(Tailor::Wigs::WigRecoveryPolicy::Request request, bool takenOffInMenu)
{
    std::lock_guard lock(_mutex);
    if (!_wigRecovery.IsCurrent(request)) {
        logger::info("Wig recovery: skipped stale token {} for the player", request.token);
        return;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto state = WigAssignments::GetSingleton().GetState(request.actorId);
    auto* armor = state ? state->currentWig.Resolve() : nullptr;
    const bool sameAssignment = armor && armor->GetFormID() == request.wigFormId;
    const bool dressable = Tailor::Player::CanDress(player);
    // Tailor's own screens put the wig on and take it off while it is open.
    const bool tailorOpen = TailorUI::GetSingleton().IsOpen();
    const bool worn = dressable && armor && player->GetWornArmor(armor->GetFormID());
    const bool headwear = dressable && armor && Tailor::Wigs::OutfitHidesWig(player, armor, nullptr, PlayerNotHeadgear());
    // Still in the inventory: knocked off or taken off. Gone: dropped, sold, stored or given away.
    const bool inInventory = armor && GetActorItemCount(player, armor) > 0;
    switch (Tailor::Player::DecidePlayerWig(sameAssignment, dressable, tailorOpen, worn, headwear, takenOffInMenu, inInventory)) {
    case Tailor::Player::PlayerWigAction::LeaveOff:
        _playerWigLeftOff = true;
        logger::info("Wig recovery: the player took their wig off or got rid of it in a menu; it stays off until Tailor next changes it");
        break;
    case Tailor::Player::PlayerWigAction::TakeOff:
        Tailor::Player::PlayerWardrobe::GetSingleton().SetWig(player, armor, false);
        Tailor::Player::RefreshPlayerModel(player);
        logger::info("Wig recovery: headgear went on over the player's wig; the wig waits for it to come off");
        break;
    case Tailor::Player::PlayerWigAction::PutBack: {
        const bool putBack = Tailor::Player::PlayerWardrobe::GetSingleton().SetWig(player, armor, true);
        Tailor::Player::RefreshPlayerModel(player);
        if (state->HasHairColor()) ScheduleActorHairRetint(player->GetHandle(), {0, 1, 5});
        logger::info("Wig recovery: putting the player's wig back on after an outside change ({})", putBack ? "worn" : "refused");
        // Keep the request pending while the equip settles, as for NPCs: an immediate
        // unequip caused by this repair is absorbed rather than fought.
        std::thread([this, request]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            SKSE::GetTaskInterface()->AddTask([this, request]() {
                std::lock_guard finishLock(_mutex);
                if (!_wigRecovery.IsCurrent(request)) return;
                if (auto* player = RE::PlayerCharacter::GetSingleton(); Tailor::Player::CanDress(player)) {
                    if (player->GetWornArmor(request.wigFormId)) {
                        logger::info("Wig recovery: restored the player's wig {:08X}", request.wigFormId);
                    } else {
                        logger::warn("Wig recovery: the player's wig {:08X} is still unequipped; no repeated repair", request.wigFormId);
                    }
                }
                _wigRecovery.Finish(request);
            });
        }).detach();
        return;
    }
    case Tailor::Player::PlayerWigAction::Skip:
        logger::info("Wig recovery: skipped the player's wig (sameAssignment={}, dressable={}, tailorOpen={}, worn={}, headwear={}, inInventory={})",
            sameAssignment, dressable, tailorOpen, worn, headwear, inInventory);
        break;
    }
    _wigRecovery.Finish(request);
}

void WigManager::ReconcilePlayerWig()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!Tailor::Player::CanDress(player)) return;
    const auto state = WigAssignments::GetSingleton().GetState(player->GetFormID());
    if (!state) return;
    if (auto* armor = state->currentWig.Resolve()) {
        // The race patch lives in memory only: after a fresh launch a worn wig loads with no model
        // for a race that needs it. Patch first, worn, hidden or off, so later put-backs have it.
        auto* npc = player->GetActorBase();
        auto* race = npc ? npc->GetRace() : nullptr;
        const bool hadModel = race && armor->GetArmorAddon(race);
        if (npc) EnsureArmorAddonRace(armor, race, npc->GetSex());
        const bool worn = player->GetWornArmor(armor->GetFormID()) != nullptr;
        if (worn && !hadModel && race && armor->GetArmorAddon(race)) Tailor::Player::RefreshPlayerModel(player);
        // The save holds what the player wore. A load ends the player's choice to leave the wig
        // off, so only a wig that is off with no headgear over it goes back on.
        if (!worn && !Tailor::Wigs::OutfitHidesWig(player, armor, nullptr, PlayerNotHeadgear())) EquipPlayerWig(player, state->currentWig);
    }
    SchedulePlayerHairRetint();
}
