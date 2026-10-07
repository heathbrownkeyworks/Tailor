#include "events/SituationHandler.h"
#include "api/ModOverrides.h"
#include "events/WarmWeather.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitStore.h"
#include "player/PlayerIds.h"
#include "player/PlayerTarget.h"
#include "player/PlayerWardrobe.h"
#include "ui/TailorUI.h"
#include "wig/WigAssignments.h"
#include "wig/WigManager.h"
#include "wig/WigSituationPolicy.h"

#include <algorithm>
#include <chrono>
#include <utility>

// SituationHandler's player half. The player's situations follow the NPC rules, dressed at once
// through the inventory, with rules of their own for combat, water and beast form. The NPC code
// in SituationHandler.cpp only branches here.

using Tailor::Player::PlayerRule;

namespace
{
    // The wake-up note's space: the interior cell, else the worldspace outdoors; 0 while neither is known.
    // Not the exterior grid cell, which changes every 4096 units.
    std::uint32_t PlayerSpace(RE::Actor* player)
    {
        if (auto* cell = player->GetParentCell(); cell && cell->IsInteriorCell()) return cell->GetFormID();
        if (auto* world = player->GetWorldspace()) return world->GetFormID();
        return 0;
    }
}

bool SituationHandler::PlayerHasSituations() const
{
    // Situations, or a mod's override, manage the player's outfit.
    return OutfitAssignments::GetSingleton().HasAnySituation(Tailor::Player::kPlayerRef) ||
        WigAssignments::GetSingleton().HasAnySituation(Tailor::Player::kPlayerRef) ||
        ModOverrides::GetSingleton().Contains(Tailor::Player::kPlayerRef);
}

void SituationHandler::ReconcilePlayerSituation(bool redress)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;
    _player.applied.reset();
    ApplyPlayerSituation(player, redress ? PlayerTrigger::Redress : PlayerTrigger::Now);
}

void SituationHandler::DeferPlayerSituation(PlayerTrigger trigger, const char* reason)
{
    // Logged when the wait starts, not on every poll that finds it still waiting. A headgear wait's
    // retries replay with pending cleared, so only its first try counts as a start.
    if (!_player.pending && _player.headwearWaits <= 1) logger::info("Situations: the player's change waits: {}", reason);
    _player.pending = true;
    _player.pendingTrigger = std::max(_player.pendingTrigger, trigger);
}

void SituationHandler::ReconcilePlayerWhenDressable()
{
    _player.reconcileAfterLoad = true;
    logger::info("Situations: the player can't be dressed yet; the poll finishes the load's reconcile");
}

void SituationHandler::ForgetPlayerSituationState()
{
    _player.applied.reset();
    _player.pending = false;
    _player.pendingTrigger = PlayerTrigger::Automatic;
    _player.headwearWaits = 0;
    _player.wakeUp = {};
}

