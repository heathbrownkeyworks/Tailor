#pragma once

#include "wig/WigAssignments.h"
#include "wig/WigCategory.h"
#include "wig/WigLibrary.h"

#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

struct CycleState
{
    WigCategory category;
    int32_t     index = 0;
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

class WigManager
{
public:
    static WigManager& GetSingleton();

    void Initialize();

    // Target — set by TailorUI when opening, delegates to OutfitManager
    void       SetTarget(RE::Actor* actor);
    RE::Actor* GetTarget() const;

    // NFF (Nether's Follower Framework) compatibility
    bool IsNFFLoaded() const { return _nffLoaded; }
    bool IsNFFManaged(RE::Actor* actor) const;

    // Wig operations
    bool EquipWig(RE::Actor* actor, const WigEntry& wig);
    bool ResetWig(RE::Actor* actor);

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

    // Re-apply all hair colors (called on cell change)
    void ReApplyAllHairColors();

    // Called by OutfitManager after outfit changes — re-equips wig + hair color
    void ReEquipWigAfterOutfitChange(RE::Actor* actor);

    // Hair color operations
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

    // Resolve the per-actor hair tint: custom color if set, else the NPC's natural
    // base-record color. Returns false (no-op) if neither. NiColor float space (8-bit / 128).
    bool ResolveHairTint(RE::Actor* actor, RE::NiColor& out) const;

    RE::ActorHandle _currentTarget;

    std::optional<CycleState>    _cycleState;
    std::optional<PreviewState>  _previewState;
    mutable std::recursive_mutex _mutex;

    std::unordered_map<RE::FormID, RE::BGSColorForm*> _originalHairColors;
    std::unordered_map<RE::FormID, RE::BGSColorForm*> _cachedColorForms;
    std::unordered_map<RE::FormID, uint32_t>           _hairColorGeneration;
    std::unordered_map<RE::FormID, uint32_t>           _hairRetintGeneration;

    // NFF detection — cached on Initialize()
    bool              _nffLoaded = false;
    RE::TESFaction*   _nffStoredFac = nullptr;
    RE::TESFaction*   _nffOutfitFac = nullptr;
};
