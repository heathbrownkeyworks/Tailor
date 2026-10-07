#include "events/SituationHandler.h"
#include "api/ModApi.h"
#include "api/ModApiPolicy.h"
#include "api/ModOverrides.h"
#include "events/HideSettingsPolicy.h"
#include "events/LocationSituationPolicy.h"
#include "events/RandomOutfitPolicy.h"
#include "events/SituationHelmets.h"
#include "events/SituationWeapons.h"
#include "events/SituationWeaponsPolicy.h"
#include "events/SleepStatePolicy.h"
#include "events/WarmPolicy.h"
#include "events/WarmWeather.h"
#include "wig/WigSituationPolicy.h"
#include "outfit/Children.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitStore.h"
#include "player/PlayerIds.h"
#include "player/PlayerTarget.h"
#include "player/PlayerWardrobe.h"
#include "PreferenceStore.h"
#include "wig/WigAssignments.h"
#include "wig/WigManager.h"
#include "ui/TailorUI.h"
#include <cmath>
#include <thread>
#include <chrono>

namespace
{
    std::atomic<std::uint64_t> sSituationGeneration{1};

    bool IsSleepSituation(RE::Actor* actor)
    {
        if (Tailor::Situations::IsSleepState(Tailor::Situations::ReadSleepState(actor))) return true;

        // Preserve furniture-based support for beds with unusual state/idle
        // behavior. Native sleep intent does not require an occupied bed yet.
        if (auto handle = actor->GetOccupiedFurniture(); handle) {
            if (auto ref = handle.get()) {
                if (auto* base = ref->GetBaseObject()) {
                    if (auto* furniture = base->As<RE::TESFurniture>()) {
                        return furniture->furnFlags.all(RE::TESFurniture::ActiveMarker::kCanSleep);
                    }
                }
            }
        }
        return false;
    }

    // The saved outfits this NPC can wear: they exist and fit the NPC's sex.
    OutfitLibrary::Wearable WearableBy(RE::FormID actorId)
    {
        return OutfitManager::WearableBy(RE::TESForm::LookupByID<RE::Actor>(actorId));
    }
}

SituationHandler* SituationHandler::GetSingleton()
{
    static SituationHandler singleton;
    return &singleton;
}

void SituationHandler::Register()
{
    auto* events = RE::ScriptEventSourceHolder::GetSingleton();
    if (events) {
        events->AddEventSink<RE::TESActorLocationChangeEvent>(GetSingleton());
        events->AddEventSink<RE::TESFurnitureEvent>(GetSingleton());
        events->AddEventSink<RE::TESSleepStopEvent>(GetSingleton());
        logger::info("SituationHandler registered for location change, furniture and sleep-stop events");
    }

    auto* ui = RE::UI::GetSingleton();
    if (ui) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(GetSingleton());
        logger::info("SituationHandler registered for menu open/close events (wait/sleep re-eval)");
    }
}

void SituationHandler::Initialize()
{
    _kwPlayerHouse = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000FC1A3);  // LocTypePlayerHouse
    _kwHouse       = RE::TESForm::LookupByID<RE::BGSKeyword>(0x0001CB85);  // LocTypeHouse
    _kwCity        = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013168);  // LocTypeCity
    _kwTown        = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013166);  // LocTypeTown
    _kwSettlement  = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013167);  // LocTypeSettlement
    _kwDwelling    = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000130DC);  // LocTypeDwelling
    _kwInn         = RE::TESForm::LookupByID<RE::BGSKeyword>(0x0001CB87);  // LocTypeInn

    logger::info("SituationHandler: keywords initialized (PlayerHouse={}, House={}, City={}, Town={}, Settlement={}, Dwelling={}, Inn={})",
        _kwPlayerHouse != nullptr, _kwHouse != nullptr, _kwCity != nullptr, _kwTown != nullptr,
        _kwSettlement != nullptr, _kwDwelling != nullptr, _kwInn != nullptr);
}

OutfitSituation SituationHandler::EvaluateSituation(RE::Actor* actor, bool includeSwimming, bool quiet) const
{
    if (!actor) return OutfitSituation::Adventuring;
    if (includeSwimming && Tailor::Situations::IsInWater(
            actor->GetWaterHeight(), actor->GetPosition().z, actor->AsActorState()->IsSwimming())) {
        return OutfitSituation::Swimming;
    }

    // Priority 1: sleep intent, entry, sleep and waking; no idle names required.
    if (IsSleepSituation(actor)) {
        if (!quiet) {
            logger::info("EvaluateSituation: {} sleepState={} → Sleep",
                actor->GetDisplayFullName(), static_cast<int>(Tailor::Situations::ReadSleepState(actor)));
        }
        return OutfitSituation::Sleep;
    }

    // Priority 1b: Warm, outdoors in cold weather or a snowy region.
    if (Tailor::Situations::IsColdOutdoors(actor)) {
        if (!quiet) logger::info("EvaluateSituation: {} outdoors in the cold → Warm", actor->GetDisplayFullName());
        return OutfitSituation::Warm;
    }

    return EvaluateLocationSituation(actor, quiet);
}

OutfitSituation SituationHandler::EvaluateLocationSituation(RE::Actor* actor, bool quiet) const
{
    if (!actor) return OutfitSituation::Adventuring;
    auto* location = actor->GetCurrentLocation();
    const auto has = [location](RE::BGSKeyword* keyword) { return keyword && location && location->HasKeyword(keyword); };
    // Home is the player's house; NPCs are Home in any house (LocTypeHouse). LocTypeDwelling, on nearly every
    // inhabited interior (inns, shops, temples, castles), stays Town.
    const auto situation = Tailor::Situations::LocationSituation<OutfitSituation>(location != nullptr, actor->IsPlayerRef(),
        has(_kwPlayerHouse), has(_kwHouse),
        has(_kwCity) || has(_kwTown) || has(_kwSettlement) || has(_kwDwelling) || has(_kwInn));
    if (!quiet) {
        if (!location) {
            logger::info("EvaluateSituation: {} has no location → Adventuring", actor->GetDisplayFullName());
        } else {
            logger::info("EvaluateSituation: {} at '{}' → {}", actor->GetDisplayFullName(), location->GetFullName(),
                situation == OutfitSituation::Home ? "Home" : situation == OutfitSituation::Town ? "Town" : "Adventuring (no keyword match)");
        }
    }
    return situation;
}

bool SituationHandler::IsSleeping(RE::Actor* actor) const
{
    return IsSleepSituation(actor);
}

// --- Randomized outfit helpers ---

float SituationHandler::GetGameDaysPassed()
{
    auto* global = RE::TESForm::LookupByID<RE::TESGlobal>(0x39);  // GameDaysPassed
    return global ? global->value : 0.0f;
}

int SituationHandler::ResolveRandomOutfit(RE::FormID actorId, OutfitSituation situation)
{
    int sitIdx = static_cast<int>(situation);
    if (sitIdx < 1 || sitIdx > kLastOutfitSituation) return 0;

    auto pool = RandomPool(actorId, situation);
    auto& rs = _randomStates[actorId].slots[sitIdx - 1];
    if (pool.empty()) {
        rs.lastRandomOutfitId = 0;
        return 0;
    }
    float currentDay = GetGameDaysPassed();

    if (!Tailor::Situations::IsRandomOutfitCurrent(rs.lastRandomOutfitId, rs.lastRandomDay, currentDay, pool)) {
        int previous = rs.lastRandomOutfitId;
        int poolSize = static_cast<int>(pool.size());
        std::uniform_int_distribution<int> dist(0, poolSize - 1);
        int picked = pool[dist(_rng)];

        // Avoid repeating the previous outfit when the pool has more than one option.
        // Without this the visual change is invisible to the user when the RNG repeats.
        if (poolSize > 1 && picked == previous) {
            int tries = 0;
            while (picked == previous && tries < 8) {
                picked = pool[dist(_rng)];
                ++tries;
            }
        }

        rs.lastRandomOutfitId = picked;
        rs.lastRandomDay = currentDay;
        logger::info("ResolveRandomOutfit: actor 0x{:X} sit={} rolled outfit {} (day={:.0f}, pool={}, prev={})",
            actorId, sitIdx, rs.lastRandomOutfitId, std::floor(currentDay), poolSize, previous);
    }

    return rs.lastRandomOutfitId;
}

