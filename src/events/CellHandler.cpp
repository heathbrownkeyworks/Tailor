#include "events/CellHandler.h"
#include "events/SituationHandler.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitStore.h"
#include "wig/WigAssignments.h"
#include "wig/WigManager.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
    constexpr int32_t kWigMaxRetries = 30;
    constexpr int32_t kWigRetryDelayMs = 16;

    // Outfit retries must wait real time. SKSE AddTask calls made from inside
    // another task can drain in the same frame, so recursive task-only retries
    // can give up before the actor's 3D has a chance to finish loading.
    constexpr int32_t kOutfitMaxRetries  = 200;
    constexpr int32_t kOutfitRetryDelayMs = 100;

    // Stagger outfit re-equips to avoid overwhelming SKEE's body morph pipeline.
    // Without this, all followers entering a cell get SetDefaultOutfit in the same
    // frame, which can exceed skee64.ini uBodyMorphMemoryLimit and CTD.
    constexpr int32_t kStaggerMs        = 50;   // ms between each actor's re-equip
    constexpr int64_t kStaggerResetMs   = 2000;  // reset counter after 2s of inactivity

    std::atomic<int32_t> sStaggerIndex{0};
    std::atomic<int64_t> sLastStaggerTimeMs{0};
    std::atomic<std::uint64_t> sOutfitTaskGeneration{1};
    std::mutex sPendingOutfitMutex;
    std::unordered_set<std::uint32_t> sPendingOutfitHandles;

    void FinishOutfitTask(RE::ActorHandle actorHandle)
    {
        std::lock_guard lock(sPendingOutfitMutex);
        sPendingOutfitHandles.erase(actorHandle.native_handle());
    }

    // Extra frame delay for NFF-managed followers.  NFF's outfit switching
    // uses utility.Wait(0.3-0.5s) between SetOutfit calls.
    constexpr int32_t kNFFExtraDelayMs = 500;

    // Base frame offset for wig re-equip — ensures outfit re-equip
    // completes first (outfit changes can strip hair-slot items).
    constexpr int32_t kWigInitialDelayMs = 80;

    // --- Outfit Re-Equip ---

    void DeferOutfitReEquip(
        RE::ActorHandle actorHandle,
        int32_t attempt,
        int32_t delayMs,
        std::uint64_t generation);

    bool DoOutfitReEquip(RE::Actor* actor, int32_t attempt)
    {
        auto& assignments = OutfitAssignments::GetSingleton();
        auto actorId = actor->GetFormID();

        if (assignments.HasAnySituation(actorId)) {
            SituationHandler::GetSingleton()->ApplyForSituation(actor);
            logger::info("CellHandler: requested situational outfit evaluation on {} (attempt {})",
                actor->GetDisplayFullName(), attempt);
            return true;
        }

        int outfitId = assignments.GetOutfitId(actorId);
        if (outfitId <= 0) return true;

        auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
        if (!outfit) {
            logger::warn("CellHandler: outfit id {} not found for {}", outfitId, actor->GetDisplayFullName());
            return true;
        }

        if (!OutfitManager::GetSingleton().ApplyCustomOutfit(actor, *outfit, true)) {
            return false;
        }
        logger::info("CellHandler: re-applied outfit '{}' on {} (attempt {})",
            outfit->name, actor->GetDisplayFullName(), attempt);
        return true;
    }

    void DeferOutfitReEquip(
        RE::ActorHandle actorHandle,
        int32_t attempt,
        int32_t delayMs,
        std::uint64_t generation)
    {
        auto schedule = [actorHandle, attempt, generation]() {
            if (generation != sOutfitTaskGeneration.load()) return;

            auto actorPtr = actorHandle.get();
            if (!actorPtr) {
                FinishOutfitTask(actorHandle);
                return;
            }
            auto* actor = actorPtr.get();

            if (!actor->Is3DLoaded()) {
                if (attempt < kOutfitMaxRetries) {
                    DeferOutfitReEquip(
                        actorHandle, attempt + 1, kOutfitRetryDelayMs, generation);
                } else {
                    logger::warn("CellHandler: gave up outfit re-equip on {} after {} attempts ({}ms delay)",
                        actor->GetDisplayFullName(), kOutfitMaxRetries, kOutfitRetryDelayMs);
                    FinishOutfitTask(actorHandle);
                }
                return;
            }

            if (!DoOutfitReEquip(actor, attempt)) {
                if (attempt < kOutfitMaxRetries) {
                    DeferOutfitReEquip(
                        actorHandle, attempt + 1, kOutfitRetryDelayMs, generation);
                } else {
                    logger::warn(
                        "CellHandler: OBody never reached a safe state for automatic outfit re-equip on {}",
                        actor->GetDisplayFullName());
                    FinishOutfitTask(actorHandle);
                }
                return;
            }

            FinishOutfitTask(actorHandle);
        };

        if (delayMs > 0) {
            std::thread([delayMs, schedule = std::move(schedule)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                SKSE::GetTaskInterface()->AddTask(std::move(schedule));
            }).detach();
            return;
        }

        SKSE::GetTaskInterface()->AddTask(std::move(schedule));
    }

    // --- Wig Re-Equip ---

    void DeferWigReEquip(
        RE::ActorHandle actorHandle,
        int32_t attempt,
        bool nffDelayed,
        std::uint64_t generation,
        int32_t delayMs);

    void DoWigReEquip(RE::Actor* actor, int32_t attempt)
    {
        auto actorId = actor->GetFormID();

        // Situational wig branch
        if (WigAssignments::GetSingleton().HasAnySituation(actorId)) {
            SituationHandler::GetSingleton()->ApplyForSituation(actor);
            logger::info("CellHandler: situational wig re-equip on {} (frame {})",
                actor->GetDisplayFullName(), attempt);
            return;
        }

        auto state = WigAssignments::GetSingleton().GetState(actorId);
        if (!state) return;

        if (state->currentWig.formId != 0 && !state->currentWig.plugin.empty()) {
            auto* armor = state->currentWig.Resolve();
            if (armor) {
                auto* npc = actor->GetActorBase();
                if (WigManager::GetActorItemCount(actor, armor) <= 0) {
                    actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
                    WigAssignments::GetSingleton().MarkItemAdded(actorId);
                }

                if (npc) {
                    WigManager::EnsureArmorAddonRace(armor, npc->GetRace(), npc->GetSex());
                }

                auto* equipManager = RE::ActorEquipManager::GetSingleton();
                if (equipManager) {
                    equipManager->EquipObject(actor, armor, nullptr, 1, nullptr, true, false, false, false);
                    logger::info("CellHandler: re-equipped wig '{}' on {} (frame {})",
                        state->currentWig.name, actor->GetDisplayFullName(), attempt);
                }
            }
        }

        if (state->HasHairColor()) {
            auto handle = actor->GetHandle();
            auto r = static_cast<uint8_t>(state->hairColorR);
            auto g = static_cast<uint8_t>(state->hairColorG);
            auto b = static_cast<uint8_t>(state->hairColorB);
            WigManager::GetSingleton().DeferHairColor(handle, r, g, b, 1);
            WigManager::GetSingleton().DeferHairColor(handle, r, g, b, 5);
            WigManager::GetSingleton().DeferHairColor(handle, r, g, b, 12);
        }

        WigManager::GetSingleton().ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
    }

    void DeferWigReEquip(
        RE::ActorHandle actorHandle,
        int32_t attempt,
        bool nffDelayed,
        std::uint64_t generation,
        int32_t delayMs)
    {
        auto schedule = [actorHandle, attempt, nffDelayed, generation]() {
            if (generation != sOutfitTaskGeneration.load()) return;

            auto actorPtr = actorHandle.get();
            if (!actorPtr) return;
            auto* actor = actorPtr.get();

            if (!actor->Is3DLoaded()) {
                if (attempt < kWigMaxRetries) {
                    DeferWigReEquip(
                        actorHandle,
                        attempt + 1,
                        nffDelayed,
                        generation,
                        kWigRetryDelayMs);
                } else {
                    logger::warn("CellHandler: gave up wig re-equip on {} after {} retries",
                        actor->GetDisplayFullName(), attempt);
                }
                return;
            }

            // For NFF-managed followers, add extra delay so NFF's outfit
            // switching finishes before we re-equip the wig.
            if (!nffDelayed && WigManager::GetSingleton().IsNFFManaged(actor)) {
                DeferWigReEquip(
                    actorHandle,
                    attempt,
                    true,
                    generation,
                    kNFFExtraDelayMs);
                return;
            }

            DoWigReEquip(actor, attempt);
        };

        // AddTask calls made from inside a running task drain in the same frame.
        // Use real time between retries so actor 3D and NFF outfit work can advance.
        if (delayMs > 0) {
            std::thread([delayMs, schedule = std::move(schedule)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                SKSE::GetTaskInterface()->AddTask(std::move(schedule));
            }).detach();
            return;
        }

        SKSE::GetTaskInterface()->AddTask(std::move(schedule));
    }
}

