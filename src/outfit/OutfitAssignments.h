#pragma once

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "outfit/OutfitArmorType.h"

enum class OutfitSituation : int { Adventuring = 1, Town = 2, Home = 3, Sleep = 4, Swimming = 5, Warm = 6 };
// The highest situation number: saved data and UI actions number situations 1 to this.
inline constexpr int kLastOutfitSituation = 6;

struct SituationalAssignment {
    int outfitId = 0;       // generic outfit, retained as the final situation fallback (0 = not set)
    int adventuringId = 0;  // dungeon/wilderness
    int townId = 0;         // city/settlement/inn
    int homeId = 0;         // player house; for NPCs any house
    int sleepId = 0;        // sleeping in bed
    int swimmingId = 0;     // temporary outfit while in water
    int warmId = 0;         // outdoors in cold weather or a snowy region

    // Randomize flags — when true, pick random outfit from situation category each day
    bool adventuringRandom = false;
    bool townRandom = false;
    bool homeRandom = false;
    bool sleepRandom = false;
    bool swimmingRandom = false;
    bool warmRandom = false;
    OutfitArmorType adventuringArmorType = OutfitArmorType::Any;

    // Original outfit before Tailor modification (for Default Outfit restore)
    std::string originalOutfitPlugin;
    std::string originalOutfitLocalId;  // hex string e.g. "0x10C7B6"
    std::string originalSleepOutfitPlugin;
    std::string originalSleepOutfitLocalId;
    // Form state is tri-state: unknown, known-null, or a known plugin-backed
    // outfit. Empty plugin/ID plus `Known=true` represents a real null.
    bool originalDefaultOutfitKnown = false;
    bool originalSleepOutfitKnown = false;
    bool originalDefaultChangeStateKnown = false;
    bool originalSleepChangeStateKnown = false;
    bool originalDefaultOutfitHadChange = false;
    bool originalSleepOutfitHadChange = false;
    // Her last outfit was deleted. She returns to the Default Outfit when the outfit
    // re-equip queue reaches her, right after the deletion if she is loaded, else when
    // she next loads; until then this entry survives so her original-outfit state does.
    bool restoreDefaultPending = false;

    bool HasAnySituation() const {
        return adventuringId > 0 || townId > 0 || homeId > 0 || sleepId > 0 || swimmingId > 0 || warmId > 0
            || adventuringRandom || townRandom || homeRandom || sleepRandom || swimmingRandom || warmRandom;
    }

    bool HasOutfits() const { return outfitId > 0 || HasAnySituation(); }
    bool HasSettings() const { return HasOutfits() || adventuringArmorType != OutfitArmorType::Any || restoreDefaultPending; }

    // Takes a deleted outfit out of the generic slot and every situation.
    void RemoveOutfit(int id) {
        for (int* slot : {&outfitId, &adventuringId, &townId, &homeId, &sleepId, &swimmingId, &warmId}) {
            if (*slot == id) *slot = 0;
        }
    }

    // `wearable` is what the NPC may wear at all (it exists and fits the NPC's sex);
    // `allowed` further limits the Adventuring slot to the armor type. An outfit that
    // fails either counts as unassigned, so the next link in the chain answers. Sleep
    // without an outfit that answers falls back to `besidesSleep`, the situation the NPC
    // would be in out of bed; Warm without one (Warm itself, or Warm as `besidesSleep`) to
    // `location`, the situation by location beneath it; Home without one to Town; then the Adventuring outfit and
    // the regular outfit. Each slot is tried once. The saved slots never change.
    template <class RandomResolver, class AdventuringFilter, class WearableFilter>
    int ResolveOutfit(OutfitSituation situation, RandomResolver&& resolveRandom, AdventuringFilter&& allowed, WearableFilter&& wearable,
        OutfitSituation besidesSleep = OutfitSituation::Adventuring, OutfitSituation location = OutfitSituation::Adventuring) const {
        auto resolveSlot = [&](OutfitSituation slot) {
            const int selected = GetRandomFlag(slot) ? resolveRandom(slot) : GetSlot(slot);
            if (selected <= 0 || !wearable(selected)) return 0;
            return slot == OutfitSituation::Adventuring && !allowed(selected) ? 0 : selected;
        };
        const auto next = situation == OutfitSituation::Sleep ? besidesSleep : situation;
        const auto beneathWarm = next == OutfitSituation::Warm ? location : next;
        const auto afterHome = beneathWarm == OutfitSituation::Home ? OutfitSituation::Town : beneathWarm;
        const OutfitSituation chain[] = {situation, next, beneathWarm, afterHome, OutfitSituation::Adventuring};
        for (std::size_t i = 0; i < std::size(chain); ++i) {
            if (std::find(chain, chain + i, chain[i]) != chain + i) continue;
            if (const int selected = resolveSlot(chain[i]); selected > 0) return selected;
        }
        return outfitId > 0 && wearable(outfitId) ? outfitId : 0;
    }