// What a random slot rolls from: the situation's category, only outfits the actor can wear,
// and for Adventuring its armor type. The player's kept pick is checked against it too.
std::vector<int> SituationHandler::RandomPool(RE::FormID actorId, OutfitSituation situation) const
{
    static const char* sitTypeMap[] = { "", "adventuring", "town", "home", "sleep", "swimming", "warm" };
    const int sitIdx = static_cast<int>(situation);
    if (sitIdx < 1 || sitIdx > kLastOutfitSituation) return {};

    auto& library = OutfitLibrary::GetSingleton();
    const auto type = situation == OutfitSituation::Adventuring
        ? OutfitAssignments::GetSingleton().GetAdventuringArmorType(actorId) : OutfitArmorType::Any;
    return library.FilterAdventuringEligible(library.GetSituationOutfitIds(sitTypeMap[sitIdx]), type, WearableBy(actorId));
}

int SituationHandler::ResolveOutfitForSituation(RE::FormID actorId, OutfitSituation situation)
{
    // A mod's override answers for every situation but water, which leaves it on; managed or not. One the actor no
    // longer fits (its sex was changed) is skipped, never removed, as assigned outfits are, until it fits again.
    if (const auto forced = ModOverrides::GetSingleton().Get(actorId); forced && WearableBy(actorId)(*forced)) {
        return situation == OutfitSituation::Swimming ? 0 : *forced;
    }
    const auto* saved = OutfitAssignments::GetSingleton().GetAssignment(actorId);
    if (!saved) return 0;
    const auto assignment = *saved;
    if (situation == OutfitSituation::Swimming) {
        return ResolveSituationSlot(actorId, situation);
    }
    const auto wearable = WearableBy(actorId);
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
    // Sleep without a Sleep outfit the actor can wear falls back to the situation they'd be in out of bed: Warm
    // when they're outdoors in the cold, else the location. Warm without a Warm outfit falls back to the location.
    const bool needsLocation = situation == OutfitSituation::Sleep || situation == OutfitSituation::Warm;
    const auto location = needsLocation ? EvaluateLocationSituation(actor) : OutfitSituation::Adventuring;
    const auto besidesSleep = situation == OutfitSituation::Sleep
        ? (Tailor::Situations::IsColdOutdoors(actor) ? OutfitSituation::Warm : location) : OutfitSituation::Adventuring;
    return assignment.ResolveOutfit(situation, [this, actorId](OutfitSituation slot) {
        return ResolveRandomOutfit(actorId, slot);
    }, [&](int outfitId) {
        return OutfitLibrary::GetSingleton().IsAdventuringEligible(outfitId, assignment.adventuringArmorType, wearable);
    }, wearable, besidesSleep, location);
}

// HasSituationChoice must agree with this: HasSituation is true exactly when this would return an outfit, without
// rolling today's random pick.
int SituationHandler::ResolveSituationSlot(RE::FormID actorId, OutfitSituation situation)
{
    const auto* saved = OutfitAssignments::GetSingleton().GetAssignment(actorId);
    if (!saved) return 0;
    const auto assignment = *saved;
    const int selected = assignment.GetRandomFlag(situation)
        ? ResolveRandomOutfit(actorId, situation) : assignment.GetSlot(situation);
    const auto wearable = WearableBy(actorId);
    if (!wearable(selected)) return 0;
    if (situation == OutfitSituation::Adventuring &&
        !OutfitLibrary::GetSingleton().IsAdventuringEligible(selected, assignment.adventuringArmorType, wearable)) return 0;
    return selected;
}

int SituationHandler::SituationChoice(RE::FormID actorId, OutfitSituation situation)
{
    return ResolveSituationSlot(actorId, situation);
}

bool SituationHandler::HasSituationChoice(RE::FormID actorId, OutfitSituation situation) const
{
    // This must agree with the situation slot's resolution above, which picks: true exactly when that returns an
    // outfit, without rolling one.
    const auto* saved = OutfitAssignments::GetSingleton().GetAssignment(actorId);
    if (!saved) return false;
    const auto assignment = *saved;
    const bool random = assignment.GetRandomFlag(situation);
    const auto wearable = WearableBy(actorId);
    const int fixed = assignment.GetSlot(situation);
    const bool fixedWearable = !random && fixed > 0 && wearable(fixed) && (situation != OutfitSituation::Adventuring ||
        OutfitLibrary::GetSingleton().IsAdventuringEligible(fixed, assignment.adventuringArmorType, wearable));
    // Only the pool is read: picking from it would roll and record today's random outfit.
    return Tailor::Api::HasOwnChoice(random, fixedWearable, random && !RandomPool(actorId, situation).empty());
}