void SituationHandler::ApplyPlayerSituation(RE::Actor* player, PlayerTrigger trigger)
{
    // Tailor's own actions go ahead while it is open; an automatic event waits, even one that
    // carries a Redress or Now that waited.
    const bool automatic = trigger == PlayerTrigger::Automatic;
    // A change that waited replays as the strongest trigger that waited, whether the poll or another
    // automatic change brings it back: a deferred Redress or Now keeps its forced dress and wig step.
    if (_player.pending) trigger = std::max(trigger, _player.pendingTrigger);
    if (!PlayerHasSituations()) {
        ForgetPlayerSituationState();
        return;
    }
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    // Automatic changes wait for Tailor to close, and nothing replaces an open preview.
    if ((automatic && TailorUI::GetSingleton().IsOpen()) || wardrobe.IsPreviewing()) {
        DeferPlayerSituation(trigger, "Tailor or a preview is open");
        return;
    }
    const auto rule = Tailor::Player::DecidePlayerRule(Tailor::Player::CanDress(player),
        _player.fight.fighting || player->IsInCombat(), IsPlayerInWater(player));
    // In beast form the transformation handles gear; the player is dressed when they turn back.
    // Whatever keeps the player from being dressed, the change waits for the poll instead of being lost.
    if (rule == PlayerRule::Suspended) {
        DeferPlayerSituation(trigger, "the player can't be dressed");
        return;
    }
    // Turning back from beast form is the poll's to notice: until it has adopted the own-gear note, a
    // change waits, so nothing dresses the player first and records Own Gear from the bare body.
    if (_player.beast.transformed) {
        DeferPlayerSituation(trigger, "the poll hasn't seen the player turn back yet");
        return;
    }
    _player.pending = false;
    _player.pendingTrigger = PlayerTrigger::Automatic;
    // The day's random pick a save shows belongs to the first look chosen after the load, even one
    // that had to wait: it is taken here, where a look is chosen, never by a change that then waits.
    const bool keepSavedPick = std::exchange(_player.keepSavedPick, false);

    const auto playerId = player->GetFormID();
    // A player who woke up in their Sleep outfit keeps it until they walk away from where they woke;
    // a fight or swimming takes over for good, and clearing the Sleep outfit and wig ends it too.
    if (_player.wakeUp.active && (rule != PlayerRule::Land || !PlayerWakeUpHolds(player))) {
        _player.wakeUp = {};
        logger::info("Situations: the player is up; their situation dresses them again");
    }
    const auto situation = rule != PlayerRule::Land ? OutfitSituation::Adventuring
        : _player.wakeUp.active ? OutfitSituation::Sleep : EvaluateSituation(player, false);
    // The wig follows the situation beneath Warm, which has no wig.
    const auto wigSituation = WigSituationFor(player, situation);
    const auto* assignment = OutfitAssignments::GetSingleton().GetAssignment(playerId);
    // A mod's override the player can wear; one they no longer fit (its sex was changed) is skipped, never
    // removed, as for NPCs. It is what the land rule resolves to. A fight follows the player's own rules:
    // with situation outfits it wears the Adventuring one, without them it leaves the override on.
    const auto forced = ModOverrides::GetSingleton().Get(playerId);
    const bool overridden = forced && OutfitManager::WearableBy(player)(*forced);
    int outfitId = Tailor::Player::kKeepOutfit;
    int slotId = 0;
    if (Tailor::Player::PlayerRuleManagesOutfit(rule, assignment && assignment->HasAnySituation())) {
        // The slot the rule rolls from: a fight's Adventuring, water's Swimming, on land the
        // situation's own (in Sleep without a Sleep outfit the player can wear, the slot of the
        // situation besides Sleep; in Warm without a Warm outfit, the location's; in Home without a
        // Home outfit, Town's), else Adventuring's, which the chain falls back to.
        const auto hasSlot = [&](OutfitSituation s) { return assignment && (assignment->GetSlot(s) > 0 || assignment->GetRandomFlag(s)); };
        auto landSlot = situation == OutfitSituation::Sleep && !HasSleepOutfit(playerId)
            ? (Tailor::Situations::IsColdOutdoors(player) ? OutfitSituation::Warm : EvaluateLocationSituation(player)) : situation;
        if (landSlot == OutfitSituation::Warm && !hasSlot(OutfitSituation::Warm)) landSlot = EvaluateLocationSituation(player);
        if (landSlot == OutfitSituation::Home && !hasSlot(OutfitSituation::Home)) landSlot = OutfitSituation::Town;
        const auto slot = rule == PlayerRule::Fight ? OutfitSituation::Adventuring
            : rule == PlayerRule::Swim ? OutfitSituation::Swimming
            : hasSlot(landSlot) ? landSlot
            : OutfitSituation::Adventuring;
        if (keepSavedPick) KeepPlayerRandomPick(slot, wardrobe.WornOutfitId());
        slotId = rule == PlayerRule::Land ? 0 : ResolveSituationSlot(playerId, slot);
        const int landId = rule == PlayerRule::Land ? ResolveOutfitForSituation(playerId, situation) : 0;
        outfitId = Tailor::Player::PlayerOutfitFor(rule, slotId, landId);
    }
    // In the water a mod's override stays on, and comes back after a fight that ended there: no Swimming
    // outfit replaces it. While it is worn nothing changes, since only another outfit than the one worn goes on.
    if (overridden && rule == PlayerRule::Swim) outfitId = *forced;
    // Where the rule leaves the outfit alone (a fight or water), a redress still puts the recorded
    // outfit on again: turning back from beast form can leave it off.
    if (trigger == PlayerTrigger::Redress) outfitId = Tailor::Player::PlayerRedressOutfit(outfitId, wardrobe.WornOutfitId());
    // Water hides weapons only when a Swimming outfit went on (one the player can wear), not when a
    // redress or a mod's override keeps the worn outfit in the water.
    const PlayerLook look{rule, situation, outfitId, rule == PlayerRule::Swim && slotId > 0 && !overridden, wigSituation};
    // As for NPCs, an unchanged look is left alone, and below only another outfit than the one
    // worn goes on: gear the player changes by hand stays until another outfit is called for.
    // A change that turns out unchanged also ends its wait for headgear.
    if (trigger != PlayerTrigger::Redress && _player.applied == look) {
        _player.headwearWaits = 0;
        return;
    }
    // Headgear a closed Tailor session couldn't put back goes back first; until it can, the change
    // waits for the poll instead of failing and being taken for done. The wait is capped, since a
    // restore that keeps failing would retry and log on every poll: after two seconds the dress goes
    // ahead as it did before the wait, and a refusal is taken for done.
    if (Tailor::Player::PlayerWaitsForHeadwear(_player.headwearWaits) && !WigManager::GetSingleton().RestorePendingHeadwear(player)) {
        ++_player.headwearWaits;
        DeferPlayerSituation(trigger, "headgear is still being put back");
        return;
    }
    _player.headwearWaits = 0;

    // Only another outfit than the one worn goes on, so a load changes nothing the save shows, unless
    // the worn outfit's pieces were edited since, in another save.
    if (outfitId != Tailor::Player::kKeepOutfit && (trigger == PlayerTrigger::Redress || wardrobe.WornOutfitId() != outfitId ||
            OutfitManager::GetSingleton().PlayerOutfitPiecesChanged(outfitId))) {
        auto& mgr = OutfitManager::GetSingleton();
        bool dressed = false;
        if (outfitId == 0) {
            dressed = mgr.RestoreOriginalOutfit(player, true);
            OutfitManager::NotifyOutfitChanged(player);
        } else if (const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId)) {
            dressed = mgr.ApplyCustomOutfit(player, *outfit, true);
        }
        logger::info("Situations: {} the player for rule {}, situation {}, outfit {}",
            dressed ? "dressed" : "could not dress", static_cast<int>(rule), static_cast<int>(situation), outfitId);
    }
    // Situation wigs as for NPCs: the situation's, in Sleep the wig of the situation besides Sleep, for
    // Home (or Sleep beside it) the Town wig, else the Adventuring wig; when the player leaves Sleep with
    // the Sleep wig on and none of those is set, the assigned wig, else their own hair; otherwise the wig
    // worn now stays. The wig step runs at a change of situation, or for Tailor's own actions and the
    // reconciles; a fight or a swim in between doesn't reach it, so a wig the player took off stays off.
    // Warm has no wig: the step goes by the situation beneath it, so the weather turning never runs it.
    if (Tailor::Player::PlayerWigStepRuns(rule, trigger == PlayerTrigger::Automatic, _player.wigSituation != wigSituation)) {
        // Whether this change can end Sleep: the wig step last ran for Sleep, or hasn't run since the load.
        const bool fromSleep = !_player.wigSituation || *_player.wigSituation == OutfitSituation::Sleep;
        _player.wigSituation = wigSituation;
        auto& wigs = WigAssignments::GetSingleton();
        const auto worn = wigs.GetState(playerId);
        // A dormant assigned wig (its plugin missing) can't go on: their own hair comes back instead.
        const auto assigned = worn && worn->assignedWig.Resolve() ? worn->assignedWig : WigEntry{};
        const auto choice = Tailor::Wigs::ChooseSituationWig(wigSituation,
            wigSituation == OutfitSituation::Sleep ? EvaluateLocationSituation(player) : wigSituation, fromSleep,
            // A situation wig whose plugin is missing can't go on: as a wig to put on, it counts as none, as for NPCs.
            [&](OutfitSituation slot) {
                const auto wig = wigs.GetSituationWig(playerId, slot);
                // Leaving Sleep, the stored Sleep wig still names the one worn, so the Sleep-ends fallback answers
                // even when its mod is missing; every wig that could go on is still filtered.
                if (slot == OutfitSituation::Sleep && wigSituation != OutfitSituation::Sleep) return wig;
                return wig.Resolve() ? wig : WigEntry{};
            },
            worn ? worn->currentWig : WigEntry{}, assigned);
        if (choice.step == Tailor::Wigs::SituationWigStep::Equip) WigManager::GetSingleton().EquipWig(player, choice.wig);
        else if (choice.step == Tailor::Wigs::SituationWigStep::TakeOff) WigManager::GetSingleton().TakeOffSituationWig(player);
    }
    _player.applied = look;
}

