#include "events/SituationHandler.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitStore.h"
#include "wig/WigAssignments.h"
#include "wig/WigManager.h"
#include <cmath>
#include <thread>
#include <chrono>

namespace
{
    std::atomic<std::uint64_t> sSituationGeneration{1};
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
        logger::info("SituationHandler registered for location change + furniture events");
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
    _kwCity        = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013168);  // LocTypeCity
    _kwTown        = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013166);  // LocTypeTown
    _kwSettlement  = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013167);  // LocTypeSettlement
    _kwDwelling    = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000130DC);  // LocTypeDwelling
    _kwInn         = RE::TESForm::LookupByID<RE::BGSKeyword>(0x0001CB87);  // LocTypeInn

    logger::info("SituationHandler: keywords initialized (PlayerHouse={}, City={}, Town={}, Settlement={}, Dwelling={}, Inn={})",
        _kwPlayerHouse != nullptr, _kwCity != nullptr, _kwTown != nullptr,
        _kwSettlement != nullptr, _kwDwelling != nullptr, _kwInn != nullptr);
}

OutfitSituation SituationHandler::EvaluateSituation(RE::Actor* actor) const
{
    if (!actor) return OutfitSituation::Adventuring;

    // Priority 1: Sleep — check furniture occupation first (most reliable; immune to
    // package-state issues caused by gameplay overhauls). Falls back
    // to GetSitSleepState() for edge cases where the actor reports sleep state without
    // an occupied furniture handle.
    if (auto furnHandle = actor->GetOccupiedFurniture(); furnHandle) {
        if (auto furnRef = furnHandle.get()) {
            if (auto* base = furnRef->GetBaseObject()) {
                if (auto* furn = base->As<RE::TESFurniture>()) {
                    if (furn->furnFlags.all(RE::TESFurniture::ActiveMarker::kCanSleep)) {
                        logger::info("EvaluateSituation: {} occupying bed '{}' → Sleep",
                            actor->GetDisplayFullName(), furn->GetFullName());
                        return OutfitSituation::Sleep;
                    }
                }
            }
        }
    }

    auto sleepState = actor->GetSitSleepState();
    if (sleepState == RE::SIT_SLEEP_STATE::kIsSleeping ||
        sleepState == RE::SIT_SLEEP_STATE::kWaitingForSleepAnim ||
        sleepState == RE::SIT_SLEEP_STATE::kWantToWake) {
        logger::info("EvaluateSituation: {} sleepState={} → Sleep",
            actor->GetDisplayFullName(), static_cast<int>(sleepState));
        return OutfitSituation::Sleep;
    }

    auto* location = actor->GetCurrentLocation();
    if (!location) {
        logger::info("EvaluateSituation: {} has no location → Adventuring",
            actor->GetDisplayFullName());
        return OutfitSituation::Adventuring;
    }

    // Priority 2: Home (player house)
    if (_kwPlayerHouse && location->HasKeyword(_kwPlayerHouse)) {
        logger::info("EvaluateSituation: {} at '{}' → Home",
            actor->GetDisplayFullName(), location->GetFullName());
        return OutfitSituation::Home;
    }

    // Priority 3: Town
    if ((_kwCity && location->HasKeyword(_kwCity)) ||
        (_kwTown && location->HasKeyword(_kwTown)) ||
        (_kwSettlement && location->HasKeyword(_kwSettlement)) ||
        (_kwDwelling && location->HasKeyword(_kwDwelling)) ||
        (_kwInn && location->HasKeyword(_kwInn))) {
        logger::info("EvaluateSituation: {} at '{}' → Town",
            actor->GetDisplayFullName(), location->GetFullName());
        return OutfitSituation::Town;
    }

    // Priority 4: Adventuring (fallback)
    logger::info("EvaluateSituation: {} at '{}' → Adventuring (no keyword match)",
        actor->GetDisplayFullName(), location->GetFullName());
    return OutfitSituation::Adventuring;
}

// --- Randomized outfit helpers ---

float SituationHandler::GetGameDaysPassed()
{
    auto* global = RE::TESForm::LookupByID<RE::TESGlobal>(0x39);  // GameDaysPassed
    return global ? global->value : 0.0f;
}