void SituationHandler::ApplyForSituation(RE::Actor* actor)
{
    if (!actor) return;
    // The player's situations have rules of their own and dress through the inventory.
    if (actor->IsPlayerRef()) {
        ApplyPlayerSituation(actor, PlayerTrigger::Automatic);
        return;
    }
    // Tailor never dresses a child; one an earlier build dressed gets their own outfit and hair back.
    if (Tailor::Children::Skip(actor)) return;

    auto& assignments = OutfitAssignments::GetSingleton();
    auto& wigAssignments = WigAssignments::GetSingleton();
    auto actorId = actor->GetFormID();

    const bool overridden = ModOverrides::GetSingleton().Contains(actorId);
    bool hasOutfitAssignment = overridden || assignments.HasAnySituation(actorId) || assignments.GetOutfitId(actorId) > 0;
    bool hasWigSituations = wigAssignments.HasAnySituation(actorId);
    if (!hasOutfitAssignment && !hasWigSituations && !_swimmingOverrides.contains(actorId) &&
        !_adventuringOverrides.contains(actorId)) {
        return;
    }

    if (actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded()) return;
    const bool inCombat = actor->IsInCombat();
    const int adventuringId = inCombat
        ? ResolveSituationSlot(actorId, OutfitSituation::Adventuring) : 0;
    if (ApplyAdventuringOverride(actor, adventuringId)) return;

    auto situation = EvaluateSituation(actor);
    const auto forced = ModOverrides::GetSingleton().Get(actorId);
    const bool overrideWearable = forced && WearableBy(actorId)(*forced);
    if (overrideWearable) {
        // A mod's override is worn in water too, as on the player: the swim switch is for Tailor's own Swimming
        // outfit. A swim found underway (their own Swimming outfit was on when the override was set) ends here
        // without putting back what they wore before the water, since the override replaces it.
        _swimmingOverrides.erase(actorId);
    } else if (ApplySwimmingOverride(actor, situation == OutfitSituation::Swimming) !=
        Tailor::Situations::OutfitOverrideResult::Inactive) return;

    // Revalidate the effective selection before the same-situation cache. A
    // changed preference/pool must not retain an ineligible daily random choice.
    const int outfitId = overrideWearable ? *forced : hasOutfitAssignment ? ResolveOutfitForSituation(actorId, situation) : 0;
    const auto current = _currentSituations.find(actorId);
    const auto applied = _appliedOutfitIds.find(actorId);
    const auto cached = current != _currentSituations.end() ? std::optional{current->second} : std::nullopt;
    const bool outfitOn = applied != _appliedOutfitIds.end() && applied->second == outfitId;
    // The wig follows the situation beneath Warm, which has no wig; the rules are Tailor::Situations::DecideNpcRedress.
    const auto wigSituation = hasWigSituations ? WigSituationFor(actor, situation) : situation;
    auto redress = Tailor::Situations::DecideNpcRedress(cached, situation, outfitOn, hasWigSituations,
        hasWigSituations && WigStepDue(actorId, wigSituation));
    // Swimming never changes the wig: under an override, water reaches this step. Walking out again, they find the
    // wig of the land they walked in from, so that runs no wig step either; after a forced re-dress in the water it
    // runs the land's, as any forced re-dress does.
    if (overrideWearable && situation == OutfitSituation::Swimming) redress.runWigStep = false;
    // An override already on stays put through Sleep and location flips. A forced re-dress, which forgets the
    // situation, puts it on again, since what was last applied may no longer be what they wear. One they no longer
    // fit dresses them by their usual rules.
    if (overridden && cached && outfitOn && ModOverrides::GetSingleton().Get(actorId) == outfitId) redress.putOutfitOn = false;
    if (!redress.dress) {
        if (redress.runWigStep) ApplySituationWig(actor, wigSituation);
        return;
    }

    // --- Outfit situational application ---
    if (hasOutfitAssignment && redress.putOutfitOn) {
        auto* outfit = outfitId > 0 ? OutfitStore::GetSingleton().GetOutfitById(outfitId) : nullptr;
        if (outfitId > 0 && !outfit) {
            logger::warn("ApplyForSituation: outfit id {} not found for {}", outfitId, actor->GetDisplayFullName());
            return;
        }
        const bool restored = outfit
            ? OutfitManager::GetSingleton().ApplyCustomOutfit(actor, *outfit, true)
            : OutfitManager::GetSingleton().RestoreOriginalOutfit(actor, true);
        if (!restored) {
            constexpr int kMaxAutomaticRetries = 80;
            if (_automaticRetryPending.contains(actorId)) {
                return;
            }

            auto& retryCount = _automaticRetryCounts[actorId];
            if (retryCount >= kMaxAutomaticRetries) {
                logger::warn(
                    "SituationHandler: OBody never reached a safe state for {} after {} retries; "
                    "leaving the situation uncached for a future event",
                    actor->GetDisplayFullName(),
                    kMaxAutomaticRetries);
                _automaticRetryCounts.erase(actorId);
                return;
            }

            ++retryCount;
            _automaticRetryPending.insert(actorId);
            ScheduleAutomaticRetry(
                actor->GetHandle(), actorId, std::chrono::milliseconds(250));
            return;
        }
        if (outfit) {
            logger::info("SituationHandler: applied outfit '{}' (id={}) on {} for situation {}",
                outfit->name, outfitId, actor->GetDisplayFullName(), static_cast<int>(situation));
        } else {
            OutfitManager::NotifyOutfitChanged(actor);
            logger::info("SituationHandler: restored original outfit on {} because no situation or generic outfit is eligible",
                actor->GetDisplayFullName());
        }
    }

    // --- Wig situational application ---
    if (redress.runWigStep) ApplySituationWig(actor, wigSituation);

    _automaticRetryCounts.erase(actorId);
    _automaticRetryPending.erase(actorId);
    _currentSituations[actorId] = situation;
    _appliedOutfitIds[actorId] = outfitId;
}

// The situation's wig, in Sleep the wig of the situation besides Sleep, for Home (or Sleep beside it) the
// Town wig, else the Adventuring wig; when the NPC leaves Sleep with the Sleep wig on and none of those is
// set, the assigned wig, else their own hair. Any other change without a wig keeps the one worn.
void SituationHandler::ApplySituationWig(RE::Actor* actor, OutfitSituation situation)
{
    const auto actorId = actor->GetFormID();
    auto& wigAssignments = WigAssignments::GetSingleton();
    if (!wigAssignments.HasAnySituation(actorId)) return;
    // Whether this change can end Sleep: the wig step last ran for Sleep, or hasn't run since a load or a
    // forced re-evaluation. Fights and swims change the cached situation without this step; their restores run it.
    const auto lastWig = _wigSituations.find(actorId);
    const bool fromSleep = lastWig == _wigSituations.end() || lastWig->second == OutfitSituation::Sleep;
    _wigSituations[actorId] = situation;
    auto& wigMgr = WigManager::GetSingleton();
    const auto worn = wigAssignments.GetState(actorId);
    // A dormant assigned wig (its plugin missing) can't go on: their own hair comes back instead.
    const auto assigned = worn && worn->assignedWig.Resolve() ? worn->assignedWig : WigEntry{};
    const auto choice = Tailor::Wigs::ChooseSituationWig(situation,
        situation == OutfitSituation::Sleep ? EvaluateLocationSituation(actor) : situation, fromSleep,
        // A situation wig whose plugin is missing can't go on: as a wig to put on, it counts as none, so the next
        // in line answers.
        [&](OutfitSituation slot) {
            const auto wig = wigAssignments.GetSituationWig(actorId, slot);
            // Leaving Sleep, the stored Sleep wig still names the one worn, so the Sleep-ends fallback answers even
            // when its mod is missing; every wig that could go on is still filtered.
            if (slot == OutfitSituation::Sleep && situation != OutfitSituation::Sleep) return wig;
            return wig.Resolve() ? wig : WigEntry{};
        },
        worn ? worn->currentWig : WigEntry{}, assigned);
    if (choice.step == Tailor::Wigs::SituationWigStep::Equip) {
        wigMgr.EquipWig(actor, choice.wig);
        wigMgr.ReApplyHairColor(actor);
        wigMgr.ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
        logger::info("SituationHandler: applied wig '{}' on {} for situation {}",
            choice.wig.name, actor->GetDisplayFullName(), static_cast<int>(situation));
    } else if (choice.step == Tailor::Wigs::SituationWigStep::TakeOff) {
        wigMgr.TakeOffSituationWig(actor);
        logger::info("SituationHandler: Sleep is over for {}; the Sleep wig came off for their own hair",
            actor->GetDisplayFullName());
    }
}

bool SituationHandler::WigStepDue(RE::FormID actorId, OutfitSituation situation) const
{
    const auto it = _wigSituations.find(actorId);
    return it == _wigSituations.end() || it->second != situation;
}

OutfitSituation SituationHandler::WigSituationFor(RE::Actor* actor, OutfitSituation situation) const
{
    // Only Warm needs the location, which is read, and logged, only then.
    if (situation != OutfitSituation::Warm) return situation;
    return Tailor::Wigs::WigSituation(situation, EvaluateLocationSituation(actor));
}

std::optional<OutfitSnapshot> SituationHandler::CaptureReturnOutfit(RE::Actor* actor)
{
    const auto actorId = actor->GetFormID();
    auto& mgr = OutfitManager::GetSingleton();
    // A world reload has no live selection cache. Reconstruct the saved land
    // assignment before capturing, so no stale runtime outfit is retained.
    if (!_appliedOutfitIds.contains(actorId)) {
        const auto landSituation = EvaluateSituation(actor, false);
        const int landId = ResolveOutfitForSituation(actorId, landSituation);
        const auto* landOutfit = OutfitStore::GetSingleton().GetOutfitById(landId);
        if (landOutfit ? !mgr.ApplyCustomOutfit(actor, *landOutfit, true) : !mgr.RestoreOriginalOutfit(actor, true)) {
            return std::nullopt;
        }
        _appliedOutfitIds[actorId] = landId;
    }
    auto snapshot = mgr.CaptureOutfitSnapshot(actor);
    if (snapshot) snapshot->outfitId = _appliedOutfitIds[actorId];
    return snapshot;
}