void SituationHandler::KeepPlayerRandomPick(OutfitSituation slot, int worn)
{
    const auto playerId = Tailor::Player::kPlayerRef;
    const auto* assignment = OutfitAssignments::GetSingleton().GetAssignment(playerId);
    const int index = static_cast<int>(slot);
    if (!assignment || worn <= 0 || index < 1 || index > kLastOutfitSituation || !assignment->GetRandomFlag(slot)) return;
    auto& pick = _randomStates[playerId].slots[index - 1];
    // Only a slot with no pick yet: a pick from another day still rolls anew.
    if (pick.lastRandomOutfitId != 0 || pick.lastRandomDay >= 0.0f) return;
    const auto pool = RandomPool(playerId, slot);
    if (std::ranges::find(pool, worn) == pool.end()) return;
    // The outfit the save shows the player wearing stays today's pick.
    pick.lastRandomOutfitId = worn;
    pick.lastRandomDay = GetGameDaysPassed();
    logger::info("Situations: the player's random pick for situation {} stays outfit {} today", index, worn);
}

void SituationHandler::PollPlayer()
{
    // The day's saved random pick belongs to the first look after a load. While that look waits (a
    // pending change, or a load's reconcile waiting for the player) the pick waits with it; otherwise
    // from here on picks roll as for NPCs.
    if (!_player.pending && !_player.reconcileAfterLoad) _player.keepSavedPick = false;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded() || player->IsDead()) return;
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = _player.lastPoll ? std::chrono::duration<float>(now - *_player.lastPoll).count() : 0.0f;
    _player.lastPoll = now;
    // Combat's five seconds run while the game does, beast form included, so turning back after
    // a fight dresses the player for their situation, not for the fight.
    const bool fightChanged = _player.fight.Update(player->IsInCombat(), elapsed);
    // A werewolf or Vampire Lord is left alone. Turning back can leave the gear off, so the player
    // is dressed again even in the outfit already recorded: for their situation, or without
    // situations their regular outfit, else Own Gear.
    if (_player.beast.TurnedBack(Tailor::Player::InBeastForm(player))) {
        logger::info("Situations: the player turned back from beast form");
        // The own gear noted before the transformation is Own Gear again: the redress puts it back
        // on, or keeps it for later under an outfit. Without situations the vanilla turn-back stays.
        if (PlayerHasSituations() && Tailor::Player::PlayerWardrobe::GetSingleton().AdoptOwnGearNote()) {
            logger::info("Situations: the player's own gear from before beast form is their Own Gear");
        }
        OutfitManager::GetSingleton().RedressPlayer();
        // RedressPlayer has nothing to put back for a player with only a Tailor wig and no
        // outfit; the transformation can still have taken the wig off, so cover that here too.
        WigManager::GetSingleton().ReEquipWigAfterOutfitChange(player);
        return;
    }
    // A load that came before the player could be dressed: its reconcile runs now, outfit then wig.
    if (_player.reconcileAfterLoad && Tailor::Player::CanDress(player)) {
        _player.reconcileAfterLoad = false;
        logger::info("Situations: the player can be dressed now; finishing the load's reconcile");
        OutfitManager::GetSingleton().ReconcilePlayer();
        WigManager::GetSingleton().ReconcilePlayerWig();
    }
    // Clearing the last situations in Tailor dresses the player without the situation apply: forget its
    // state here too, so no stale note or wait outlives them.
    if (!PlayerHasSituations()) ForgetPlayerSituationState();
    // Beast form ends a wake-up: turning back dresses the player for their situation, not the bed.
    if (_player.beast.transformed) _player.wakeUp = {};
    if (_player.beast.transformed || !PlayerHasSituations()) return;
    // Noted on every poll while the player wears their own gear; beast form stops the poll above,
    // which freezes the note at what they wore before the transformation.
    Tailor::Player::PlayerWardrobe::GetSingleton().NoteOwnGear(player);
    const bool swimming = IsPlayerInWater(player);
    // Outdoors in cold weather or a snowy region: Warm starts or ends.
    const bool cold = Tailor::Situations::IsColdOutdoors(player);
    // A note from the co-save carries no space: the first poll that sees the player keys it to where they are.
    if (_player.wakeUp.active && _player.wakeUp.space == 0) _player.wakeUp.space = PlayerSpace(player);
    // In bed by the bed check (a bed-animation mod), or just woken and still by the bed.
    const bool sleeping = IsSleeping(player) || PlayerWakeUpHolds(player);
    if (fightChanged) logger::info("Situations: the player {} fighting", _player.fight.fighting ? "is" : "is no longer");
    if (swimming != _player.swimming) logger::info("Situations: the player {} in the water", swimming ? "is" : "is no longer");
    if (sleeping != _player.sleeping) logger::info("Situations: the player {} in bed or by the bed they woke in", sleeping ? "is" : "is no longer");
    if (cold != _player.cold) logger::info("Situations: the player {} outdoors in the cold", cold ? "is" : "is no longer");
    const bool changed = fightChanged || swimming != _player.swimming || sleeping != _player.sleeping || cold != _player.cold;
    _player.swimming = swimming;
    _player.sleeping = sleeping;
    _player.cold = cold;
    if (!changed && !_player.pending) return;
    ApplyPlayerSituation(player, PlayerTrigger::Automatic);
}