CellHandler* CellHandler::GetSingleton()
{
    static CellHandler singleton;
    return &singleton;
}

void CellHandler::Register()
{
    auto* events = RE::ScriptEventSourceHolder::GetSingleton();
    if (events) {
        events->AddEventSink<RE::TESCellAttachDetachEvent>(GetSingleton());
        events->AddEventSink<RE::TESCellFullyLoadedEvent>(GetSingleton());
        logger::info("CellHandler registered for cell attach/detach/loaded events");
    }
}

void CellHandler::QueueOutfitReEquip(RE::ActorHandle actorHandle, int32_t delayMs)
{
    const auto handle = actorHandle.native_handle();
    if (handle == 0) return;

    {
        std::lock_guard lock(sPendingOutfitMutex);
        if (!sPendingOutfitHandles.insert(handle).second) {
            return;
        }
    }

    DeferOutfitReEquip(
        actorHandle,
        0,
        delayMs,
        sOutfitTaskGeneration.load());
}

void CellHandler::InvalidatePendingOutfitTasks()
{
    sOutfitTaskGeneration.fetch_add(1);
    {
        std::lock_guard lock(sPendingOutfitMutex);
        sPendingOutfitHandles.clear();
    }
    sStaggerIndex.store(0);
    sLastStaggerTimeMs.store(0);
    logger::info("CellHandler: invalidated pending outfit tasks for game load");
}