bool SituationHandler::ApplyAdventuringOverride(RE::Actor* actor, int outfitId)
{
    using namespace Tailor::Situations;
    const auto actorId = actor->GetFormID();
    if (outfitId <= 0 && !_adventuringOverrides.contains(actorId)) return false;

    auto& state = _adventuringOverrides[actorId];
    auto& mgr = OutfitManager::GetSingleton();
    std::optional<OutfitSituation> landAfterFight;  // where a successful restore put the NPC, out of water
    bool swimKeepsNote = false;  // the restore gave a swim the snapshot of a woken NPC's bed: the note stays for its end
    const auto result = state.Update(outfitId, [&]() {
        return CaptureReturnOutfit(actor);
    }, [&](int id) {
        // Suspend Swimming without losing its pre-water outfit. A later return
        // to water must equip Swimming again, even when its daily ID is unchanged.
        if (auto it = _swimmingOverrides.find(actorId); it != _swimmingOverrides.end()) {
            it->second.appliedId = 0;
        }
        const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(id);
        return outfit && mgr.ApplyCustomOutfit(actor, *outfit, true);
    }, [&](const OutfitSnapshot& previous) {
        const auto situation = EvaluateSituation(actor);
        // A mod's override they can wear comes back when the fight ends, in place of the situation's own choice, in
        // water too: it goes on directly, and a swim the fight suspended ends without its pre-water outfit. One they
        // can't wear leaves the situation's chain to dress them.
        const auto forced = ModOverrides::GetSingleton().Get(actorId);
        const bool overrideWearable = forced && WearableBy(actorId)(*forced);
        const int situationId = overrideWearable ? *forced : ModOverrides::GetSingleton().Contains(actorId)
            ? ResolveOutfitForSituation(actorId, situation) : ResolveSituationSlot(actorId, situation);
        if (situation == OutfitSituation::Swimming && situationId > 0 && !overrideWearable) {
            auto& swimming = _swimmingOverrides[actorId];
            if (!swimming.previous) swimming.previous = previous;
            // The swim now carries the fight's snapshot, for a woken NPC that of the bed: the note goes with it, and
            // the swim's end dresses them.
            swimKeepsNote = _wokenByFight.contains(actorId);
            const auto resumed = ApplySwimmingOverride(actor, true);
            return resumed == OutfitOverrideResult::Applied || resumed == OutfitOverrideResult::Unchanged;
        }

        int restoredId = situationId;
        bool restored = false;
        if (situationId <= 0 && _wokenByFight.contains(actorId)) {
            // What they wore in bed is over: their situation's chain dresses them, as ApplyForSituation would. In water
            // without a Swimming outfit, that is the land they'd be on.
            const auto dressFor = situation == OutfitSituation::Swimming ? EvaluateSituation(actor, false) : situation;
            restoredId = ResolveOutfitForSituation(actorId, dressFor);
            const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(restoredId);
            restored = outfit ? mgr.ApplyCustomOutfit(actor, *outfit, true) : mgr.RestoreOriginalOutfit(actor, true);
            if (restored && !outfit) OutfitManager::NotifyOutfitChanged(actor);
        } else if (situationId <= 0) {
            const auto water = _swimmingOverrides.find(actorId);
            // If combat ended on shore, restore the pre-water outfit, never the
            // swimsuit captured when combat interrupted the water override.
            const auto& snapshot = water != _swimmingOverrides.end() && water->second.previous
                ? *water->second.previous : previous;
            restored = mgr.RestoreOutfitSnapshot(actor, snapshot);
            restoredId = snapshot.outfitId;
        } else {
            const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(situationId);
            restored = outfit && mgr.ApplyCustomOutfit(actor, *outfit, true);
        }
        if (!restored) return false;
        _swimmingOverrides.erase(actorId);
        _currentSituations[actorId] = situation;
        if (situation != OutfitSituation::Swimming) landAfterFight = situation;
        _appliedOutfitIds[actorId] = restoredId;
        return true;
    });

    if (result == OutfitOverrideResult::Inactive) {
        _adventuringOverrides.erase(actorId);
        _wokenByFight.erase(actorId);
        return false;
    }
    if (result == OutfitOverrideResult::Applied) {
        // A fight that wakes the NPC ends their Sleep look: when it's over, they're dressed for where they are.
        if (const auto it = _currentSituations.find(actorId); it != _currentSituations.end() && it->second == OutfitSituation::Sleep) {
            if (_wokenByFight.insert(actorId).second) {
                logger::info("SituationHandler: a fight woke {}; when it's over they're dressed for where they are",
                    actor->GetDisplayFullName());
            }
        }
        _currentSituations[actorId] = OutfitSituation::Adventuring;
        _appliedOutfitIds[actorId] = outfitId;
        logger::info("SituationHandler: combat Adventuring priority applied outfit {} to {}",
            outfitId, actor->GetDisplayFullName());
    } else if (result == OutfitOverrideResult::Restored) {
        _adventuringOverrides.erase(actorId);
        // A swim that took over the snapshot of a woken NPC's bed keeps the note for its own end.
        if (!swimKeepsNote) _wokenByFight.erase(actorId);
        logger::info("SituationHandler: Adventuring priority ended for {}; restored outfit {}",
            actor->GetDisplayFullName(), _appliedOutfitIds[actorId]);
        // The restore put their outfit back without the wig step: a change of situation (waking
        // included) gets its wig now, as for the player, by the situation beneath Warm.
        if (landAfterFight) {
            const auto wigSituation = WigSituationFor(actor, *landAfterFight);
            if (WigStepDue(actorId, wigSituation)) ApplySituationWig(actor, wigSituation);
        }
    }
    // Active overrides and failed restores are retried by the game-thread poll.
    return true;
}

Tailor::Situations::OutfitOverrideResult SituationHandler::ApplySwimmingOverride(RE::Actor* actor, bool inWater)
{
    using Tailor::Situations::OutfitOverrideResult;
    const auto actorId = actor->GetFormID();
    const int swimmingId = inWater ? ResolveOutfitForSituation(actorId, OutfitSituation::Swimming) : 0;
    auto it = _swimmingOverrides.find(actorId);
    if (it == _swimmingOverrides.end() && swimmingId <= 0) {
        // Entering water without a Swimming choice must leave clothing and wigs alone.
        return inWater ? OutfitOverrideResult::Unchanged : OutfitOverrideResult::Inactive;
    }
    auto& state = _swimmingOverrides[actorId];
    auto& mgr = OutfitManager::GetSingleton();
    const int previousId = state.previous ? state.previous->outfitId : 0;
    std::optional<int> wokenLandId;  // what the end of a woken NPC's swim dressed them in, in place of the snapshot's outfit
    const auto result = state.Update(swimmingId, [&]() -> std::optional<OutfitSnapshot> {
        return CaptureReturnOutfit(actor);
    }, [&](int id) {
        const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(id);
        return outfit && mgr.ApplyCustomOutfit(actor, *outfit, true);
    }, [&](const OutfitSnapshot& snapshot) {
        // A fight that woke the NPC handed the swim its snapshot of the bed: dress them for where they are instead.
        if (!_wokenByFight.contains(actorId)) return mgr.RestoreOutfitSnapshot(actor, snapshot);
        wokenLandId = ResolveOutfitForSituation(actorId, EvaluateSituation(actor, false));
        const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(*wokenLandId);
        const bool restored = outfit ? mgr.ApplyCustomOutfit(actor, *outfit, true) : mgr.RestoreOriginalOutfit(actor, true);
        if (restored && !outfit) OutfitManager::NotifyOutfitChanged(actor);
        return restored;
    });

    if (result == OutfitOverrideResult::Inactive) {
        _swimmingOverrides.erase(actorId);
        return inWater ? OutfitOverrideResult::Unchanged : OutfitOverrideResult::Inactive;
    }
    if (result == OutfitOverrideResult::Applied) {
        _currentSituations[actorId] = OutfitSituation::Swimming;
        _appliedOutfitIds[actorId] = swimmingId;
        logger::info("Swimming: applied outfit {} to {}", swimmingId, actor->GetDisplayFullName());
    } else if (result == OutfitOverrideResult::Restored) {
        _swimmingOverrides.erase(actorId);
        _wokenByFight.erase(actorId);
        const auto land = EvaluateSituation(actor, false);
        _currentSituations[actorId] = land;
        _appliedOutfitIds[actorId] = wokenLandId.value_or(previousId);
        if (wokenLandId) {
            logger::info("Swimming: {} left the water after a fight woke them; dressed for where they are (outfit {})",
                actor->GetDisplayFullName(), *wokenLandId);
        } else {
            logger::info("Swimming: restored pre-water outfit {} on {}", previousId, actor->GetDisplayFullName());
        }
        // As after a fight: a change of situation beneath Warm gets its wig now.
        if (const auto wigSituation = WigSituationFor(actor, land); WigStepDue(actorId, wigSituation)) ApplySituationWig(actor, wigSituation);
    }
    // Retry on the existing 250 ms game-thread poll, preserving the snapshot.
    // Do not immediately overwrite a restored outfit with a new land selection.
    return result;
}