void SituationHandler::PlayerWokeUp()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    // Waking changes nothing for a player without a Sleep outfit they can wear, or a Sleep wig.
    if (!player || !Tailor::Player::CanDress(player) || !PlayerHasSituations() || !PlayerHasSleepLook()) return;
    // A sleep outlasts the five seconds after a fight: only a fight after waking takes over.
    if (!player->IsInCombat()) _player.fight = {};
    const auto position = player->GetPosition();
    _player.wakeUp.Woke(position.x, position.y, position.z, PlayerSpace(player));
    ApplyPlayerSituation(player, PlayerTrigger::Automatic);
    // A fight or water at waking ends the note at once, and the apply logs that instead.
    if (_player.wakeUp.active) logger::info("Situations: the player woke up; their Sleep outfit stays on until they walk away");
}

bool SituationHandler::PlayerHasSleepLook() const
{
    return HasSleepLook(Tailor::Player::kPlayerRef);
}

bool SituationHandler::PlayerWakeUpHolds(RE::Actor* player) const
{
    if (!_player.wakeUp.active || !PlayerHasSleepLook()) return false;
    const auto position = player->GetPosition();
    return !_player.wakeUp.WalkedAway(position.x, position.y, position.z, PlayerSpace(player));
}

Tailor::Player::PlayerWakeUp SituationHandler::ExportPlayerWakeUp() const
{
    return _player.wakeUp;
}

