#pragma once

#include "outfit/OutfitAssignments.h"
#include "wig/WigCategory.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

struct ActorWigState
{
    WigEntry currentWig;
    // True when Tailor added the wig item to the actor's inventory itself.
    // Guards removal on wig switch/reset: a copy the user gave the NPC
    // outside Tailor must never be taken back.
    bool     itemAdded = false;
    int16_t  hairColorR = -1;  // -1 = no override, 0-255 = active
    int16_t  hairColorG = -1;
    int16_t  hairColorB = -1;
    // The wig set in the Hair Dresser, which situation wigs never change: when Sleep ends and no
    // situation wig is due, it goes on again. Last, so positional initializers stay valid.
    WigEntry assignedWig;

    bool HasHairColor() const { return hairColorR >= 0; }
    // Nothing left to keep: no worn wig, no hair color and no assigned wig. An empty row is erased.
    bool IsEmpty() const { return currentWig.formId == 0 && !HasHairColor() && assignedWig.formId == 0; }
};

struct WigSituationalAssignment
{
    WigEntry adventuring;  // situation 1
    WigEntry town;         // situation 2
    WigEntry home;         // situation 3
    WigEntry sleep;        // situation 4

    bool HasAnySituation() const {
        return adventuring.formId != 0 || town.formId != 0 ||
               home.formId != 0 || sleep.formId != 0;
    }

    // Whether a wig is one of these situation wigs: the same plugin and ID in any slot.
    bool Holds(const WigEntry& wig) const {
        return wig.formId != 0 && (adventuring == wig || town == wig || home == wig || sleep == wig);
    }

    WigEntry GetSlot(OutfitSituation s) const {
        switch (s) {
        case OutfitSituation::Adventuring: return adventuring;
        case OutfitSituation::Town:        return town;
        case OutfitSituation::Home:        return home;
        case OutfitSituation::Sleep:       return sleep;
        }
        return {};
    }

    void SetSlot(OutfitSituation s, const WigEntry& wig) {
        switch (s) {
        case OutfitSituation::Adventuring: adventuring = wig; break;
        case OutfitSituation::Town:        town = wig; break;
        case OutfitSituation::Home:        home = wig; break;
        case OutfitSituation::Sleep:       sleep = wig; break;
        }
    }

    void ClearSlot(OutfitSituation s) { SetSlot(s, {}); }

    void ClearAll() {
        adventuring = {};
        town = {};
        home = {};
        sleep = {};
    }
};

// The player's wig rows: each save's co-save holds them, never the Wiggy files.
struct PlayerWigRow
{
    std::optional<ActorWigState> state;
    std::optional<WigSituationalAssignment> situations;
};

class WigAssignments
{
public:
    static WigAssignments& GetSingleton();

    void Load();
    void Save() const;

    void SetAssignment(RE::FormID actorFormId, const WigEntry& wig, bool itemAdded);
    void MarkItemAdded(RE::FormID actorFormId);
    void ClearAssignment(RE::FormID actorFormId);
    // The Hair Dresser's wig; an empty one clears it.
    void SetAssignedWig(RE::FormID actorFormId, const WigEntry& wig);
    std::optional<WigEntry> GetAssignment(RE::FormID actorFormId) const;
    bool HasAssignment(RE::FormID actorFormId) const;

    void SetHairColor(RE::FormID actorFormId, int16_t r, int16_t g, int16_t b);
    void ClearHairColor(RE::FormID actorFormId);
    std::optional<ActorWigState> GetState(RE::FormID actorFormId) const;

    std::unordered_map<RE::FormID, ActorWigState> GetAll() const;

    void Clear();

    // Situational wig assignments
    void AssignSituation(RE::FormID actorFormId, OutfitSituation situation, const WigEntry& wig);
    void ClearSituation(RE::FormID actorFormId, OutfitSituation situation);
    void ClearAllSituations(RE::FormID actorFormId);
    bool HasAnySituation(RE::FormID actorFormId) const;
    WigEntry GetSituationWig(RE::FormID actorFormId, OutfitSituation situation) const;
    const WigSituationalAssignment* GetSituationalAssignment(RE::FormID actorFormId) const;
    std::unordered_map<RE::FormID, WigSituationalAssignment> GetAllSituational() const;
    void LoadSituations();
    void SaveSituations() const;
    // Rows saved before the assigned wig was kept: once both files load in full, a worn wig that isn't
    // one of the actor's situation wigs is taken as the one set in the Hair Dresser. Until then the
    // rows wait, and are saved without the field.
    void InferAssignedWigs();

    // The player's rows for the co-save; the JSON files never hold them.
    PlayerWigRow ExportPlayer() const;
    void ImportPlayer(const PlayerWigRow& row);

private:
    WigAssignments() = default;

    std::filesystem::path GetAssignmentsPath() const;
    std::filesystem::path GetSituationsPath() const;

    std::unordered_map<RE::FormID, ActorWigState> _assignments;
    std::unordered_map<RE::FormID, WigSituationalAssignment> _situations;
    nlohmann::json _retainedAssignments = nlohmann::json::array();
    nlohmann::json _retainedSituations = nlohmann::json::array();
    bool _saveAssignmentsAllowed = false;
    bool _saveSituationsAllowed = false;
    std::unordered_set<RE::FormID> _inferAssignedWig;  // rows loaded without the assigned wig field, until inferred or set
    mutable std::mutex _mutex;
};