    template <class RandomResolver, class AdventuringFilter>
    int ResolveOutfit(OutfitSituation situation, RandomResolver&& resolveRandom, AdventuringFilter&& allowed) const {
        return ResolveOutfit(situation, resolveRandom, allowed, [](int) { return true; });
    }

    template <class RandomResolver>
    int ResolveOutfit(OutfitSituation situation, RandomResolver&& resolveRandom) const {
        return ResolveOutfit(situation, resolveRandom, [](int) { return true; });
    }

    int GetSlot(OutfitSituation s) const {
        switch (s) {
        case OutfitSituation::Adventuring: return adventuringId;
        case OutfitSituation::Town:        return townId;
        case OutfitSituation::Home:        return homeId;
        case OutfitSituation::Sleep:       return sleepId;
        case OutfitSituation::Swimming:    return swimmingId;
        case OutfitSituation::Warm:        return warmId;
        }
        return 0;
    }

    void SetSlot(OutfitSituation s, int id) {
        switch (s) {
        case OutfitSituation::Adventuring: adventuringId = id; break;
        case OutfitSituation::Town:        townId = id; break;
        case OutfitSituation::Home:        homeId = id; break;
        case OutfitSituation::Sleep:       sleepId = id; break;
        case OutfitSituation::Swimming:    swimmingId = id; break;
        case OutfitSituation::Warm:        warmId = id; break;
        }
    }

    void ClearSlot(OutfitSituation s) {
        SetSlot(s, 0);
        SetRandomFlag(s, false);
    }

    bool GetRandomFlag(OutfitSituation s) const {
        switch (s) {
        case OutfitSituation::Adventuring: return adventuringRandom;
        case OutfitSituation::Town:        return townRandom;
        case OutfitSituation::Home:        return homeRandom;
        case OutfitSituation::Sleep:       return sleepRandom;
        case OutfitSituation::Swimming:    return swimmingRandom;
        case OutfitSituation::Warm:        return warmRandom;
        }
        return false;
    }

    void SetRandomFlag(OutfitSituation s, bool val) {
        switch (s) {
        case OutfitSituation::Adventuring: adventuringRandom = val; break;
        case OutfitSituation::Town:        townRandom = val; break;
        case OutfitSituation::Home:        homeRandom = val; break;
        case OutfitSituation::Sleep:       sleepRandom = val; break;
        case OutfitSituation::Swimming:    swimmingRandom = val; break;
        case OutfitSituation::Warm:        warmRandom = val; break;
        }
    }

    void ClearSituations() {
        for (auto situation : {OutfitSituation::Adventuring, OutfitSituation::Town,
                 OutfitSituation::Home, OutfitSituation::Sleep, OutfitSituation::Swimming, OutfitSituation::Warm}) {
            ClearSlot(situation);
        }
    }

    void ClearAll() {
        outfitId = 0;
        adventuringId = 0;
        townId = 0;
        homeId = 0;
        sleepId = 0;
        swimmingId = 0;
        warmId = 0;
        adventuringRandom = false;
        townRandom = false;
        homeRandom = false;
        sleepRandom = false;
        swimmingRandom = false;
        warmRandom = false;
    }
};

class OutfitAssignments
{
public:
    static OutfitAssignments& GetSingleton();

    void Load();
    void Save() const;
    // Whether the last load read the whole file, so changes can be saved. Edits are never refused (the 09-26 rule);
    // only deleting an outfit asks, since it changes this file too.
    bool SaveAllowed() const;

    void Assign(RE::FormID actorRuntimeId, int outfitId);
    void Unassign(RE::FormID actorRuntimeId);
    bool HasAssignment(RE::FormID actorRuntimeId) const;
    int  GetOutfitId(RE::FormID actorRuntimeId) const;

