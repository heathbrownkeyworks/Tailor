#include "events/SituationHandler.h"
#include "events/RandomOutfitPolicy.h"
#include "events/SleepStatePolicy.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitStore.h"
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

    // Priority 1: sleep intent, entry, sleep and waking; no idle names required.
    if (IsSleepSituation(actor)) {
        logger::info("EvaluateSituation: {} sleepState={} → Sleep",
            actor->GetDisplayFullName(), static_cast<int>(Tailor::Situations::ReadSleepState(actor)));
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

    auto& library = OutfitLibrary::GetSingleton();
    const auto type = situation == OutfitSituation::Adventuring
        ? OutfitAssignments::GetSingleton().GetAdventuringArmorType(actorId) : OutfitArmorType::Any;
    auto pool = library.FilterByArmorType(library.GetSituationOutfitIds(sitTypeMap[sitIdx]), type);
    std::erase_if(pool, [](int id) { return !OutfitStore::GetSingleton().GetOutfitById(id); });
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

int SituationHandler::ResolveOutfitForSituation(RE::FormID actorId, OutfitSituation situation)
{
    const auto* saved = OutfitAssignments::GetSingleton().GetAssignment(actorId);
    if (!saved) return 0;
    const auto assignment = *saved;
    return assignment.ResolveOutfit(situation, [this, actorId](OutfitSituation slot) {
        return ResolveRandomOutfit(actorId, slot);
    }, [&](int outfitId) {
        return OutfitStore::GetSingleton().GetOutfitById(outfitId) &&
            OutfitLibrary::GetSingleton().MatchesArmorType(outfitId, assignment.adventuringArmorType);
    });
}

void SituationHandler::ApplyForSituation(RE::Actor* actor)
{
    if (!actor) return;

    auto& assignments = OutfitAssignments::GetSingleton();
    auto& wigAssignments = WigAssignments::GetSingleton();
    auto actorId = actor->GetFormID();

    bool hasOutfitAssignment = assignments.HasAnySituation(actorId) || assignments.GetOutfitId(actorId) > 0;
    bool hasWigSituations = wigAssignments.HasAnySituation(actorId);
    if (!hasOutfitAssignment && !hasWigSituations) {
        return;
    }

    if (actor->IsInCombat()) {
        logger::info("ApplyForSituation: {} is in combat, skipping", actor->GetDisplayFullName());
        return;
    }

    auto situation = EvaluateSituation(actor);

    // Revalidate the effective selection before the same-situation cache. A
    // changed preference/pool must not retain an ineligible daily random choice.
    const int outfitId = hasOutfitAssignment ? ResolveOutfitForSituation(actorId, situation) : 0;
    const auto current = _currentSituations.find(actorId);
    const auto applied = _appliedOutfitIds.find(actorId);
    if (current != _currentSituations.end() && current->second == situation &&
        applied != _appliedOutfitIds.end() && applied->second == outfitId) return;

    // --- Outfit situational application ---
    if (hasOutfitAssignment) {
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
    _appliedOutfitIds[actorId] = outfitId;
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
    _appliedOutfitIds.erase(actorId);
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
    _sleepMonitoringEnabled.store(false);
    sSituationGeneration.fetch_add(1);
    _observedSleepStates.clear();
    _currentSituations.clear();
    _appliedOutfitIds.clear();
    _automaticRetryCounts.clear();
    _automaticRetryPending.clear();
    _randomStates.clear();
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

    std::unordered_set<RE::FormID> actorIds;
    for (const auto& [id, assignment] : OutfitAssignments::GetSingleton().GetAll()) {
        if (assignment.HasAnySituation()) actorIds.insert(id);
    }
    for (const auto& [id, assignment] : WigAssignments::GetSingleton().GetAllSituational()) {
        if (assignment.HasAnySituation()) actorIds.insert(id);
    }

    std::erase_if(_observedSleepStates, [&actorIds](const auto& entry) {
        return !actorIds.contains(entry.first);
    });
    for (auto id : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded() || actor->IsPlayerRef() || actor->IsDead()) {
            _observedSleepStates.erase(id);
            continue;
        }
        // Leave a blocked transition unobserved so it can be applied after combat.
        if (actor->IsInCombat()) continue;

        const bool sleeping = IsSleepSituation(actor);
        auto [it, inserted] = _observedSleepStates.try_emplace(id, sleeping);
        const bool wasSleeping = inserted ? GetCachedSituation(id) == OutfitSituation::Sleep : it->second;
        it->second = sleeping;
        if (wasSleeping != sleeping) {
            logger::info("SituationHandler: {} sleep transition {} -> {} (nativeState={})",
                actor->GetDisplayFullName(), wasSleeping, sleeping, static_cast<int>(Tailor::Situations::ReadSleepState(actor)));
            ApplyForSituation(actor);
        }
    }
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
        SituationHandler::GetSingleton()->EvaluateAllAssignedActors();
    });

    return RE::BSEventNotifyControl::kContinue;
}