OutfitSituation SituationHandler::GetCachedSituation(RE::FormID actorId) const
{
    auto it = _currentSituations.find(actorId);
    return it != _currentSituations.end() ? it->second : OutfitSituation::Adventuring;
}

std::optional<int> SituationHandler::GetAppliedOutfitId(RE::FormID actorId) const
{
    const auto it = _appliedOutfitIds.find(actorId);
    return it != _appliedOutfitIds.end() ? std::optional{it->second} : std::nullopt;
}

bool SituationHandler::DressedBySituations(RE::FormID actorId) const
{
    return ModOverrides::GetSingleton().Contains(actorId) || OutfitAssignments::GetSingleton().HasAnySituation(actorId) ||
        _swimmingOverrides.contains(actorId) || _adventuringOverrides.contains(actorId);
}

int SituationHandler::CurrentOutfitId(RE::Actor* actor) const
{
    if (!actor) return 0;
    if (actor->IsPlayerRef()) return Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId();
    const auto actorId = actor->GetFormID();
    // What the situation flow last put on, for an NPC it dresses. Tailor's screens put a regular outfit on outside
    // it, so for anyone else what it last applied may be stale, and their regular outfit below is what they wear.
    // OutfitPending goes by the same rule.
    if (DressedBySituations(actorId)) {
        if (const auto applied = GetAppliedOutfitId(actorId)) return *applied;
    }
    // Not dressed by Tailor yet, as when they weren't loaded when it was set: an override they can wear goes
    // on as soon as it can.
    const int sex = OutfitManager::GetNpcSex(actor);
    if (const auto forced = ModOverrides::GetSingleton().Get(actorId); forced && OutfitStore::GetSingleton().Fits(*forced, sex)) {
        return *forced;
    }
    // A regular outfit alone can go on outside the situation flow, which records the rest.
    const auto* saved = OutfitAssignments::GetSingleton().GetAssignment(actorId);
    if (!saved) return 0;
    const auto assignment = *saved;
    if (assignment.HasAnySituation() || assignment.outfitId <= 0) return 0;
    return OutfitStore::GetSingleton().Fits(assignment.outfitId, sex) ? assignment.outfitId : 0;
}

bool SituationHandler::OutfitPending(RE::Actor* actor) const
{
    if (!actor || actor->IsPlayerRef()) return false;
    const auto actorId = actor->GetFormID();
    // CurrentOutfitId's rule: only an NPC the situation flow dresses waits for it, until it records what it put on.
    if (!DressedBySituations(actorId) || GetAppliedOutfitId(actorId)) return false;
    // As CurrentOutfitId: an override they can wear is known before it goes on.
    const auto forced = ModOverrides::GetSingleton().Get(actorId);
    if (forced && OutfitStore::GetSingleton().Fits(*forced, OutfitManager::GetNpcSex(actor))) return false;
    // Their situation's outfit goes on through the situation flow, which records it.
    return OutfitAssignments::GetSingleton().HasAnySituation(actorId);
}

void SituationHandler::SetCachedSituation(RE::FormID actorId, OutfitSituation situation)
{
    _currentSituations[actorId] = situation;
}

void SituationHandler::ClearCachedSituation(RE::FormID actorId)
{
    _currentSituations.erase(actorId);
    _wigSituations.erase(actorId);
    _appliedOutfitIds.erase(actorId);
    _automaticRetryCounts.erase(actorId);
}

void SituationHandler::ClearOutfitOverrides(RE::FormID actorId)
{
    // The player keeps no overrides; Reset forgets the look their situations put on.
    if (actorId == Tailor::Player::kPlayerRef) {
        _player.applied.reset();
        return;
    }
    _swimmingOverrides.erase(actorId);
    _adventuringOverrides.erase(actorId);
    _wokenByFight.erase(actorId);
    _observedWaterStates.erase(actorId);
    _observedCombatStates.erase(actorId);
    _observedColdStates.erase(actorId);
    ClearCachedSituation(actorId);
}

std::vector<RE::FormID> SituationHandler::ForgetOutfit(int outfitId)
{
    std::unordered_set<RE::FormID> actorIds;
    for (const auto& [actorId, applied] : _appliedOutfitIds) {
        if (applied == outfitId) actorIds.insert(actorId);
    }
    for (const auto* overrides : {&_swimmingOverrides, &_adventuringOverrides}) {
        for (const auto& [actorId, state] : *overrides) {
            if (state.appliedId == outfitId || (state.previous && state.previous->outfitId == outfitId)) actorIds.insert(actorId);
        }
    }
    for (const auto actorId : actorIds) ClearOutfitOverrides(actorId);
    return {actorIds.begin(), actorIds.end()};
}

void SituationHandler::ForceApplyForSituation(RE::Actor* actor)
{
    if (!actor) {
        logger::warn("ForceApplyForSituation: actor is null");
        return;
    }
    // Tailor's own actions dress the player at once: for their situation, else the regular outfit. Clearing
    // the last situation ends their state now, not at the first poll after Tailor closes, or a Sleep outfit
    // set again in this session would find the old wake-up note.
    if (actor->IsPlayerRef()) {
        if (!PlayerHasSituations()) ForgetPlayerSituationState();
        OutfitManager::GetSingleton().ReconcilePlayer();
        return;
    }
    logger::info("ForceApplyForSituation: forcing re-evaluation for {}", actor->GetDisplayFullName());
    const auto applied = _appliedOutfitIds.find(actor->GetFormID());
    const std::optional<int> previousId = applied != _appliedOutfitIds.end()
        ? std::optional{applied->second} : std::nullopt;
    ClearCachedSituation(actor->GetFormID());
    // Keep the last actual selection available for a first water snapshot.
    // Clearing the situation cache is sufficient to force normal reapplication.
    if (previousId) _appliedOutfitIds[actor->GetFormID()] = *previousId;
    if (auto it = _swimmingOverrides.find(actor->GetFormID()); it != _swimmingOverrides.end()) {
        it->second.appliedId = 0;
    }
    if (auto it = _adventuringOverrides.find(actor->GetFormID()); it != _adventuringOverrides.end()) {
        it->second.appliedId = 0;
    }
    ApplyForSituation(actor);
}

