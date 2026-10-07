#pragma once

#include "outfit/ArmorItem.h"
#include "outfit/CustomOutfit.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"
#include "outfit/OutfitSnapshot.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class OutfitManager
{
public:
    static OutfitManager& GetSingleton();

    void Initialize();

    void        UpdateTargetFromCrosshair();
    RE::Actor*  GetTarget() const;

    // --- The player (dressed through PlayerWardrobe, never outfit records) ---
    // With no NPC under the crosshair, or from the switch button.
    bool        TargetPlayer();
    // Back to the NPC the session opened on, while she is valid and loaded.
    bool        TargetSessionNpc();
    bool        IsPlayerTarget() const;
    RE::Actor*  GetSessionNpc() const;
    // The target exactly as it stands, and putting it back without TargetPlayer's checks: a switch that fails part-way
    // (TailorUI::SwitchTarget) returns the outfit side to the target the wig side and the preview still have.
    struct TargetState { RE::ActorHandle actor; bool player = false; };
    TargetState SaveTarget() const { return {_currentTarget, _playerTarget}; }
    void RestoreTarget(const TargetState& saved) { _currentTarget = saved.actor; _playerTarget = saved.player; }
    // Dress the player for their assignments, even in the outfit already worn (it was edited):
    // their situation's outfit, else the regular outfit, else Own Gear.
    void        RedressPlayer();
    // After a load: redress only when the assignments now call for another outfit.
    void        ReconcilePlayerAfterLoad();
    // Redress the player only when their assignments now call for another outfit than the one worn.
    void        ReconcilePlayer();
    // Whether the player wears this outfit with pieces it no longer has: it was edited, in another
    // save, since Tailor dressed them. False without a recorded list (older saves).
    bool        PlayerOutfitPiecesChanged(int outfitId) const;
    std::string GetTargetName() const;
    std::string GetTargetSex() const;
    // The NPC's sex as the engine records it (0 male, 1 female), or -1 when unknown.
    static int GetNpcSex(RE::Actor* actor);
    // The saved outfits this NPC can wear: they exist and fit the NPC's sex.
    static OutfitLibrary::Wearable WearableBy(RE::Actor* actor);

    // --- Create Outfit: dynamic outfit preview via OTFT ---
    void BeginCreateOutfit(RE::Actor* actor);
    void EndCreateOutfit();
    void AddItemToCreateOutfit(RE::Actor* actor, const ArmorItem& item);
    void RemoveItemFromCreateOutfit(RE::Actor* actor, const ArmorItem& item);
    void LoadCreateOutfitItems(RE::Actor* actor, const std::vector<ArmorItem>& items);

    // --- Cycling custom outfits ---
    struct CycleState
    {
        int              categoryId = 0;
        int              index = 0;
        std::vector<int> outfitIds;   // custom outfit IDs
        OutfitSituation situation = static_cast<OutfitSituation>(0);
    };

    bool StartCycle(int categoryId, OutfitSituation situation = static_cast<OutfitSituation>(0));
    bool CycleNext();
    bool CyclePrev();
    bool CycleToIndex(int index);
    bool ConfirmCycle(OutfitSituation situation = static_cast<OutfitSituation>(0));
    void CancelCycle();
    bool IsCycling() const;
    const CycleState* GetCycleState() const;

    // Get the name of the current cycle outfit
    std::string GetCycleOutfitName() const;

    // Apply a custom outfit to an actor via dynamic OTFT
    bool ApplyCustomOutfit(RE::Actor* actor, const CustomOutfit& outfit, bool automatic = false);

    // Reset to vanilla outfit
    bool ResetOutfit(RE::Actor* actor);
    // The outfit was deleted from the library: strip it from every assignment and
    // redress everyone who was assigned it or is wearing it.
    void ReleaseDeletedOutfit(int outfitId);
    // These outfits changed sex: queue a staggered redress for every loaded NPC assigned
    // or wearing one, through the usual fallback chain. Assignments stay as they are.
    void RefitOutfits(const std::vector<int>& outfitIds);
    // True while the open Create/Edit session is dressing this NPC.
    bool IsCreateSessionActor(RE::Actor* actor) const;
    // An NPC Tailor keeps no outfit for, whom a mod's override let go (its outfit was deleted, or the mod
    // cleared it): the re-equip queue gives them their own outfit back, now or when they next load,
    // retrying until it goes on.
    bool IsReturningToOwnOutfit(RE::FormID actorId) const;
    void MarkReturnToOwnOutfit(RE::FormID actorId);
    void ForgetReturnToOwnOutfit(RE::FormID actorId);
    // Restore the saved baseline without clearing assignments or NPC preferences.
    bool RestoreOriginalOutfit(RE::Actor* actor, bool automatic = false);
    std::optional<OutfitSnapshot> CaptureOutfitSnapshot(RE::Actor* actor) const;
    bool RestoreOutfitSnapshot(RE::Actor* actor, const OutfitSnapshot& snapshot);

    void CaptureDefaultOutfitsAtDataLoad();
    void ReApplyAllAssignments();
    void PrepareForGameLoad();

    // Re-equip wig after an outfit change on this actor
    static void NotifyOutfitChanged(RE::Actor* actor);

    // An NPC Tailor can dress: alive, not a creature or a dragon, never the player. Public for the mod
    // API, whose overrides take the player and these.
    bool IsValidTarget(RE::Actor* actor) const;

private:
    OutfitManager() = default;

    // The active dynamic OTFT is never edited in place. New contents are built in
    // `alternate`, applied, and only then swapped into `primary`.
    struct OutfitPair
    {
        RE::BGSOutfit* primary   = nullptr;
        RE::BGSOutfit* alternate = nullptr;
        std::vector<RE::TESForm*> desiredItems;
    };

    struct DefaultOutfitState
    {
        RE::BGSOutfit* defaultOutfit = nullptr;
        RE::BGSOutfit* sleepOutfit = nullptr;
    };

    bool InitializeHiddenOutfitItems(RE::Actor* actor, RE::BGSOutfit* outfit, bool update3D) const;
    bool SetActorDefaultOutfit(RE::Actor* actor, RE::BGSOutfit* outfit, bool update3D) const;
    bool SetActorSleepOutfit(RE::Actor* actor, RE::BGSOutfit* outfit) const;
    static bool GetOutfitChangeFlag(RE::TESNPC* npc, std::uint32_t flag);
    static void SuppressOutfitChangeFlags(RE::TESNPC* npc);
    static void RestoreOutfitChangeFlags(RE::TESNPC* npc, bool defaultHadChange, bool sleepHadChange);
    bool GetDataLoadedDefaultOutfitState(RE::Actor* actor, DefaultOutfitState& state) const;
    // DOFT/SOFT as the plugin that created the NPC wrote them. The game only
    // remembers the winning override, so this reads that plugin's file.
    bool GetOriginRecordOutfitState(RE::Actor* actor, DefaultOutfitState& state) const;
    // Whether each outfit must be saved for it to survive a reload: true when the
    // load order's winning record would otherwise put a different one back.
    void GetOriginRecordChangeState(RE::Actor* actor, const DefaultOutfitState& origin, bool& defaultHadChange, bool& sleepHadChange) const;
    bool StoreOriginRecordOutfitState(RE::Actor* actor);
    void CapturePersistedOutfitStateIfNeeded(RE::Actor* actor);
    void InitFlushOutfit();
    bool InitOutfitPair(OutfitPair& pair);
    OutfitPair* GetOrCreateActorOutfit(RE::FormID actorId);
    bool FlushAndApplyOutfit(RE::Actor* actor, OutfitPair& pair);
    bool RestoreCycleSnapshot(RE::Actor* actor);
    // Whether this is one of Tailor's runtime forms: an NPC's outfit pair, the Create/Edit pair or the flush outfit.
    // Their FF FormIDs don't survive a load, so a record pointing at one must never be marked for the save.
    bool IsRuntimeOutfit(const RE::BGSOutfit* outfit) const;
    // The change flags StartCycle captured, put back field by field, never on a field that points at a runtime form.
    void RestoreCycleChangeFlags(RE::TESNPC* npc) const;
    // StartCycle's first outfit didn't go on: undo what the attempt changed and forget the capture.
    void AbandonCycleStart(RE::Actor* target, bool player);
    void TryEndCreateOutfit(std::uint64_t generation, int attempt);
    void RestoreAssignedOutfit(RE::Actor* actor);
    void QueuePreviewDiagnostics(RE::ActorHandle actor, std::uint64_t generation);
    void QueueEquipmentAudit(RE::Actor* actor, RE::BGSOutfit* outfit) const;

    bool ApplyPlayerOutfit(RE::Actor* player, const CustomOutfit& outfit);
    bool RestorePlayerOwnGear(RE::Actor* player);
    // Ends a player preview that is being put back, not kept: cancels its pending
    // equipment check too, since the previewed pieces are about to come off anyway.
    bool CancelPlayerPreview(RE::Actor* player);
    bool ResetPlayerOutfit(RE::Actor* player);
    bool ConfirmPlayerCycle(RE::Actor* player, OutfitSituation situation, int outfitId);
    void BeginPlayerCreateOutfit(RE::Actor* player);
    bool PreviewPlayerItems(RE::Actor* player, const std::vector<RE::TESForm*>& items);
    void EndPlayerCreateOutfit(RE::Actor* player);
    void QueuePlayerEquipmentAudit(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items) const;
    // A preview or Edit of an outfit none of whose pieces load: the player's look stays, and a toast says why.
    void KeepPlayerLookForUnloadedOutfit(RE::Actor* player);

    RE::ActorHandle _currentTarget;
    // The NPC the session opened on, for Switch back; the player is a target only when chosen.
    RE::ActorHandle _sessionNpc;
    bool            _playerTarget = false;

    // Scratch pair for the Create/Edit Outfit live preview. Shared is fine here —
    // only one actor is ever being edited, and EndCreateOutfit restores their
    // original outfit when the screen closes.
    OutfitPair _editOutfit;

    // Applied outfits, one pair per actor. Each managed NPC must own its BGSOutfit:
    // npc->defaultOutfit keeps pointing at it indefinitely, so a shared object means
    // dressing one NPC silently rewrites what every other managed NPC's defaultOutfit
    // resolves to. Anything that re-equips an NPC from their default outfit outside
    // Tailor's control (an undress mod redressing, ResetInventory) then dresses them
    // in whoever was dressed last.
    std::unordered_map<RE::FormID, OutfitPair> _actorOutfits;
    std::unordered_map<RE::FormID, DefaultOutfitState> _dataLoadedDefaultOutfits;
    // See IsReturningToOwnOutfit. Runtime only: before a load, PrepareForGameLoad takes Tailor's outfits off
    // everyone, and gives an NPC with no assignment their own.
    std::unordered_set<RE::FormID> _returnToOwnOutfit;
    mutable std::unordered_map<RE::FormID, std::uint64_t> _equipmentAudits;
    mutable std::uint64_t _nextEquipmentAudit = 0;

    RE::BGSOutfit*  _flushOutfit = nullptr;
    RE::BGSOutfit*  _preCreateOutfit = nullptr;
    RE::ActorHandle _createSessionActor;
    bool            _createSessionActive = false;
    bool            _createSessionEnding = false;
    std::uint64_t   _createSessionGeneration = 0;

    // Track original outfits before cycling
    RE::BGSOutfit*               _preCycleOutfit = nullptr;
    RE::BGSOutfit*               _preCycleSleepOutfit = nullptr;
    RE::BGSOutfit*               _preCreateSleepOutfit = nullptr;
    std::vector<RE::TESForm*>     _preCycleOutfitItems;
    bool                         _preCycleStateCaptured = false;
    bool                         _preCycleDefaultWasActorPair = false;
    bool                         _preCycleSleepWasActorPair = false;
    bool                         _preCycleDefaultHadChange = false;
    bool                         _preCycleSleepHadChange = false;
    bool                         _preCreateDefaultHadChange = false;
    bool                         _preCreateSleepHadChange = false;
    std::optional<CycleState>    _cycleState;
    mutable std::recursive_mutex _mutex;
};
