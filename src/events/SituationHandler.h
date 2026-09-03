#pragma once

#include <random>
#include <unordered_map>
#include <unordered_set>
#include "outfit/OutfitAssignments.h"

class SituationHandler : public RE::BSTEventSink<RE::TESActorLocationChangeEvent>,
                         public RE::BSTEventSink<RE::TESFurnitureEvent>,
                         public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    static SituationHandler* GetSingleton();
    static void Register();

    void Initialize();

    OutfitSituation EvaluateSituation(RE::Actor* actor) const;
    void ApplyForSituation(RE::Actor* actor);
    OutfitSituation GetCachedSituation(RE::FormID actorId) const;
    void SetCachedSituation(RE::FormID actorId, OutfitSituation situation);
    void ClearCachedSituation(RE::FormID actorId);
    void ForceApplyForSituation(RE::Actor* actor);
    void ResetForGameLoad();

private:
    SituationHandler() = default;

    void EvaluateAllAssignedActors();
    static void ScheduleDelayedEval(std::chrono::milliseconds delay);
    static void ScheduleDelayedActorEval(RE::ActorHandle handle, std::chrono::milliseconds delay);
    static void ScheduleAutomaticRetry(
        RE::ActorHandle handle,
        RE::FormID actorId,
        std::chrono::milliseconds delay);

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESActorLocationChangeEvent* event,
        RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESFurnitureEvent* event,
        RE::BSTEventSource<RE::TESFurnitureEvent>*) override;

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

    RE::BGSKeyword* _kwPlayerHouse = nullptr;
    RE::BGSKeyword* _kwCity = nullptr;
    RE::BGSKeyword* _kwTown = nullptr;
    RE::BGSKeyword* _kwSettlement = nullptr;
    RE::BGSKeyword* _kwDwelling = nullptr;
    RE::BGSKeyword* _kwInn = nullptr;

    std::unordered_map<RE::FormID, OutfitSituation> _currentSituations;
    std::unordered_map<RE::FormID, int> _automaticRetryCounts;
    std::unordered_set<RE::FormID> _automaticRetryPending;

    // --- Randomized outfit support ---
    struct RandomSlotState {
        float lastRandomDay = -1.0f;
        int   lastRandomOutfitId = 0;
    };
    struct ActorRandomState {
        RandomSlotState slots[4];  // indexed by (int)OutfitSituation - 1
    };
    std::unordered_map<RE::FormID, ActorRandomState> _randomStates;

    int ResolveRandomOutfit(RE::FormID actorId, OutfitSituation situation);
    int ResolveOutfitForSituation(RE::FormID actorId, OutfitSituation situation);
    static float GetGameDaysPassed();
    std::mt19937 _rng{ std::random_device{}() };
};
