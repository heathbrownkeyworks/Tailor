#include "pch.h"
#include "outfit/OutfitAssignments.h"

namespace
{
    bool EncodeOutfit(
        RE::BGSOutfit* outfit,
        std::string& plugin,
        std::string& localId)
    {
        if (!outfit) return false;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return false;

        const RE::FormID outfitFormId = outfit->GetFormID();
        const uint8_t modIndex = (outfitFormId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex != 0xFE && modIndex != 0xFF) {
            sourceFile = dataHandler->LookupLoadedModByIndex(modIndex);
            localFormId = outfitFormId & 0x00FFFFFF;
        } else if (modIndex == 0xFE) {
            const uint16_t eslIndex = static_cast<uint16_t>((outfitFormId & 0x00FFF000) >> 12);
            sourceFile = dataHandler->LookupLoadedLightModByIndex(eslIndex);
            localFormId = outfitFormId & 0x00000FFF;
        }

        if (!sourceFile) return false;

        plugin = std::string(sourceFile->GetFilename());
        localId = std::format("0x{:X}", localFormId);
        return true;
    }

    RE::BGSOutfit* ResolveOutfit(const std::string& plugin, const std::string& localId)
    {
        if (plugin.empty() || localId.empty()) return nullptr;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        const auto formId = static_cast<RE::FormID>(std::stoul(localId, nullptr, 16));
        auto* form = dataHandler->LookupForm(formId, plugin);
        return form ? form->As<RE::BGSOutfit>() : nullptr;
    }
}

OutfitAssignments& OutfitAssignments::GetSingleton()
{
    static OutfitAssignments singleton;
    return singleton;
}

std::filesystem::path OutfitAssignments::GetFilePath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Tailor");
    std::filesystem::create_directories(path);
    return path / "assignments.json";
}