int SituationHandler::ResolveRandomOutfit(RE::FormID actorId, OutfitSituation situation)
{
    static const char* sitTypeMap[] = { "", "adventuring", "town", "home", "sleep" };
    int sitIdx = static_cast<int>(situation);
    if (sitIdx < 1 || sitIdx > 4) return 0;

    // Sex-specific pool — v2.0+
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
    if (!actor) return 0;
    auto* npc = actor->GetActorBase();
    if (!npc) return 0;
    std::string sex = (npc->GetSex() == RE::SEX::kFemale) ? "female" : "male";

    auto* cat = OutfitLibrary::GetSingleton().GetCategoryBySituationType(sitTypeMap[sitIdx], sex);
    if (!cat || cat->outfitIds.empty()) return 0;

    auto& rs = _randomStates[actorId].slots[sitIdx - 1];
    float currentDay = GetGameDaysPassed();

    if (std::floor(currentDay) != std::floor(rs.lastRandomDay) || rs.lastRandomOutfitId == 0) {
        int previous = rs.lastRandomOutfitId;
        int poolSize = static_cast<int>(cat->outfitIds.size());
        std::uniform_int_distribution<int> dist(0, poolSize - 1);
        int picked = cat->outfitIds[dist(_rng)];

        // Avoid repeating the previous outfit when the pool has more than one option.
        // Without this the visual change is invisible to the user when the RNG repeats.
        if (poolSize > 1 && picked == previous) {
            int tries = 0;
            while (picked == previous && tries < 8) {
                picked = cat->outfitIds[dist(_rng)];
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

int SituationHandler::ResolveOutfitForSituation(RE::FormID actorId, OutfitSituation situation)
{
    auto& assignments = OutfitAssignments::GetSingleton();
    bool isRandom = assignments.GetSituationRandom(actorId, situation);

    int outfitId = 0;
    if (isRandom) {
        outfitId = ResolveRandomOutfit(actorId, situation);
    } else {
        outfitId = assignments.GetSituationOutfitId(actorId, situation);
    }

    // Fallback to Adventuring if no outfit for current situation
    if (outfitId <= 0 && situation != OutfitSituation::Adventuring) {
        bool advRandom = assignments.GetSituationRandom(actorId, OutfitSituation::Adventuring);
        if (advRandom) {
            outfitId = ResolveRandomOutfit(actorId, OutfitSituation::Adventuring);
        } else {
            outfitId = assignments.GetSituationOutfitId(actorId, OutfitSituation::Adventuring);
        }
        if (outfitId > 0) {
            logger::info("ApplyForSituation: no outfit for situation {}, falling back to Adventuring",
                static_cast<int>(situation));
        }
    }

    return outfitId;
}

void SituationHandler::ApplyForSituation(RE::Actor* actor)
{
    if (!actor) return;

    auto& assignments = OutfitAssignments::GetSingleton();
    auto& wigAssignments = WigAssignments::GetSingleton();
    auto actorId = actor->GetFormID();

    bool hasOutfitSituations = assignments.HasAnySituation(actorId);
    bool hasWigSituations = wigAssignments.HasAnySituation(actorId);
    if (!hasOutfitSituations && !hasWigSituations) {
        return;
    }

    if (actor->IsInCombat()) {
        logger::info("ApplyForSituation: {} is in combat, skipping", actor->GetDisplayFullName());
        return;
    }

    auto situation = EvaluateSituation(actor);

    auto it = _currentSituations.find(actorId);
    if (it != _currentSituations.end() && it->second == situation) {
        // Same situation — but check if randomize needs a day-change re-roll
        bool anyRandom = assignments.GetSituationRandom(actorId, situation);
        if (!anyRandom) return;  // Not random, same situation, skip

        float currentDay = GetGameDaysPassed();
        int sitIdx = static_cast<int>(situation) - 1;
        auto rit = _randomStates.find(actorId);
        if (rit != _randomStates.end() && sitIdx >= 0 && sitIdx < 4) {
            if (std::floor(currentDay) == std::floor(rit->second.slots[sitIdx].lastRandomDay)) {
                return;  // Same day, skip
            }
        }
        // Day changed with random on — fall through to re-apply
    }

    // --- Outfit situational application ---
    if (hasOutfitSituations) {
        int outfitId = ResolveOutfitForSituation(actorId, situation);
        if (outfitId > 0) {
            auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
            if (outfit) {
                if (!OutfitManager::GetSingleton().ApplyCustomOutfit(actor, *outfit, true)) {
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
                logger::info("SituationHandler: applied outfit '{}' (id={}) on {} for situation {}",
                    outfit->name, outfitId, actor->GetDisplayFullName(), static_cast<int>(situation));
            } else {
                logger::warn("ApplyForSituation: outfit id {} not found for {} (situation {})",
                    outfitId, actor->GetDisplayFullName(), static_cast<int>(situation));
            }
        }
    }

    // --- Wig situational application ---
    if (hasWigSituations) {
        auto wigEntry = wigAssignments.GetSituationWig(actorId, situation);
        if (wigEntry.formId == 0 && situation != OutfitSituation::Adventuring) {
            wigEntry = wigAssignments.GetSituationWig(actorId, OutfitSituation::Adventuring);
        }
        if (wigEntry.formId != 0) {
            auto& wigMgr = WigManager::GetSingleton();
            wigMgr.EquipWig(actor, wigEntry);
            wigMgr.ReApplyHairColor(actor);
            wigMgr.ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
            logger::info("SituationHandler: applied wig '{}' on {} for situation {}",
                wigEntry.name, actor->GetDisplayFullName(), static_cast<int>(situation));
        }
    }

    _automaticRetryCounts.erase(actorId);
    _automaticRetryPending.erase(actorId);
    _currentSituations[actorId] = situation;
}

OutfitSituation SituationHandler::GetCachedSituation(RE::FormID actorId) const
{
    auto it = _currentSituations.find(actorId);
    return it != _currentSituations.end() ? it->second : OutfitSituation::Adventuring;
}

void SituationHandler::SetCachedSituation(RE::FormID actorId, OutfitSituation situation)
{
    _currentSituations[actorId] = situation;
}

void SituationHandler::ClearCachedSituation(RE::FormID actorId)
{
    _currentSituations.erase(actorId);
    _automaticRetryCounts.erase(actorId);
}

void SituationHandler::ForceApplyForSituation(RE::Actor* actor)
{
    if (!actor) {
        logger::warn("ForceApplyForSituation: actor is null");
        return;
    }
    logger::info("ForceApplyForSituation: forcing re-evaluation for {}", actor->GetDisplayFullName());
    ClearCachedSituation(actor->GetFormID());
    ApplyForSituation(actor);
}

void SituationHandler::ResetForGameLoad()
{
    sSituationGeneration.fetch_add(1);
    _currentSituations.clear();
    _automaticRetryCounts.clear();
    _automaticRetryPending.clear();
    _randomStates.clear();
    logger::info("SituationHandler: cleared runtime situation state for game load");
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
            SituationHandler::GetSingleton()->EvaluateAllAssignedActors();
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
            SituationHandler::GetSingleton()->ClearCachedSituation(actor->GetFormID());
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
    if (!actor || actor->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;

    auto formId = actor->GetFormID();
    if (!OutfitAssignments::GetSingleton().HasAnySituation(formId) &&
        !WigAssignments::GetSingleton().HasAnySituation(formId)) {
        return RE::BSEventNotifyControl::kContinue;
    }

    bool entering = (event->type == RE::TESFurnitureEvent::FurnitureEventType::kEnter);
    auto sleepState = actor->GetSitSleepState();
    logger::info("SituationHandler: {} {} furniture (sleepState={})",
        actor->GetDisplayFullName(), entering ? "entered" : "exited",
        static_cast<int>(sleepState));

    // Check if the furniture itself is a bed (kCanSleep flag)
    bool isBed = false;
    if (auto* furnRef = event->targetFurniture.get()) {
        if (auto* baseObj = furnRef->GetBaseObject()) {
            if (auto* furn = baseObj->As<RE::TESFurniture>()) {
                isBed = furn->furnFlags.all(RE::TESFurniture::ActiveMarker::kCanSleep);
            }
        }
    }

    auto handle = actor->GetHandle();
    if (entering && (sleepState == RE::SIT_SLEEP_STATE::kWantToSleep || isBed)) {
        // NPC entering bed — apply sleep outfit/wig directly
        // Check both kWantToSleep state and furniture kCanSleep flag for robustness
        const auto generation = sSituationGeneration.load();
        SKSE::GetTaskInterface()->AddTask([handle, generation]() {
            if (generation != sSituationGeneration.load()) return;
            auto ptr = handle.get();
            if (!ptr) return;
            auto* a = ptr.get();
            auto id = a->GetFormID();

            // Sleep outfit (supports randomize)
            int sleepOutfitId = SituationHandler::GetSingleton()->ResolveOutfitForSituation(id, OutfitSituation::Sleep);
            if (sleepOutfitId > 0) {
                auto* outfit = OutfitStore::GetSingleton().GetOutfitById(sleepOutfitId);
                if (outfit) {
                    if (!OutfitManager::GetSingleton().ApplyCustomOutfit(a, *outfit, true)) {
                        auto* handler = SituationHandler::GetSingleton();
                        handler->ClearCachedSituation(id);
                        handler->ApplyForSituation(a);
                        return;
                    }
                    logger::info("SituationHandler: applied sleep outfit '{}' on {} (furniture + kWantToSleep)",
                        outfit->name, a->GetDisplayFullName());
                }
            }

            // Sleep wig
            auto& wigAssignments = WigAssignments::GetSingleton();
            if (wigAssignments.HasAnySituation(id)) {
                auto sleepWig = wigAssignments.GetSituationWig(id, OutfitSituation::Sleep);
                if (sleepWig.formId != 0) {
                    auto& wigMgr = WigManager::GetSingleton();
                    wigMgr.EquipWig(a, sleepWig);
                    wigMgr.ReApplyHairColor(a);
                    wigMgr.ScheduleActorHairRetint(a->GetHandle(), {1, 5, 12});
                    logger::info("SituationHandler: applied sleep wig '{}' on {} (furniture)",
                        sleepWig.name, a->GetDisplayFullName());
                }
            }

            SituationHandler::GetSingleton()->SetCachedSituation(id, OutfitSituation::Sleep);
        });
    } else {
        // Non-sleep furniture enter, or furniture exit — re-evaluate after delay
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
        SituationHandler::GetSingleton()->EvaluateAllAssignedActors();
    });

    return RE::BSEventNotifyControl::kContinue;
}