void SituationHandler::ResetForGameLoad()
{
    _sleepMonitoringEnabled.store(false);
    sSituationGeneration.fetch_add(1);
    _observedSleepStates.clear();
    _observedWaterStates.clear();
    _swimmingOverrides.clear();
    _adventuringOverrides.clear();
    _wokenByFight.clear();
    _observedCombatStates.clear();
    _observedColdStates.clear();
    _currentSituations.clear();
    _wigSituations.clear();
    _appliedOutfitIds.clear();
    _automaticRetryCounts.clear();
    _automaticRetryPending.clear();
    _randomStates.clear();
    _player = {};
    SituationWeapons::GetSingleton().ShowAll();
    // Everyone seen after the load is a first sighting, which sends no event.
    Tailor::Api::ForgetEvents();
    logger::info("SituationHandler: cleared runtime situation state for game load");
}

void SituationHandler::StartSleepMonitoring()
{
    _sleepMonitoringEnabled.store(true);
    if (_sleepMonitor.joinable()) return;

    _sleepMonitor = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (stop.stop_requested()) return;
            if (!_sleepMonitoringEnabled.load() || _sleepPollPending.exchange(true)) continue;

            const auto generation = sSituationGeneration.load();
            SKSE::GetTaskInterface()->AddTask([this, generation]() {
                _sleepPollPending.store(false);
                if (generation != sSituationGeneration.load() || !_sleepMonitoringEnabled.load()) return;
                PollSleepStates();
            });
        }
    });
}

void SituationHandler::PollSleepStates()
{
    auto* ui = RE::UI::GetSingleton();
    if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
        ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || TailorUI::GetSingleton().IsOpen()) return;
    // The player first: dressed at once, by rules of their own.
    PollPlayer();

    std::unordered_set<RE::FormID> actorIds;
    for (const auto& [id, assignment] : OutfitAssignments::GetSingleton().GetAll()) {
        if (assignment.HasAnySituation()) actorIds.insert(id);
    }
    for (const auto& [id, assignment] : WigAssignments::GetSingleton().GetAllSituational()) {
        if (assignment.HasAnySituation()) actorIds.insert(id);
    }
    for (const auto& [id, state] : _swimmingOverrides) {
        (void)state;
        actorIds.insert(id);  // Finish restoration even after the Swimming assignment is cleared.
    }
    for (const auto& [id, state] : _adventuringOverrides) {
        (void)state;
        actorIds.insert(id);  // Finish combat restoration after assignments are cleared.
    }
    // People a mod's override dresses, managed or not: their fights must be seen.
    for (const auto id : ModOverrides::GetSingleton().Actors()) actorIds.insert(id);
    // Children are never polled: no situation outfit, Hide Weapons, Hide Helmets or change event for them. Their
    // release happens on the other paths, since nothing the poll calls may log every poll.
    std::erase_if(actorIds, [](RE::FormID id) {
        const auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        return actor && actor->IsChild();
    });

    std::erase_if(_observedSleepStates, [&actorIds](const auto& entry) {
        return !actorIds.contains(entry.first);
    });
    std::erase_if(_observedWaterStates, [&actorIds](const auto& entry) {
        return !actorIds.contains(entry.first);
    });
    std::erase_if(_observedCombatStates, [&actorIds](const auto& entry) {
        return !actorIds.contains(entry.first);
    });
    std::erase_if(_observedColdStates, [&actorIds](const auto& entry) {
        return !actorIds.contains(entry.first);
    });
    // The weather turns for everyone at once: their re-dresses are spread 50 ms apart.
    std::vector<RE::ActorHandle> coldChanges;
    for (auto id : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef() || actor->IsDead()) {
            _observedSleepStates.erase(id);
            _observedWaterStates.erase(id);
            _observedCombatStates.erase(id);
            _observedColdStates.erase(id);
            continue;
        }
        const bool inCombat = actor->IsInCombat();
        auto [combatIt, combatInserted] = _observedCombatStates.try_emplace(id, inCombat);
        const bool combatChanged = combatInserted || combatIt->second != inCombat;
        combatIt->second = inCombat;

        const bool sleeping = IsSleepSituation(actor);
        const bool inWater = Tailor::Situations::IsInWater(
            actor->GetWaterHeight(), actor->GetPosition().z, actor->AsActorState()->IsSwimming());
        auto [waterIt, waterInserted] = _observedWaterStates.try_emplace(id, inWater);
        const bool wasInWater = waterInserted ? GetCachedSituation(id) == OutfitSituation::Swimming : waterIt->second;
        waterIt->second = inWater;
        auto [it, inserted] = _observedSleepStates.try_emplace(id, sleeping);
        const bool wasSleeping = inserted ? GetCachedSituation(id) == OutfitSituation::Sleep : it->second;
        it->second = sleeping;
        // Outdoors in the cold, held while a swim or a fight lasts (Tailor::Situations::ObserveCold).
        const bool cold = Tailor::Situations::IsColdOutdoors(actor);
        const auto seenCold = _observedColdStates.find(id);
        const auto coldSeen = Tailor::Situations::ObserveCold(
            seenCold != _observedColdStates.end() ? std::optional{seenCold->second} : std::nullopt,
            GetCachedSituation(id) == OutfitSituation::Warm, cold,
            _swimmingOverrides.contains(id) || _adventuringOverrides.contains(id));
        if (coldSeen.record) _observedColdStates[id] = cold;
        if (wasSleeping != sleeping || wasInWater != inWater || _swimmingOverrides.contains(id) ||
            combatChanged || _adventuringOverrides.contains(id)) {
            if (combatChanged) {
                logger::info("SituationHandler: {} combat state={}", actor->GetDisplayFullName(), inCombat);
            }
            if (wasSleeping != sleeping) {
                logger::info("SituationHandler: {} sleep transition {} -> {} (nativeState={})",
                    actor->GetDisplayFullName(), wasSleeping, sleeping, static_cast<int>(Tailor::Situations::ReadSleepState(actor)));
            }
            if (wasInWater != inWater) {
                logger::info("Swimming: {} water transition {} -> {}", actor->GetDisplayFullName(), wasInWater, inWater);
            }
            ApplyForSituation(actor);
        } else if (coldSeen.changed) {
            logger::info("Warm: {} {} outdoors in the cold", actor->GetDisplayFullName(), cold ? "is" : "is no longer");
            coldChanges.push_back(actor->GetHandle());
        }
    }
    long long delay = 0;
    for (const auto handle : coldChanges) {
        ScheduleDelayedActorEval(handle, std::chrono::milliseconds(delay));
        delay += 50;
    }
    UpdateSituationWeapons(actorIds);
    UpdateSituationHelmets(actorIds);
    // Last, so the events tell what this poll left everyone in.
    Tailor::Api::PollEvents(actorIds);
}

void SituationHandler::EvaluateAllAssignedActors()
{
    auto allAssignments = OutfitAssignments::GetSingleton().GetAll();

    // Collect all actors that need evaluation
    std::vector<RE::ActorHandle> batch;
    int total = 0;

    for (auto& [actorId, sa] : allAssignments) {
        if (!sa.HasAnySituation()) continue;
        total++;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef()) continue;

        batch.push_back(actor->GetHandle());
    }

    // Also evaluate actors with wig-only situational assignments
    auto allWigSituations = WigAssignments::GetSingleton().GetAllSituational();
    for (auto& [actorId, wsa] : allWigSituations) {
        if (!wsa.HasAnySituation()) continue;
        if (allAssignments.count(actorId) && allAssignments[actorId].HasAnySituation()) continue;
        total++;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef()) continue;

        batch.push_back(actor->GetHandle());
    }

    // And people a mod's override dresses, managed or not, once each.
    for (const auto actorId : ModOverrides::GetSingleton().Actors()) {
        if (allAssignments.count(actorId) && allAssignments[actorId].HasAnySituation()) continue;
        if (allWigSituations.count(actorId) && allWigSituations[actorId].HasAnySituation()) continue;
        total++;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef() || actor->IsDead()) continue;

        batch.push_back(actor->GetHandle());
    }

    if (batch.empty()) {
        if (total > 0) {
            logger::info("EvaluateAllAssignedActors: 0/{} actors loaded", total);
        }
        return;
    }

    // Stagger evaluations to avoid overwhelming SKEE body morph pipeline
    constexpr int32_t kStaggerMs = 50;
    const auto generation = sSituationGeneration.load();

    std::thread([batch = std::move(batch), total, kStaggerMs, generation]() {
        for (size_t i = 0; i < batch.size(); i++) {
            if (generation != sSituationGeneration.load()) return;
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kStaggerMs));
            }
            auto handle = batch[i];
            SKSE::GetTaskInterface()->AddTask([handle, generation]() {
                if (generation != sSituationGeneration.load()) return;
                auto ptr = handle.get();
                if (!ptr) return;
                SituationHandler::GetSingleton()->ApplyForSituation(ptr.get());
            });
        }

        logger::info("EvaluateAllAssignedActors: staggered {}/{} actors ({}ms apart)",
            batch.size(), total, kStaggerMs);
    }).detach();
}