void OutfitAssignments::Load()
{
    bool needsPrune = false;

    {
        std::lock_guard lock(_mutex);
        _assignments.clear();

        auto path = GetFilePath();
        if (!std::filesystem::exists(path)) {
            logger::info("OutfitAssignments: no assignments.json found, starting empty");
            return;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            logger::error("OutfitAssignments: no TESDataHandler");
            return;
        }

        try {
            std::ifstream file(path);
            auto json = nlohmann::json::parse(file);
            const int schemaVersion = json.value("version", 1);

            if (!json.contains("assignments") || !json["assignments"].is_array()) {
                return;
            }

            for (auto& entry : json["assignments"]) {
                // Parse actor identity (local FormID + plugin)
                std::string actorFormStr = entry.value("actorFormId", std::string{});
                std::string actorPlugin = entry.value("actorPlugin", std::string{});
                int outfitId = entry.value("outfitId", 0);
                int adventuringId = entry.value("adventuringId", 0);
                int townId = entry.value("townId", 0);
                int homeId = entry.value("homeId", 0);
                int sleepId = entry.value("sleepId", 0);
                bool adventuringRandom = entry.value("adventuringRandom", false);
                bool townRandom = entry.value("townRandom", false);
                bool homeRandom = entry.value("homeRandom", false);
                bool sleepRandom = entry.value("sleepRandom", false);

                if (actorFormStr.empty() || actorPlugin.empty()) {
                    needsPrune = true;
                    continue;
                }
                if (outfitId <= 0 && adventuringId <= 0 && townId <= 0 && homeId <= 0 && sleepId <= 0
                    && !adventuringRandom && !townRandom && !homeRandom && !sleepRandom) {
                    needsPrune = true;
                    continue;
                }

                RE::FormID localFormId = static_cast<RE::FormID>(std::stoul(actorFormStr, nullptr, 16));

                // Resolve to runtime FormID
                auto* form = dataHandler->LookupForm(localFormId, actorPlugin);
                if (!form) {
                    logger::warn("OutfitAssignments: actor 0x{:X} from '{}' not found, pruning",
                        localFormId, actorPlugin);
                    needsPrune = true;
                    continue;
                }

                SituationalAssignment sa;
                sa.outfitId = outfitId;
                sa.adventuringId = adventuringId;
                sa.townId = townId;
                sa.homeId = homeId;
                sa.sleepId = sleepId;
                sa.adventuringRandom = adventuringRandom;
                sa.townRandom = townRandom;
                sa.homeRandom = homeRandom;
                sa.sleepRandom = sleepRandom;
                sa.originalOutfitPlugin = entry.value("originalOutfitPlugin", std::string{});
                sa.originalOutfitLocalId = entry.value("originalOutfitLocalId", std::string{});
                sa.originalSleepOutfitPlugin = entry.value("originalSleepOutfitPlugin", std::string{});
                sa.originalSleepOutfitLocalId = entry.value("originalSleepOutfitLocalId", std::string{});
                if (schemaVersion >= 3) {
                    sa.originalDefaultOutfitKnown = entry.value("originalDefaultOutfitKnown", false);
                    sa.originalSleepOutfitKnown = entry.value("originalSleepOutfitKnown", false);
                    sa.originalDefaultChangeStateKnown = entry.value("originalDefaultChangeStateKnown", false);
                    sa.originalSleepChangeStateKnown = entry.value("originalSleepChangeStateKnown", false);
                } else if (schemaVersion == 2 && entry.value("originalOutfitStateCaptured", false)) {
                    // Version 2 was used only by development builds. Its single
                    // capture bit meant both form values and both change bits.
                    sa.originalDefaultOutfitKnown = true;
                    sa.originalSleepOutfitKnown = true;
                    sa.originalDefaultChangeStateKnown = true;
                    sa.originalSleepChangeStateKnown = true;
                } else {
                    // Released v1 knew only a non-null original DOFT. Its SOFT
                    // and change-bit ownership are genuinely unknowable.
                    sa.originalDefaultOutfitKnown =
                        !sa.originalOutfitPlugin.empty() && !sa.originalOutfitLocalId.empty();
                }
                sa.originalDefaultOutfitHadChange = entry.value("originalDefaultOutfitHadChange", false);
                sa.originalSleepOutfitHadChange = entry.value("originalSleepOutfitHadChange", false);
                _assignments[form->GetFormID()] = sa;

                if (outfitId > 0) {
                    logger::info("OutfitAssignments: loaded actor 0x{:X} from '{}' (runtime 0x{:X}) — outfit={}",
                        localFormId, actorPlugin, form->GetFormID(), outfitId);
                } else {
                    logger::info("OutfitAssignments: loaded actor 0x{:X} from '{}' (runtime 0x{:X}) — adv={} town={} home={} sleep={}",
                        localFormId, actorPlugin, form->GetFormID(),
                        adventuringId, townId, homeId, sleepId);
                }
            }

            logger::info(
                "OutfitAssignments: loaded {} assignment(s) (schema v{})",
                _assignments.size(), schemaVersion);
        } catch (const std::exception& e) {
            logger::error("OutfitAssignments: failed to load: {}", e.what());
        }
    }  // mutex released here

    // Prune outside the lock to avoid deadlock
    if (needsPrune) {
        Save();
    }
}

