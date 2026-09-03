#pragma once

#include <filesystem>
#include <mutex>
#include <unordered_map>

enum class OutfitSituation : int { Adventuring = 1, Town = 2, Home = 3, Sleep = 4 };

struct SituationalAssignment {
    int outfitId = 0;       // legacy single-outfit (0 = not set)
    int adventuringId = 0;  // dungeon/wilderness
    int townId = 0;         // city/settlement/inn
    int homeId = 0;         // player house
    int sleepId = 0;        // sleeping in bed

    // Randomize flags — when true, pick random outfit from situation category each day
    bool adventuringRandom = false;
    bool townRandom = false;
    bool homeRandom = false;
    bool sleepRandom = false;

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

    bool HasAnySituation() const {
        return adventuringId > 0 || townId > 0 || homeId > 0 || sleepId > 0
            || adventuringRandom || townRandom || homeRandom || sleepRandom;
    }

    int GetSlot(OutfitSituation s) const {
        switch (s) {
        case OutfitSituation::Adventuring: return adventuringId;
        case OutfitSituation::Town:        return townId;
        case OutfitSituation::Home:        return homeId;
        case OutfitSituation::Sleep:       return sleepId;
        }
        return 0;
    }

    void SetSlot(OutfitSituation s, int id) {
        switch (s) {
        case OutfitSituation::Adventuring: adventuringId = id; break;
        case OutfitSituation::Town:        townId = id; break;
        case OutfitSituation::Home:        homeId = id; break;
        case OutfitSituation::Sleep:       sleepId = id; break;
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
        }
        return false;
    }

    void SetRandomFlag(OutfitSituation s, bool val) {
        switch (s) {
        case OutfitSituation::Adventuring: adventuringRandom = val; break;
        case OutfitSituation::Town:        townRandom = val; break;
        case OutfitSituation::Home:        homeRandom = val; break;
        case OutfitSituation::Sleep:       sleepRandom = val; break;
        }
    }

    void ClearAll() {
        outfitId = 0;
        adventuringId = 0;
        townId = 0;
        homeId = 0;
        sleepId = 0;
        adventuringRandom = false;
        townRandom = false;
        homeRandom = false;
        sleepRandom = false;
    }
};

class OutfitAssignments
{
public:
    static OutfitAssignments& GetSingleton();

    void Load();
    void Save() const;

    void Assign(RE::FormID actorRuntimeId, int outfitId);
    void Unassign(RE::FormID actorRuntimeId);
    bool HasAssignment(RE::FormID actorRuntimeId) const;
    int  GetOutfitId(RE::FormID actorRuntimeId) const;

    // Returns all assignments: actor runtime FormID -> SituationalAssignment
    std::unordered_map<RE::FormID, SituationalAssignment> GetAll() const;

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
    void RemoveOutfitFromAllAssignments(int outfitId);

private:
    OutfitAssignments() = default;

    std::filesystem::path GetFilePath() const;

    std::unordered_map<RE::FormID, SituationalAssignment> _assignments;
    mutable std::mutex                                    _mutex;
};