RE::BSEventNotifyControl SituationHandler::ProcessEvent(
    const RE::TESActorLocationChangeEvent* event,
    RE::BSTEventSource<RE::TESActorLocationChangeEvent>*)
{
    if (!event) return RE::BSEventNotifyControl::kContinue;

    auto* ref = event->actor.get();
    if (!ref) return RE::BSEventNotifyControl::kContinue;

    auto* actor = ref->As<RE::Actor>();
    if (!actor) return RE::BSEventNotifyControl::kContinue;

    if (actor->IsPlayerRef()) {
        // Player changed location — re-evaluate ALL actors with situational assignments
        auto* loc = actor->GetCurrentLocation();
        logger::info("SituationHandler: player location changed to '{}'",
            loc ? loc->GetFullName() : "null");
        const auto generation = sSituationGeneration.load();
        SKSE::GetTaskInterface()->AddTask([generation]() {
            if (generation != sSituationGeneration.load()) return;
            // The player is dressed at once; NPCs keep their staggered queue.
            auto* handler = SituationHandler::GetSingleton();
            handler->ApplyForSituation(RE::PlayerCharacter::GetSingleton());
            handler->EvaluateAllAssignedActors();
        });
        // Delayed re-evaluation for followers that haven't loaded 3D yet
        ScheduleDelayedEval(std::chrono::seconds(3));
        return RE::BSEventNotifyControl::kContinue;
    }

    if (!OutfitAssignments::GetSingleton().HasAnySituation(actor->GetFormID()) &&
        !WigAssignments::GetSingleton().HasAnySituation(actor->GetFormID())) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto handle = actor->GetHandle();
    const auto generation = sSituationGeneration.load();
    SKSE::GetTaskInterface()->AddTask([handle, generation]() {
        if (generation != sSituationGeneration.load()) return;
        auto actorPtr = handle.get();
        if (!actorPtr) return;
        auto* a = actorPtr.get();
        if (!a->Is3DLoaded()) return;

        SituationHandler::GetSingleton()->ApplyForSituation(a);
    });

    return RE::BSEventNotifyControl::kContinue;
}

void SituationHandler::ScheduleDelayedEval(std::chrono::milliseconds delay)
{
    const auto generation = sSituationGeneration.load();
    std::thread([delay, generation]() {
        std::this_thread::sleep_for(delay);
        SKSE::GetTaskInterface()->AddTask([generation]() {
            if (generation != sSituationGeneration.load()) return;
            SituationHandler::GetSingleton()->EvaluateAllAssignedActors();
        });
    }).detach();
}

void SituationHandler::ScheduleDelayedActorEval(RE::ActorHandle handle, std::chrono::milliseconds delay)
{
    const auto generation = sSituationGeneration.load();
    std::thread([handle, delay, generation]() {
        std::this_thread::sleep_for(delay);
        SKSE::GetTaskInterface()->AddTask([handle, generation]() {
            if (generation != sSituationGeneration.load()) return;
            auto ptr = handle.get();
            if (!ptr) return;
            auto* actor = ptr.get();
            if (!actor->Is3DLoaded()) return;
            SituationHandler::GetSingleton()->ApplyForSituation(actor);
        });
    }).detach();
}

void SituationHandler::ScheduleAutomaticRetry(
    RE::ActorHandle handle,
    RE::FormID actorId,
    std::chrono::milliseconds delay)
{
    const auto generation = sSituationGeneration.load();
    std::thread([handle, actorId, delay, generation]() {
        std::this_thread::sleep_for(delay);
        SKSE::GetTaskInterface()->AddTask([handle, actorId, generation]() {
            if (generation != sSituationGeneration.load()) return;

            auto* handler = SituationHandler::GetSingleton();
            handler->_automaticRetryPending.erase(actorId);
            auto ptr = handle.get();
            if (!ptr) return;

            auto* actor = ptr.get();
            if (actor->Is3DLoaded()) {
                handler->ApplyForSituation(actor);
            }
        });
    }).detach();
}