void OutfitAssignments::Save() const
{
    std::lock_guard lock(_mutex);

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    nlohmann::json json;
    json["version"] = 3;
    json["assignments"] = nlohmann::json::array();

    for (auto& [runtimeId, sa] : _assignments) {
        // Skip empty entries (no outfit, no situations)
        if (sa.outfitId <= 0 && !sa.HasAnySituation()) continue;

        auto* form = RE::TESForm::LookupByID(runtimeId);
        if (!form) continue;

        // Decompose runtime FormID to get the correct source plugin
        uint8_t modIndex = (runtimeId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex != 0xFE) {
            // Regular plugin
            sourceFile = dataHandler->LookupLoadedModByIndex(modIndex);
            localFormId = runtimeId & 0x00FFFFFF;
        } else {
            // ESL/light plugin
            uint16_t eslIndex = static_cast<uint16_t>((runtimeId & 0x00FFF000) >> 12);
            sourceFile = dataHandler->LookupLoadedLightModByIndex(eslIndex);
            localFormId = runtimeId & 0x00000FFF;
        }

        if (!sourceFile) {
            logger::warn("OutfitAssignments: could not resolve plugin for runtime 0x{:X}", runtimeId);
            continue;
        }

        std::string plugin(sourceFile->GetFilename());

        nlohmann::json entry;
        entry["actorFormId"] = std::format("0x{:X}", localFormId);
        entry["actorPlugin"] = plugin;
        entry["outfitId"] = sa.outfitId;
        if (sa.adventuringId > 0) entry["adventuringId"] = sa.adventuringId;
        if (sa.townId > 0) entry["townId"] = sa.townId;
        if (sa.homeId > 0) entry["homeId"] = sa.homeId;
        if (sa.sleepId > 0) entry["sleepId"] = sa.sleepId;
        if (sa.adventuringRandom) entry["adventuringRandom"] = true;
        if (sa.townRandom) entry["townRandom"] = true;
        if (sa.homeRandom) entry["homeRandom"] = true;
        if (sa.sleepRandom) entry["sleepRandom"] = true;
        if (!sa.originalOutfitPlugin.empty()) entry["originalOutfitPlugin"] = sa.originalOutfitPlugin;
        if (!sa.originalOutfitLocalId.empty()) entry["originalOutfitLocalId"] = sa.originalOutfitLocalId;
        if (!sa.originalSleepOutfitPlugin.empty()) entry["originalSleepOutfitPlugin"] = sa.originalSleepOutfitPlugin;
        if (!sa.originalSleepOutfitLocalId.empty()) entry["originalSleepOutfitLocalId"] = sa.originalSleepOutfitLocalId;
        if (sa.originalDefaultOutfitKnown) entry["originalDefaultOutfitKnown"] = true;
        if (sa.originalSleepOutfitKnown) entry["originalSleepOutfitKnown"] = true;
        if (sa.originalDefaultChangeStateKnown) {
            entry["originalDefaultChangeStateKnown"] = true;
            entry["originalDefaultOutfitHadChange"] = sa.originalDefaultOutfitHadChange;
        }
        if (sa.originalSleepChangeStateKnown) {
            entry["originalSleepChangeStateKnown"] = true;
            entry["originalSleepOutfitHadChange"] = sa.originalSleepOutfitHadChange;
        }
        json["assignments"].push_back(entry);

        logger::info("OutfitAssignments: saving actor 0x{:X} from '{}' (runtime 0x{:X})",
            localFormId, plugin, runtimeId);
    }

    try {
        auto path = GetFilePath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("OutfitAssignments: failed to open assignments.json for writing");
            return;
        }
        file << json.dump(2);
        file.flush();
        logger::info("OutfitAssignments: saved {} assignment(s)", _assignments.size());
    } catch (const std::exception& e) {
        logger::error("OutfitAssignments: failed to save: {}", e.what());
    }
}

void OutfitAssignments::Assign(RE::FormID actorRuntimeId, int outfitId)
{
    std::lock_guard lock(_mutex);
    _assignments[actorRuntimeId].outfitId = outfitId;
    logger::info("OutfitAssignments: assigned outfit {} to actor 0x{:X}", outfitId, actorRuntimeId);
}

void OutfitAssignments::Unassign(RE::FormID actorRuntimeId)
{
    std::lock_guard lock(_mutex);
    _assignments.erase(actorRuntimeId);
    logger::info("OutfitAssignments: unassigned actor 0x{:X}", actorRuntimeId);
}

bool OutfitAssignments::HasAssignment(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    return _assignments.contains(actorRuntimeId);
}

int OutfitAssignments::GetOutfitId(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    return it != _assignments.end() ? it->second.outfitId : 0;
}

std::unordered_map<RE::FormID, SituationalAssignment> OutfitAssignments::GetAll() const
{
    std::lock_guard lock(_mutex);
    return _assignments;
}

void OutfitAssignments::AssignSituation(RE::FormID actorRuntimeId, OutfitSituation situation, int outfitId)
{
    std::lock_guard lock(_mutex);
    auto& a = _assignments[actorRuntimeId];
    a.SetSlot(situation, outfitId);
    // Clear legacy single-outfit when entering situational mode
    a.outfitId = 0;
    logger::info("OutfitAssignments: assigned situation {} outfit {} to actor 0x{:X}",
        static_cast<int>(situation), outfitId, actorRuntimeId);
}