    // Active NPC assignments only; preference-only NPCs must not enter equipment/OBody
    // work, and neither does the player, whose outfits are dressed through the inventory.
    std::unordered_map<RE::FormID, SituationalAssignment> GetAll() const;

    // The player's row lives here like any NPC's, so every shared flow reads it, but
    // each save's co-save holds it, never assignments.json.
    std::optional<SituationalAssignment> ExportPlayer() const;
    void ImportPlayer(const std::optional<SituationalAssignment>& assignment);

    // Situational assignment methods
    void AssignSituation(RE::FormID actorRuntimeId, OutfitSituation situation, int outfitId);
    void ClearSituation(RE::FormID actorRuntimeId, OutfitSituation situation);
    void ClearAllSituations(RE::FormID actorRuntimeId);
    bool HasAnySituation(RE::FormID actorRuntimeId) const;
    int  GetSituationOutfitId(RE::FormID actorRuntimeId, OutfitSituation situation) const;
    const SituationalAssignment* GetAssignment(RE::FormID actorRuntimeId) const;

    // Randomize flag per situation slot
    void SetSituationRandom(RE::FormID actorRuntimeId, OutfitSituation situation, bool random);
    bool GetSituationRandom(RE::FormID actorRuntimeId, OutfitSituation situation) const;
    void SetAdventuringArmorType(RE::FormID actorRuntimeId, OutfitArmorType type);
    OutfitArmorType GetAdventuringArmorType(RE::FormID actorRuntimeId) const;

    // Original outfit tracking (for Default Outfit restore)
    bool CaptureOriginalOutfitState(
        RE::FormID actorRuntimeId,
        RE::BGSOutfit* defaultOutfit,
        RE::BGSOutfit* sleepOutfit,
        bool defaultOutfitHadChange,
        bool sleepOutfitHadChange);
    bool CaptureMissingOriginalOutfitState(
        RE::FormID actorRuntimeId,
        RE::BGSOutfit* defaultOutfit,
        bool defaultOutfitKnown,
        RE::BGSOutfit* sleepOutfit,
        bool sleepOutfitKnown,
        bool defaultChangeStateKnown,
        bool defaultOutfitHadChange,
        bool sleepChangeStateKnown,
        bool sleepOutfitHadChange);
    // Replaces whatever was captured. Used when the NPC's own plugin record is
    // readable: that record, not the outfit found on the NPC, is the original.
    bool SetOriginalOutfitState(
        RE::FormID actorRuntimeId,
        RE::BGSOutfit* defaultOutfit,
        RE::BGSOutfit* sleepOutfit,
        bool defaultOutfitHadChange,
        bool sleepOutfitHadChange);
    bool GetOriginalDefaultOutfit(RE::FormID actorRuntimeId, RE::BGSOutfit*& outfit) const;
    bool GetOriginalSleepOutfit(RE::FormID actorRuntimeId, RE::BGSOutfit*& outfit) const;
    bool GetOriginalDefaultChangeState(RE::FormID actorRuntimeId, bool& hadChange) const;
    bool GetOriginalSleepChangeState(RE::FormID actorRuntimeId, bool& hadChange) const;
    bool GetOriginalOutfitChangeState(
        RE::FormID actorRuntimeId,
        bool& defaultOutfitHadChange,
        bool& sleepOutfitHadChange) const;

    // Query and cleanup for outfit deletion
    std::vector<RE::FormID> GetActorsUsingOutfit(int outfitId) const;
    // Every slot of every NPC. One of `restoreLater` who is left with no outfit
    // keeps her entry, marked to return to the Default Outfit when the outfit
    // re-equip queue reaches her.
    void RemoveOutfitFromAllAssignments(int outfitId, const std::vector<RE::FormID>& restoreLater = {});
    bool IsRestoreDefaultPending(RE::FormID actorRuntimeId) const;
    void ClearRestoreDefaultPending(RE::FormID actorRuntimeId);

private:
    OutfitAssignments() = default;

    std::filesystem::path GetFilePath() const;

    std::unordered_map<RE::FormID, SituationalAssignment> _assignments;
    nlohmann::json _retainedEntries = nlohmann::json::array();
    bool _saveAllowed = false;  // A failed/incomplete load must never replace the file.
    mutable std::mutex                                    _mutex;
};
