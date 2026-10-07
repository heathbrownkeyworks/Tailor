#pragma once

#include "wig/WigAssignments.h"
#include "wig/WigCategory.h"
#include "wig/WigLibrary.h"
#include "wig/WigRecoveryPolicy.h"
#include "wig/HeadwearEquipment.h"

#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

struct CycleState
{
    WigCategory category;
    int32_t     index = 0;
    std::vector<WigEntry> wigs;
    WigEntry    originalWig;
    bool        hadOriginal = false;
};

struct PreviewState
{
    WigEntry originalWig;
    bool     hadOriginal = false;
};

struct InventoryWig
{
    RE::FormID  formId;
    std::string plugin;
    std::string name;
};

struct ModWigList
{
    std::string              modName;
    std::vector<InventoryWig> wigs;
};

class WigManager : public RE::BSTEventSink<RE::TESEquipEvent>,
                   public RE::BSTEventSink<RE::TESContainerChangedEvent>
{
public:
    static WigManager& GetSingleton();

    void Initialize();

    // Disable and cancel queued recovery before loading; enable only once ready.
    void SetRecoveryEnabled(bool enabled);

    // Target — set by TailorUI when opening, delegates to OutfitManager
    bool       SetTarget(RE::Actor* actor);
    RE::Actor* GetTarget() const;

    // NFF (Nether's Follower Framework) compatibility
    bool IsNFFLoaded() const { return _nffLoaded; }
    bool IsNFFManaged(RE::Actor* actor) const;

    // Wig operations
    bool EquipWig(RE::Actor* actor, const WigEntry& wig);
    bool ResetWig(RE::Actor* actor);
    // Sleep is over and no other wig is due: the worn wig comes off for the actor's own hair, which
    // keeps its color. Unlike Default Hair, nothing else is cleared.
    bool TakeOffSituationWig(RE::Actor* actor);
    // Explicit user action; internal preview cancellation keeps using ResetWig.
    bool ResetToDefaultHair(RE::Actor* actor);
    bool SetWigScreen(bool enabled);
    bool IsWigScreen(RE::Actor* actor) const;
    bool PrepareForOutfitChange(RE::Actor* actor, const std::vector<RE::TESForm*>& items);
    void ClearHeadwearForGameLoad();
    bool HasPendingHeadwear(RE::Actor* actor) const;
    bool RestorePendingHeadwear(RE::Actor* actor);

    // Preview — temporary equip for Add Wig browsing
    void StartPreview();
    bool PreviewWig(RE::Actor* actor, const WigEntry& wig);
    void EndPreview();
    bool IsPreviewing() const;

    // Cycling
    bool                    StartCycling(RE::Actor* actor, WigCategory category);
    std::optional<WigEntry> CycleNext();
    std::optional<WigEntry> CyclePrev();
    std::optional<WigEntry> CycleToIndex(int32_t index);
    std::optional<WigEntry> GetCurrentCycleWig() const;
    int32_t                 GetCycleIndex() const;
    int32_t                 GetCycleCount() const;
    std::vector<WigEntry>   GetCycleWigs() const;
    void                    ConfirmCycle();
    void                    ConfirmCycle(OutfitSituation situation);
    void                    CancelCycle();
    bool                    IsCycling() const;

    // Mod scanning — finds all hair-slot armor records grouped by plugin
    std::vector<ModWigList> ScanAllModWigs() const;

    // Process deferred RemoveItem calls for an actor whose cell is detaching.
    void ProcessDeferredRemoval(RE::Actor* actor);

    // Re-equip all on game load
    void ReEquipAllAssignments();
    // After a load, after the player's outfit: their wig goes back on unless headgear hides
    // it, and their hair is re-tinted.
    void ReconcilePlayerWig();

    // Re-apply all hair colors (called on cell change)
    void ReApplyAllHairColors();

    // Called by OutfitManager after outfit changes — re-equips wig + hair color
    void ReEquipWigAfterOutfitChange(RE::Actor* actor);

    // Hair color operations
    bool ConfirmHairColor(RE::Actor* actor, uint8_t r, uint8_t g, uint8_t b);
    bool ApplyHairColor(RE::Actor* actor, uint8_t r, uint8_t g, uint8_t b);
    bool ResetHairColor(RE::Actor* actor);
    void ReApplyHairColor(RE::Actor* actor);
    void DeferHairColor(RE::ActorHandle handle, uint8_t r, uint8_t g, uint8_t b, int32_t delaySecs);
    void ScheduleHairColorDefers(RE::ActorHandle handle, uint8_t r, uint8_t g, uint8_t b,
                                 std::initializer_list<int32_t> delays);

    // Per-actor wig/hair retint — clones each hair-tint material so colors never
    // bleed across NPCs sharing a wig FormID. Tints ALL hair geometry on the actor
    // (scalp, wig, wig sub-shapes, brows/beard) to the resolved per-actor color.
    void RetintActorHair(RE::Actor* actor);
    void ScheduleActorHairRetint(RE::ActorHandle handle, std::initializer_list<int32_t> delays);
    // The game rebuilds the player's hair model on armor changes, cell loads and game loads;
    // Tailor's color goes back on after. Without a Tailor color the game's own color stays.
    void SchedulePlayerHairRetint();

    // Re-split shared hair materials on every other loaded actor. Heals bleed already
    // baked into a save by an older build (or by another mod calling UpdateHairColor)
    // without waiting for a cell reload. Cheap: RetintActorHair no-ops when the color
    // already matches.
    void RetintNearbyActors(RE::Actor* except);

    // Patch ArmorAddon race list so wigs with missing races still render.
    static void EnsureArmorAddonRace(RE::TESObjectARMO* armor, RE::TESRace* race, RE::SEX sex);

    // Get the actor's effective inventory count for an item.
    static int32_t GetActorItemCount(RE::Actor* actor, RE::TESBoundObject* item);

private:
    WigManager() = default;

    void RemoveCurrentWig(RE::Actor* actor);

    // The player's half (WigManagerPlayer.cpp): the player's wig goes on and comes off
    // through PlayerWardrobe and is never locked, so the player may take it off.
    bool EquipPlayerWig(RE::Actor* player, const WigEntry& wig);
    bool ResetPlayerWig(RE::Actor* player);
    void ReEquipPlayerWig(RE::Actor* player);
    void ConfirmPlayerSituationCycle(RE::Actor* player, OutfitSituation situation, const WigEntry& wig);
    bool ApplyPlayerHairColor(RE::Actor* player);
    bool ResetPlayerHairColor(RE::Actor* player);
    bool ResetPlayerToDefaultHair(RE::Actor* player);
    void QueuePlayerWigRecovery(RE::Actor* player, RE::FormID changedArmorId, bool equipped, const char* eventName);
    void RecoverPlayerWig(Tailor::Wigs::WigRecoveryPolicy::Request request, bool takenOffInMenu);

    void ClearActiveHeadwear();
    void DeferHeadwearRestore();
    RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* event,
        RE::BSTEventSource<RE::TESEquipEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) override;
    void QueueWigRecovery(RE::Actor* actor, RE::FormID changedArmorId, const char* eventName);
    void RecoverWig(RE::ActorHandle handle, Tailor::Wigs::WigRecoveryPolicy::Request request);

    // Resolve the per-actor hair tint: custom color if set, else the NPC's natural
    // base-record color. Returns false (no-op) if neither. NiColor float space (8-bit / 128).
    bool ResolveHairTint(RE::Actor* actor, RE::NiColor& out) const;

    RE::ActorHandle _currentTarget;

    std::optional<CycleState>    _cycleState;
    std::optional<PreviewState>  _previewState;
    mutable std::recursive_mutex _mutex;
    Tailor::Wigs::WigRecoveryPolicy _wigRecovery;
    Tailor::Wigs::HeadwearPreview _headwearPreview;
    RE::ActorHandle _headwearActor;
    bool _wigScreen = false;
    // The player took their wig off in an inventory menu; it stays off until Tailor next changes it.
    bool _playerWigLeftOff = false;
    struct PendingHeadwear
    {
        Tailor::Wigs::HeadwearPreview headwear;
        std::optional<PreviewState> original;
    };
    std::unordered_map<RE::FormID, PendingHeadwear> _pendingHeadwear;

    // Kept across game loads on purpose, unlike the outfit forms PrepareForGameLoad drops: tested in
    // game, each colour form survives a load, even of a save older than the form, at the same
    // address, under the same ID that no new form takes, and with the NPC record still on it.
    std::unordered_map<RE::FormID, RE::BGSColorForm*> _originalHairColors;
    std::unordered_map<RE::FormID, RE::BGSColorForm*> _cachedColorForms;
    std::unordered_map<RE::FormID, uint32_t>           _hairColorGeneration;
    std::unordered_map<RE::FormID, uint32_t>           _hairRetintGeneration;

    // NFF detection — cached on Initialize()
    bool              _nffLoaded = false;
    RE::TESFaction*   _nffStoredFac = nullptr;
    RE::TESFaction*   _nffOutfitFac = nullptr;
};
