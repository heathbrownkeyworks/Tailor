#pragma once

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitSnapshot.h"
#include "events/SwimmingPolicy.h"
#include "events/OutfitOverridePolicy.h"
#include "player/PlayerSituationPolicy.h"

class SituationHandler : public RE::BSTEventSink<RE::TESActorLocationChangeEvent>,
                         public RE::BSTEventSink<RE::TESFurnitureEvent>,
                         public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                         public RE::BSTEventSink<RE::TESSleepStopEvent>
{
public:
    static SituationHandler* GetSingleton();
    static void Register();

    void Initialize();

    // `quiet` skips the logs, for the poll's Hide settings, which ask every poll.
    OutfitSituation EvaluateSituation(RE::Actor* actor, bool includeSwimming = true, bool quiet = false) const;
    // The situation by location alone (Home, Town or Adventuring): the one besides Sleep, which a
    // Sleep look without a Sleep outfit falls back to.
    OutfitSituation EvaluateLocationSituation(RE::Actor* actor, bool quiet = false) const;
    void ApplyForSituation(RE::Actor* actor);
    OutfitSituation GetCachedSituation(RE::FormID actorId) const;
    void SetCachedSituation(RE::FormID actorId, OutfitSituation situation);
    void ClearCachedSituation(RE::FormID actorId);
    void ClearOutfitOverrides(RE::FormID actorId);
    // A deleted outfit must not live on as what an NPC is wearing, or as the outfit
    // a water or combat override put aside to return to. Drops that state and
    // returns the NPCs it belonged to.
    std::vector<RE::FormID> ForgetOutfit(int outfitId);
    void ForceApplyForSituation(RE::Actor* actor);
    void ResetForGameLoad();
    void StartSleepMonitoring();
    int ResolveOutfitForSituation(RE::FormID actorId, OutfitSituation situation);
    // The person's own fixed or random choice for that situation, if they can wear it; 0 without one. A
    // mod's override is not asked.
    int SituationChoice(RE::FormID actorId, OutfitSituation situation);
    // Whether the actor's own choice would dress them for this situation, without picking one: a fixed outfit they
    // can wear (for Adventuring, one their armor type allows), or Random with an outfit they can wear in the pool.
    bool HasSituationChoice(RE::FormID actorId, OutfitSituation situation) const;
    std::optional<int> GetAppliedOutfitId(RE::FormID actorId) const;
    // The Tailor outfit this person wears now, without evaluating a situation: the player's as the wardrobe
    // has it; for an NPC the situation flow dresses (DressedBySituations), as it last put it on; else a mod's
    // override they can wear, else their regular outfit when they have no situations and can wear it. 0 for
    // none.
    int CurrentOutfitId(RE::Actor* actor) const;
    // An NPC whose situation outfit Tailor hasn't put on since a load or since it last forgot what they wear:
    // dressed by the situation flow, no outfit applied, no override they can wear, and situation outfits.
    // CurrentOutfitId says 0 for them, which is not yet what they wear.
    bool OutfitPending(RE::Actor* actor) const;

    // --- The player (SituationHandlerPlayer.cpp) ---
    // The player's situations run while they have a situation outfit or wig, or a mod's override.
    bool PlayerHasSituations() const;
    // Dress the player for their situation now: Tailor's own actions, a load, a deletion, a
    // sex change or an edit. Only another outfit than the one worn goes on, unless `redress`.
    void ReconcilePlayerSituation(bool redress);
    // A load that came before the player could be dressed: the poll runs its reconcile, outfit and
    // wig, once they can be.
    void ReconcilePlayerWhenDressable();
    // The player has no situations left: the look, a waiting change and its trigger, a headgear wait and the
    // wake-up note end. Tailor's own clearing calls it at once, since the poll waits for Tailor to close.
    void ForgetPlayerSituationState();
    // Where the player woke up in their Sleep outfit travels in each save's co-save.
    Tailor::Player::PlayerWakeUp ExportPlayerWakeUp() const;
    void ImportPlayerWakeUp(const Tailor::Player::PlayerWakeUp& note);

private:
    SituationHandler() = default;

    void EvaluateAllAssignedActors();
    void PollSleepStates();
    Tailor::Situations::OutfitOverrideResult ApplySwimmingOverride(RE::Actor* actor, bool inWater);
    bool ApplyAdventuringOverride(RE::Actor* actor, int outfitId);
    std::optional<OutfitSnapshot> CaptureReturnOutfit(RE::Actor* actor);
    int ResolveSituationSlot(RE::FormID actorId, OutfitSituation situation);
    // Whether the situation flow dresses this NPC and records what it puts on: one with a mod's override,
    // situation outfits, or a swim or a fight holding them. CurrentOutfitId and OutfitPending go by it.
    bool DressedBySituations(RE::FormID actorId) const;
    // The NPC wig step: see ApplySituationWig. Due at a change of situation, as for the player. `situation` is
    // the wig situation, never Warm.
    void ApplySituationWig(RE::Actor* actor, OutfitSituation situation);
    bool WigStepDue(RE::FormID actorId, OutfitSituation situation) const;
    // The situation the wig follows: under Warm, which has no wig, the location beneath it.
    OutfitSituation WigSituationFor(RE::Actor* actor, OutfitSituation situation) const;
    // Weapons in the Sleep and Swimming looks and with Hide Weapons (SituationWeapons), for the player
    // and every NPC the poll visits; everyone else gets theirs back.
    void UpdateSituationWeapons(const std::unordered_set<RE::FormID>& actorIds);
    // The situation Hide Weapons and Hide Helmets go by: the player's as evaluated now, an NPC's as
    // last applied, and under Warm the location beneath it.
    OutfitSituation HidePlace(RE::Actor* actor) const;
    // Hide Helmets (SituationHelmets): outside Adventuring and combat the player's and every polled NPC's
    // helmets and hoods come off, and go back on otherwise.
    void UpdateSituationHelmets(const std::unordered_set<RE::FormID>& actorIds);
    // A Sleep outfit the actor can wear (a fixed one that fits, or a random pool holding one), or a
    // Sleep wig that can go on: what makes a Sleep look.
    bool HasSleepLook(RE::FormID actorId) const;
    bool HasSleepOutfit(RE::FormID actorId) const;
    // An NPC in bed with a Sleep outfit they can wear, or a Sleep wig, or in the Swimming outfit their
    // water override put on; or wearing a mod's override set for Swimming or Sleep.
    bool NpcWeaponsLookOn(RE::FormID actorId) const;
    // A mod's override the person wears decides the look alone: set for Swimming or Sleep it is that look wherever
    // they are, set for anything else it is none. Nothing when they don't wear it (not dressed in it yet, or in
    // their Adventuring outfit for a fight), so Tailor's own looks decide.
    std::optional<bool> OverrideLook(RE::FormID actorId, int wornOutfitId) const;
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

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESSleepStopEvent* event,
        RE::BSTEventSource<RE::TESSleepStopEvent>*) override;

    RE::BGSKeyword* _kwPlayerHouse = nullptr;
    RE::BGSKeyword* _kwHouse = nullptr;
    RE::BGSKeyword* _kwCity = nullptr;
    RE::BGSKeyword* _kwTown = nullptr;
    RE::BGSKeyword* _kwSettlement = nullptr;
    RE::BGSKeyword* _kwDwelling = nullptr;
    RE::BGSKeyword* _kwInn = nullptr;

    std::unordered_map<RE::FormID, OutfitSituation> _currentSituations;
    // The situation each NPC's wig step last ran for. Fights and swims don't run the step while they last;
    // their restores run it when the situation changed.
    std::unordered_map<RE::FormID, OutfitSituation> _wigSituations;
    std::unordered_map<RE::FormID, int> _appliedOutfitIds;
    std::unordered_map<RE::FormID, int> _automaticRetryCounts;
    std::unordered_set<RE::FormID> _automaticRetryPending;

    // --- Randomized outfit support ---
    struct RandomSlotState {
        float lastRandomDay = -1.0f;
        int   lastRandomOutfitId = 0;
    };
    struct ActorRandomState {
        RandomSlotState slots[kLastOutfitSituation];  // indexed by (int)OutfitSituation - 1
    };
    std::unordered_map<RE::FormID, ActorRandomState> _randomStates;

    int ResolveRandomOutfit(RE::FormID actorId, OutfitSituation situation);
    std::vector<int> RandomPool(RE::FormID actorId, OutfitSituation situation) const;
    static float GetGameDaysPassed();
    std::mt19937 _rng{ std::random_device{}() };

    std::unordered_map<RE::FormID, bool> _observedSleepStates;
    std::unordered_map<RE::FormID, bool> _observedWaterStates;
    std::unordered_map<RE::FormID, Tailor::Situations::OutfitOverride<OutfitSnapshot>> _swimmingOverrides;
    std::unordered_map<RE::FormID, Tailor::Situations::OutfitOverride<OutfitSnapshot>> _adventuringOverrides;
    std::unordered_set<RE::FormID> _wokenByFight;  // NPCs a fight woke: its end dresses them for where they are
    std::unordered_map<RE::FormID, bool> _observedCombatStates;
    // Outdoors in the cold, as the poll last saw it; held while a swim or a fight lasts.
    std::unordered_map<RE::FormID, bool> _observedColdStates;

    // --- The player (SituationHandlerPlayer.cpp) ---
    // Automatic changes wait for Tailor to close; Now and Redress are Tailor's own actions and
    // the reconciles, and Redress puts the outfit on again even when it is the one worn. Weakest
    // first: a change that waits keeps the strongest trigger that waited.
    enum class PlayerTrigger { Automatic, Now, Redress };
    struct PlayerLook
    {
        Tailor::Player::PlayerRule rule = Tailor::Player::PlayerRule::Suspended;
        OutfitSituation situation = OutfitSituation::Adventuring;
        int outfitId = Tailor::Player::kKeepOutfit;
        bool swimmingOutfit = false;  // in water, a Swimming outfit went on: the look hides weapons
        // The situation beneath Warm the wig follows: a location change under Warm changes the look.
        OutfitSituation wigSituation = OutfitSituation::Adventuring;
        bool operator==(const PlayerLook&) const = default;
    };
    struct PlayerState
    {
        std::optional<PlayerLook> applied;  // the look the situations last put on
        std::optional<OutfitSituation> wigSituation;  // the land situation the wig step last ran for
        bool pending = false;               // a change waiting for Tailor, a preview, a dressable player, the turn-back or headgear
        // The strongest trigger that waited: the poll replays a deferred Redress or Now as itself.
        PlayerTrigger pendingTrigger = PlayerTrigger::Automatic;
        int headwearWaits = 0;              // polls the change has waited for headgear to go back
        bool reconcileAfterLoad = false;    // a load's reconcile waiting for the player to be dressable
        Tailor::Player::PlayerWakeUp wakeUp;  // where the player woke up in their Sleep outfit
        Tailor::Player::PlayerFight fight;
        // Only the first look after a load keeps the day's random pick the save shows.
        bool keepSavedPick = true;
        Tailor::Player::PlayerBeastForm beast;
        bool swimming = false;              // in the water: swimming, or wading knee-deep until fully out
        bool sleeping = false;
        bool cold = false;                  // outdoors in cold weather or a snowy region
        std::optional<std::chrono::steady_clock::time_point> lastPoll;
    };
    void ApplyPlayerSituation(RE::Actor* player, PlayerTrigger trigger);
    // A change that can't go ahead yet waits for the poll, keeping the strongest trigger that waited;
    // the reason is logged when the wait starts.
    void DeferPlayerSituation(PlayerTrigger trigger, const char* reason);
    void KeepPlayerRandomPick(OutfitSituation slot, int worn);
    // The player's sleep ended: with a Sleep outfit they can wear, or a Sleep wig, they wake up in it,
    // until they walk away.
    void PlayerWokeUp();
    // What waking up puts on (a Sleep outfit they can wear, or a Sleep wig), and whether the
    // wake-up note still holds: that, and the player still by the bed.
    bool PlayerHasSleepLook() const;
    bool PlayerWakeUpHolds(RE::Actor* player) const;
    // The player's Sleep look, or their Swimming outfit on, as the situations last put it on; or a mod's override
    // set for Swimming or Sleep that they wear.
    bool PlayerWeaponsLookOn() const;
    // The poll's player branch: combat and the five seconds after it, swimming, bed, beast form, a
    // load's waiting reconcile, and replaying a change that waited.
    void PollPlayer();
    bool IsSleeping(RE::Actor* actor) const;
    // Swimming, or wading knee-deep, until fully out of the water; the poll's last state carries it.
    bool IsPlayerInWater(RE::Actor* player) const;
    PlayerState _player;
    std::atomic<bool> _sleepMonitoringEnabled{false};
    std::atomic<bool> _sleepPollPending{false};
    // Destroy/join the timer before the state it reads. It never accesses actors.
    std::jthread _sleepMonitor;
};