RE::BSEventNotifyControl CellHandler::ProcessEvent(
    const RE::TESCellAttachDetachEvent*                event,
    RE::BSTEventSource<RE::TESCellAttachDetachEvent>*)
{
    if (!event) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto ref = event->reference.get();
    if (!ref) return RE::BSEventNotifyControl::kContinue;

    auto* actor = ref->As<RE::Actor>();
    if (!actor || actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    if (!event->attached) {
        // Cell detach — process any deferred wig inventory removals.
        WigManager::GetSingleton().ProcessDeferredRemoval(actor);
        return RE::BSEventNotifyControl::kContinue;
    }

    // Cell attach — re-equip outfit if assigned (staggered to avoid SKEE morph memory burst)
    auto& outfitAssignments = OutfitAssignments::GetSingleton();
    auto actorId = actor->GetFormID();
    if (outfitAssignments.HasAssignment(actorId) || outfitAssignments.HasAnySituation(actorId)) {
        // Auto-reset stagger counter after a gap (new cell transition burst)
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - sLastStaggerTimeMs.load() > kStaggerResetMs) {
            sStaggerIndex.store(0);
        }
        sLastStaggerTimeMs.store(nowMs);

        int32_t delayMs = sStaggerIndex.fetch_add(1) * kStaggerMs;
        QueueOutfitReEquip(actor->GetHandle(), delayMs);
    }

    // Cell attach — re-equip wig if assigned or situational (starts with frame offset so outfit goes first)
    if (WigAssignments::GetSingleton().HasAssignment(actorId) ||
        WigAssignments::GetSingleton().HasAnySituation(actorId)) {
        DeferWigReEquip(
            actor->GetHandle(),
            0,
            false,
            sOutfitTaskGeneration.load(),
            kWigInitialDelayMs);
    }

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl CellHandler::ProcessEvent(
    const RE::TESCellFullyLoadedEvent*                 event,
    RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
{
    if (!event) {
        return RE::BSEventNotifyControl::kContinue;
    }

    // Cell finished loading — re-apply hair colors for nearby NPCs.
    // Catches followers who travel with the player (persistent refs).
    WigManager::GetSingleton().ReApplyAllHairColors();

    return RE::BSEventNotifyControl::kContinue;
}
