#pragma once

#include "outfit/ArmorItem.h"
#include "outfit/CustomOutfit.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"

#include <mutex>
#include <optional>
#include <unordered_map>

class OutfitManager
{
public:
    static OutfitManager& GetSingleton();

    void Initialize();

    void        UpdateTargetFromCrosshair();
    RE::Actor*  GetTarget() const;
    std::string GetTargetName() const;
    std::string GetTargetSex() const;

    // --- Create Outfit: dynamic outfit preview via OTFT ---
    void BeginCreateOutfit(RE::Actor* actor);
    void EndCreateOutfit(RE::Actor* actor);
    void AddItemToCreateOutfit(RE::Actor* actor, const ArmorItem& item);
    void RemoveItemFromCreateOutfit(RE::Actor* actor, const ArmorItem& item);
    void LoadCreateOutfitItems(RE::Actor* actor, const std::vector<ArmorItem>& items);

    // --- Cycling custom outfits ---
    struct CycleState
    {
        int              categoryId = 0;
        int              index = 0;
        std::vector<int> outfitIds;   // custom outfit IDs
    };

    bool StartCycle(int categoryId);
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

    void ReApplyAllAssignments();
    void PrepareForGameLoad();

    // Re-equip wig after an outfit change on this actor
    static void NotifyOutfitChanged(RE::Actor* actor);

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

    bool IsValidTarget(RE::Actor* actor) const;
    bool SetActorDefaultOutfit(RE::Actor* actor, RE::BGSOutfit* outfit, bool update3D) const;
    bool SetActorSleepOutfit(RE::Actor* actor, RE::BGSOutfit* outfit) const;
    static bool GetOutfitChangeFlag(RE::TESNPC* npc, std::uint32_t flag);
    static void SuppressOutfitChangeFlags(RE::TESNPC* npc);
    static void RestoreOutfitChangeFlags(RE::TESNPC* npc, bool defaultHadChange, bool sleepHadChange);
    void CapturePersistedOutfitStateIfNeeded(RE::Actor* actor);
    void InitFlushOutfit();
    bool InitOutfitPair(OutfitPair& pair);
    OutfitPair* GetOrCreateActorOutfit(RE::FormID actorId);
    bool FlushAndApplyOutfit(RE::Actor* actor, OutfitPair& pair);
    bool RestoreCycleSnapshot(RE::Actor* actor);

    RE::ActorHandle _currentTarget;

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

    RE::BGSOutfit*  _flushOutfit = nullptr;
    RE::BGSOutfit*  _preCreateOutfit = nullptr;
    RE::ActorHandle _createSessionActor;
    bool            _createSessionActive = false;

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