void OutfitAssignments::ClearSituation(RE::FormID actorRuntimeId, OutfitSituation situation)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return;
    it->second.ClearSlot(situation);
    // If no slots remain, remove the entry entirely
    if (!it->second.HasAnySituation() && it->second.outfitId <= 0) {
        _assignments.erase(it);
    }
    logger::info("OutfitAssignments: cleared situation {} for actor 0x{:X}",
        static_cast<int>(situation), actorRuntimeId);
}

void OutfitAssignments::ClearAllSituations(RE::FormID actorRuntimeId)
{
    std::lock_guard lock(_mutex);
    _assignments.erase(actorRuntimeId);
    logger::info("OutfitAssignments: cleared all situations for actor 0x{:X}", actorRuntimeId);
}

bool OutfitAssignments::HasAnySituation(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    return it != _assignments.end() && it->second.HasAnySituation();
}

int OutfitAssignments::GetSituationOutfitId(RE::FormID actorRuntimeId, OutfitSituation situation) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return 0;
    return it->second.GetSlot(situation);
}

void OutfitAssignments::SetSituationRandom(RE::FormID actorRuntimeId, OutfitSituation situation, bool random)
{
    std::lock_guard lock(_mutex);
    auto& a = _assignments[actorRuntimeId];
    a.SetRandomFlag(situation, random);
    logger::info("OutfitAssignments: set situation {} random={} for actor 0x{:X}",
        static_cast<int>(situation), random, actorRuntimeId);
}

bool OutfitAssignments::GetSituationRandom(RE::FormID actorRuntimeId, OutfitSituation situation) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return false;
    return it->second.GetRandomFlag(situation);
}

const SituationalAssignment* OutfitAssignments::GetAssignment(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    return it != _assignments.end() ? &it->second : nullptr;
}

std::vector<RE::FormID> OutfitAssignments::GetActorsUsingOutfit(int outfitId) const
{
    std::lock_guard lock(_mutex);
    std::vector<RE::FormID> result;
    for (auto& [actorId, sa] : _assignments) {
        if (sa.outfitId == outfitId ||
            sa.adventuringId == outfitId ||
            sa.townId == outfitId ||
            sa.homeId == outfitId ||
            sa.sleepId == outfitId) {
            result.push_back(actorId);
        }
    }
    return result;
}

void OutfitAssignments::RemoveOutfitFromAllAssignments(int outfitId)
{
    {
        std::lock_guard lock(_mutex);
        for (auto it = _assignments.begin(); it != _assignments.end(); ) {
            auto& sa = it->second;
            if (sa.outfitId == outfitId) sa.outfitId = 0;
            if (sa.adventuringId == outfitId) sa.adventuringId = 0;
            if (sa.townId == outfitId) sa.townId = 0;
            if (sa.homeId == outfitId) sa.homeId = 0;
            if (sa.sleepId == outfitId) sa.sleepId = 0;

            if (sa.outfitId <= 0 && !sa.HasAnySituation()) {
                it = _assignments.erase(it);
            } else {
                ++it;
            }
        }
    }
    Save();
    logger::info("OutfitAssignments: removed outfit {} from all assignments", outfitId);
}

bool OutfitAssignments::CaptureOriginalOutfitState(
    RE::FormID actorRuntimeId,
    RE::BGSOutfit* defaultOutfit,
    RE::BGSOutfit* sleepOutfit,
    bool defaultOutfitHadChange,
    bool sleepOutfitHadChange)
{
    return CaptureMissingOriginalOutfitState(
        actorRuntimeId,
        defaultOutfit,
        true,
        sleepOutfit,
        true,
        true,
        defaultOutfitHadChange,
        true,
        sleepOutfitHadChange);
}

