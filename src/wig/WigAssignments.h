#pragma once

#include "outfit/OutfitAssignments.h"
#include "wig/WigCategory.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>

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

    bool HasHairColor() const { return hairColorR >= 0; }
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

class WigAssignments
{
public:
    static WigAssignments& GetSingleton();

    void Load();
    void Save() const;

    void SetAssignment(RE::FormID actorFormId, const WigEntry& wig, bool itemAdded);
    void MarkItemAdded(RE::FormID actorFormId);
    void ClearAssignment(RE::FormID actorFormId);
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

private:
    WigAssignments() = default;

    std::filesystem::path GetAssignmentsPath() const;
    std::filesystem::path GetSituationsPath() const;

    std::unordered_map<RE::FormID, ActorWigState> _assignments;
    std::unordered_map<RE::FormID, WigSituationalAssignment> _situations;
    mutable std::mutex _mutex;
};