RE::BSEventNotifyControl SituationHandler::ProcessEvent(
    const RE::TESFurnitureEvent* event,
    RE::BSTEventSource<RE::TESFurnitureEvent>*)
{
    if (!event) return RE::BSEventNotifyControl::kContinue;

    auto* ref = event->actor.get();
    if (!ref) return RE::BSEventNotifyControl::kContinue;

    auto* actor = ref->As<RE::Actor>();
    // The player's bed: their situation is re-read on the game thread when they get in, and
    // 2 seconds after they get up, as for NPCs; the poll also sees the sleep state.
    if (actor && actor->IsPlayerRef()) {
        if (event->type == RE::TESFurnitureEvent::FurnitureEventType::kEnter) {
            const auto generation = sSituationGeneration.load();
            SKSE::GetTaskInterface()->AddTask([generation]() {
                if (generation != sSituationGeneration.load()) return;
                SituationHandler::GetSingleton()->ApplyForSituation(RE::PlayerCharacter::GetSingleton());
            });
        } else {
            ScheduleDelayedActorEval(actor->GetHandle(), std::chrono::seconds(2));
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    if (!actor || actor->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;

    auto formId = actor->GetFormID();
    if (!OutfitAssignments::GetSingleton().HasAnySituation(formId) &&
        !WigAssignments::GetSingleton().HasAnySituation(formId)) {
        return RE::BSEventNotifyControl::kContinue;
    }

    bool entering = (event->type == RE::TESFurnitureEvent::FurnitureEventType::kEnter);
    auto sleepState = Tailor::Situations::ReadSleepState(actor);
    logger::info("SituationHandler: {} {} furniture (sleepState={})",
        actor->GetDisplayFullName(), entering ? "entered" : "exited",
        static_cast<int>(sleepState));

    auto handle = actor->GetHandle();
    if (entering) {
        // Re-read current state on the game thread. A queued event must not force
        // Sleep after an interrupted entry, or duplicate a swap made by the poll.
        const auto generation = sSituationGeneration.load();
        SKSE::GetTaskInterface()->AddTask([handle, generation]() {
            if (generation != sSituationGeneration.load()) return;
            auto ptr = handle.get();
            if (!ptr) return;
            auto* a = ptr.get();
            if (!a->Is3DLoaded() || a->IsDead()) return;
            SituationHandler::GetSingleton()->ApplyForSituation(a);
        });
    } else {
        // Retain the delayed exit check; polling also catches long/custom exits.
        ScheduleDelayedActorEval(handle, std::chrono::seconds(2));
    }
    return RE::BSEventNotifyControl::kContinue;
}

// --- Sleep/Wait menu close — re-roll randomized situations after time skip ---
//
// The day-change check inside ApplyForSituation only fires when the situation
// is re-evaluated (location change, cell load, furniture event). The wait/sleep
// menu skips game days but doesn't trigger any of those events on its own, so
// randomized outfits would only re-roll on the next location transition.
// Listening for the SleepWaitMenu close lets us re-evaluate immediately after
// the time skip completes.
RE::BSEventNotifyControl SituationHandler::ProcessEvent(
    const RE::MenuOpenCloseEvent* event,
    RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
    if (!event) return RE::BSEventNotifyControl::kContinue;
    if (event->opening) return RE::BSEventNotifyControl::kContinue;

    // "Sleep/Wait Menu" matches RE::SleepWaitMenu::MENU_NAME
    if (event->menuName != RE::SleepWaitMenu::MENU_NAME) {
        return RE::BSEventNotifyControl::kContinue;
    }

    logger::info("SituationHandler: Sleep/Wait menu closed, re-evaluating assignments");

    // Defer to the SKSE task thread; the menu close fires on the UI thread
    // and we need game-thread access to actor data.
    const auto generation = sSituationGeneration.load();
    SKSE::GetTaskInterface()->AddTask([generation]() {
        if (generation != sSituationGeneration.load()) return;
        auto* handler = SituationHandler::GetSingleton();
        handler->ApplyForSituation(RE::PlayerCharacter::GetSingleton());
        handler->EvaluateAllAssignedActors();
    });

    return RE::BSEventNotifyControl::kContinue;
}

// --- The player's sleep ends ---
//
// Vanilla beds never put the player into bed, so the bed check can't see them sleep: they wake up in
// their Sleep outfit instead (SituationHandlerPlayer.cpp). Only the player sleeps through this event.
RE::BSEventNotifyControl SituationHandler::ProcessEvent(
    const RE::TESSleepStopEvent* event,
    RE::BSTEventSource<RE::TESSleepStopEvent>*)
{
    if (!event) return RE::BSEventNotifyControl::kContinue;
    const auto generation = sSituationGeneration.load();
    SKSE::GetTaskInterface()->AddTask([generation]() {
        if (generation != sSituationGeneration.load()) return;
        SituationHandler::GetSingleton()->PlayerWokeUp();
    });
    return RE::BSEventNotifyControl::kContinue;
}

// --- Weapons in the Sleep and Swimming looks, and Hide Weapons ---
//
// The look hides weapons, shields, quivers and torches while it is on; Hide Weapons hides all but the
// torch outside Adventuring and combat. Shown in combat, whenever the weapons aren't sheathed, and as
// soon as neither applies. A model the game rebuilds is hidden again at the next poll.
void SituationHandler::UpdateSituationWeapons(const std::unordered_set<RE::FormID>& actorIds)
{
    auto& weapons = SituationWeapons::GetSingleton();
    const bool hideWeapons = PreferenceStore::GetSingleton().Get().hideWeapons;
    std::unordered_set<RE::FormID> updated;
    const auto update = [&](RE::Actor* actor, bool lookOn) {
        auto* state = actor->AsActorState();
        const bool inCombat = actor->IsInCombat();
        const bool setting = hideWeapons && Tailor::Situations::HiddenOutsideAdventuring(HidePlace(actor), inCombat);
        const bool hidden = Tailor::Situations::WeaponsHidden(lookOn || setting, inCombat,
            state && !Tailor::Situations::WeaponsPutAway(state->GetWeaponState()));
        weapons.Update(actor, !hidden ? SituationWeapons::Set::None :
            lookOn ? SituationWeapons::Set::WithTorch : SituationWeapons::Set::WithoutTorch);
        updated.insert(actor->GetFormID());
    };
    if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded() && !player->IsDead()) {
        update(player, PlayerWeaponsLookOn());
    }
    for (const auto id : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef() || actor->IsDead()) continue;
        update(actor, NpcWeaponsLookOn(id));
    }
    weapons.ShowAllExcept(updated);
}

OutfitSituation SituationHandler::HidePlace(RE::Actor* actor) const
{
    // Quiet: the poll asks every 250 ms; the evaluators log for changes of dress, not for this.
    const auto situation = actor->IsPlayerRef() ? EvaluateSituation(actor, true, true) : GetCachedSituation(actor->GetFormID());
    // As WigSituationFor, quietly: under Warm, the location beneath it.
    return situation == OutfitSituation::Warm ? EvaluateLocationSituation(actor, true) : situation;
}

// --- Hide Helmets ---
//
// Outside Adventuring and combat the player's and polled NPCs' helmets and hoods come off; the same
// copies go back on for adventuring, a fight, or the setting turned off. Anyone else Tailor took a
// helmet off (their situations since cleared) gets it back once loaded.
void SituationHandler::UpdateSituationHelmets(const std::unordered_set<RE::FormID>& actorIds)
{
    auto& helmets = SituationHelmets::GetSingleton();
    const bool hideHelmets = PreferenceStore::GetSingleton().Get().hideHelmets;
    // The look a helmet comes off and goes back with: the outfit worn now, for the player their own gear as 0.
    const auto look = [this](RE::Actor* actor) {
        return actor->IsPlayerRef() ? Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId() :
            GetAppliedOutfitId(actor->GetFormID()).value_or(0);
    };
    std::unordered_set<RE::FormID> updated;
    const auto update = [&](RE::Actor* actor) {
        helmets.Update(actor, hideHelmets && Tailor::Situations::HiddenOutsideAdventuring(HidePlace(actor), actor->IsInCombat()),
            look(actor));
        updated.insert(actor->GetFormID());
    };
    // The player only while Tailor may dress them: loaded, alive and not in beast form.
    if (auto* player = RE::PlayerCharacter::GetSingleton(); Tailor::Player::CanDress(player)) update(player);
    for (const auto id : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef() || actor->IsDead()) continue;
        update(actor);
    }
    for (const auto id : helmets.Actors()) {
        if (updated.contains(id)) continue;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        // Not found is not gone: a non-persistent reference whose cell unloaded comes back with it, so it is kept.
        if (!actor) continue;
        // Dead: nobody to put a helmet back on, so the ledger lets them go.
        if (actor->IsDead()) {
            helmets.Forget(id);
            continue;
        }
        if (!actor->Is3DLoaded()) continue;
        if (actor->IsPlayerRef() && !Tailor::Player::CanDress(actor)) continue;
        helmets.Update(actor, false, look(actor));
    }
}

bool SituationHandler::HasSleepLook(RE::FormID actorId) const
{
    return HasSleepOutfit(actorId) ||
        WigAssignments::GetSingleton().GetSituationWig(actorId, OutfitSituation::Sleep).Resolve() != nullptr;
}

bool SituationHandler::HasSleepOutfit(RE::FormID actorId) const
{
    const auto* assignment = OutfitAssignments::GetSingleton().GetAssignment(actorId);
    if (!assignment) return false;
    // A random pool counts when it holds an outfit the actor can wear: checking never rolls a pick.
    if (assignment->GetRandomFlag(OutfitSituation::Sleep)) return !RandomPool(actorId, OutfitSituation::Sleep).empty();
    const int slot = assignment->GetSlot(OutfitSituation::Sleep);
    return slot > 0 && WearableBy(actorId)(slot);
}

std::optional<bool> SituationHandler::OverrideLook(RE::FormID actorId, int wornOutfitId) const
{
    const auto& overrides = ModOverrides::GetSingleton();
    return Tailor::Api::OverrideWeaponsLook(overrides.Get(actorId), overrides.SituationOf(actorId).value_or(0), wornOutfitId);
}

bool SituationHandler::NpcWeaponsLookOn(RE::FormID actorId) const
{
    if (const auto overrideLook = OverrideLook(actorId, GetAppliedOutfitId(actorId).value_or(0))) return *overrideLook;
    if (const auto it = _swimmingOverrides.find(actorId); it != _swimmingOverrides.end() && it->second.appliedId > 0) return true;
    return GetCachedSituation(actorId) == OutfitSituation::Sleep && HasSleepLook(actorId);
}