bool OutfitAssignments::CaptureMissingOriginalOutfitState(
    RE::FormID actorRuntimeId,
    RE::BGSOutfit* defaultOutfit,
    bool defaultOutfitKnown,
    RE::BGSOutfit* sleepOutfit,
    bool sleepOutfitKnown,
    bool defaultChangeStateKnown,
    bool defaultOutfitHadChange,
    bool sleepChangeStateKnown,
    bool sleepOutfitHadChange)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) {
        return false;
    }

    auto& assignment = it->second;
    bool changed = false;

    auto captureForm = [&changed](
                           RE::BGSOutfit* outfit,
                           bool suppliedKnown,
                           bool& storedKnown,
                           std::string& plugin,
                           std::string& localId) {
        if (!suppliedKnown || storedKnown) return;

        if (!outfit) {
            plugin.clear();
            localId.clear();
            storedKnown = true;
            changed = true;
            return;
        }

        std::string encodedPlugin;
        std::string encodedLocalId;
        if (EncodeOutfit(outfit, encodedPlugin, encodedLocalId)) {
            plugin = std::move(encodedPlugin);
            localId = std::move(encodedLocalId);
            storedKnown = true;
            changed = true;
        }
    };

    captureForm(
        defaultOutfit,
        defaultOutfitKnown,
        assignment.originalDefaultOutfitKnown,
        assignment.originalOutfitPlugin,
        assignment.originalOutfitLocalId);
    captureForm(
        sleepOutfit,
        sleepOutfitKnown,
        assignment.originalSleepOutfitKnown,
        assignment.originalSleepOutfitPlugin,
        assignment.originalSleepOutfitLocalId);

    if (defaultChangeStateKnown && !assignment.originalDefaultChangeStateKnown) {
        assignment.originalDefaultChangeStateKnown = true;
        assignment.originalDefaultOutfitHadChange = defaultOutfitHadChange;
        changed = true;
    }
    if (sleepChangeStateKnown && !assignment.originalSleepChangeStateKnown) {
        assignment.originalSleepChangeStateKnown = true;
        assignment.originalSleepOutfitHadChange = sleepOutfitHadChange;
        changed = true;
    }

    if (changed) {
        logger::info(
            "OutfitAssignments: captured original state for actor 0x{:X} "
            "(defaultKnown={}, sleepKnown={}, defaultChangeKnown={}, sleepChangeKnown={})",
            actorRuntimeId,
            assignment.originalDefaultOutfitKnown,
            assignment.originalSleepOutfitKnown,
            assignment.originalDefaultChangeStateKnown,
            assignment.originalSleepChangeStateKnown);
    }
    return changed;
}

bool OutfitAssignments::GetOriginalDefaultOutfit(
    RE::FormID actorRuntimeId, RE::BGSOutfit*& outfit) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end() || !it->second.originalDefaultOutfitKnown) return false;

    const auto& assignment = it->second;
    if (assignment.originalOutfitPlugin.empty() && assignment.originalOutfitLocalId.empty()) {
        outfit = nullptr;
        return true;
    }

    outfit = ResolveOutfit(assignment.originalOutfitPlugin, assignment.originalOutfitLocalId);
    return outfit != nullptr;
}

bool OutfitAssignments::GetOriginalSleepOutfit(
    RE::FormID actorRuntimeId, RE::BGSOutfit*& outfit) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end() || !it->second.originalSleepOutfitKnown) return false;

    const auto& assignment = it->second;
    if (assignment.originalSleepOutfitPlugin.empty() && assignment.originalSleepOutfitLocalId.empty()) {
        outfit = nullptr;
        return true;
    }

    outfit = ResolveOutfit(
        assignment.originalSleepOutfitPlugin,
        assignment.originalSleepOutfitLocalId);
    return outfit != nullptr;
}

bool OutfitAssignments::GetOriginalDefaultChangeState(
    RE::FormID actorRuntimeId, bool& hadChange) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end() || !it->second.originalDefaultChangeStateKnown) return false;

    hadChange = it->second.originalDefaultOutfitHadChange;
    return true;
}

bool OutfitAssignments::GetOriginalSleepChangeState(
    RE::FormID actorRuntimeId, bool& hadChange) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end() || !it->second.originalSleepChangeStateKnown) return false;

    hadChange = it->second.originalSleepOutfitHadChange;
    return true;
}

bool OutfitAssignments::GetOriginalOutfitChangeState(
    RE::FormID actorRuntimeId,
    bool& defaultOutfitHadChange,
    bool& sleepOutfitHadChange) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end() ||
        !it->second.originalDefaultChangeStateKnown ||
        !it->second.originalSleepChangeStateKnown) {
        return false;
    }

    defaultOutfitHadChange = it->second.originalDefaultOutfitHadChange;
    sleepOutfitHadChange = it->second.originalSleepOutfitHadChange;
    return true;
}