void SituationHandler::ImportPlayerWakeUp(const Tailor::Player::PlayerWakeUp& note)
{
    _player.wakeUp = note;
}

bool SituationHandler::IsPlayerInWater(RE::Actor* player) const
{
    const bool swimming = player->AsActorState()->IsSwimming();
    // On horseback the rider's feet are in the stirrups, well above the water the horse wades: only
    // swimming counts.
    if (player->IsOnMount()) return swimming;
    // Midair counts only while already in the water (a jump while wading); out of it, skip the engine read.
    return Tailor::Player::PlayerInWater(_player.swimming, swimming, _player.swimming && player->IsInMidair(),
        player->GetWaterHeight(), player->GetPosition().z);
}

bool SituationHandler::PlayerWeaponsLookOn() const
{
    if (!_player.applied || _player.beast.transformed || !PlayerHasSituations()) return false;
    // A mod's override they wear decides alone (OverrideLook).
    if (const auto overrideLook = OverrideLook(Tailor::Player::kPlayerRef, Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId())) {
        return *overrideLook;
    }
    const auto& look = *_player.applied;
    // Water hides weapons only with a Swimming outfit on; without one, the outfit stays and hides nothing.
    return (look.rule == PlayerRule::Swim && look.swimmingOutfit) ||
        (look.rule == PlayerRule::Land && look.situation == OutfitSituation::Sleep && PlayerHasSleepLook());
}
