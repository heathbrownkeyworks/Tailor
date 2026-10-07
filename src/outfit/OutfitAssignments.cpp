#include "outfit/OutfitAssignments.h"
#include "player/PlayerIds.h"
#include "persistence/AssignmentIdentity.h"
#include "persistence/JsonFile.h"

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
    std::lock_guard lock(_mutex);
    _saveAllowed = false;
    try {
        const auto path = GetFilePath();
        if (!std::filesystem::exists(path)) {
            _assignments.clear();
            _retainedEntries = nlohmann::json::array();
            _saveAllowed = true;
            logger::info("OutfitAssignments: no assignments.json found, starting empty");
            return;
        }
        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        const int schemaVersion = json.value("version", 1);
        if (schemaVersion < 1 || schemaVersion > 3 || !json.contains("assignments") || !json["assignments"].is_array()) {
            throw std::runtime_error("unsupported assignment document");
        }
        std::unordered_map<RE::FormID, SituationalAssignment> parsed;
        auto retained = nlohmann::json::array();
        // A bad row must not disable situations for every other NPC at startup.
        // Keep valid rows usable, but never overwrite an incompletely read file.
        bool complete = true;
        std::size_t row = 0;
        for (const auto& entry : json["assignments"]) {
            ++row;
            try {
                std::string actorFormStr = entry.value("actorFormId", std::string{});
                std::string actorPlugin = entry.value("actorPlugin", std::string{});
                int outfitId = entry.value("outfitId", 0);
                int adventuringId = entry.value("adventuringId", 0);
                int townId = entry.value("townId", 0);
                int homeId = entry.value("homeId", 0);
                int sleepId = entry.value("sleepId", 0);
                int swimmingId = entry.value("swimmingId", 0);
                int warmId = entry.value("warmId", 0);
                bool adventuringRandom = entry.value("adventuringRandom", false);
                bool townRandom = entry.value("townRandom", false);
                bool homeRandom = entry.value("homeRandom", false);
                bool sleepRandom = entry.value("sleepRandom", false);
                bool swimmingRandom = entry.value("swimmingRandom", false);
                bool warmRandom = entry.value("warmRandom", false);
                const auto armorType = entry.contains("adventuringArmorType") && entry["adventuringArmorType"].is_string()
                    ? ParseOutfitArmorType(entry["adventuringArmorType"].get<std::string>()) : OutfitArmorType::Any;
                const auto localFormId = Tailor::Persistence::ReadLocalFormID(actorFormStr);
                const auto runtimeId = Tailor::Persistence::ActorRuntimeID(localFormId, actorPlugin);
                // The player's row belongs to each save's co-save.
                if (runtimeId == Tailor::Player::kPlayerRef) continue;
                SituationalAssignment sa;
                sa.outfitId = outfitId;
                sa.adventuringId = adventuringId;
                sa.townId = townId;
                sa.homeId = homeId;
                sa.sleepId = sleepId;
                sa.swimmingId = swimmingId;
                sa.warmId = warmId;
                sa.adventuringRandom = adventuringRandom;
                sa.townRandom = townRandom;
                sa.homeRandom = homeRandom;
                sa.sleepRandom = sleepRandom;
                sa.swimmingRandom = swimmingRandom;
                sa.warmRandom = warmRandom;
                sa.adventuringArmorType = armorType;
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
                sa.restoreDefaultPending = entry.value("restoreDefaultPending", false);
                if (!runtimeId) {
                    auto dormant = entry;
                    // The output document is v3 even when this row came from v1/v2.
                    dormant["originalDefaultOutfitKnown"] = sa.originalDefaultOutfitKnown;
                    dormant["originalSleepOutfitKnown"] = sa.originalSleepOutfitKnown;
                    dormant["originalDefaultChangeStateKnown"] = sa.originalDefaultChangeStateKnown;
                    dormant["originalSleepChangeStateKnown"] = sa.originalSleepChangeStateKnown;
                    retained.push_back(std::move(dormant));
                    logger::warn("OutfitAssignments: retaining actor {} because plugin '{}' is unavailable", actorFormStr, actorPlugin);
                    continue;
                }
                if (!parsed.emplace(runtimeId, std::move(sa)).second) throw std::runtime_error("duplicate actor assignment");
            } catch (const std::exception& e) {
                complete = false;
                logger::error("OutfitAssignments: row {} could not load; other rows remain active, file preserved and saves disabled: {}", row, e.what());
            }
        }
        _assignments = std::move(parsed);
        _retainedEntries = std::move(retained);
        _saveAllowed = complete;
        logger::info("OutfitAssignments: loaded {} assignment(s), retained {} dormant row(s) (schema v{})",
            _assignments.size(), _retainedEntries.size(), schemaVersion);
    } catch (const std::exception& e) {
        logger::error("OutfitAssignments: failed to load; file preserved and saves disabled: {}", e.what());
    }
}

void OutfitAssignments::Save() const
{
    std::lock_guard lock(_mutex);
    if (!_saveAllowed) {
        logger::error("OutfitAssignments: save skipped because the file was not loaded successfully");
        return;
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    nlohmann::json json;
    json["version"] = 3;
    json["assignments"] = _retainedEntries;

    for (auto& [runtimeId, sa] : _assignments) {
        // The player's row is saved with each game, in the co-save.
        if (runtimeId == Tailor::Player::kPlayerRef) continue;
        if (!sa.HasSettings()) continue;

        // Decompose runtime FormID to get the correct source plugin
        uint8_t modIndex = (runtimeId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex == 0xFF) {
            logger::warn("OutfitAssignments: runtime actor 0x{:X} has no persistent plugin identity", runtimeId);
            continue;
        }
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
            logger::error("OutfitAssignments: save cancelled; could not resolve plugin for runtime 0x{:X}", runtimeId);
            return;
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
        if (sa.swimmingId > 0) entry["swimmingId"] = sa.swimmingId;
        if (sa.warmId > 0) entry["warmId"] = sa.warmId;
        if (sa.adventuringRandom) entry["adventuringRandom"] = true;
        if (sa.townRandom) entry["townRandom"] = true;
        if (sa.homeRandom) entry["homeRandom"] = true;
        if (sa.sleepRandom) entry["sleepRandom"] = true;
        if (sa.swimmingRandom) entry["swimmingRandom"] = true;
        if (sa.warmRandom) entry["warmRandom"] = true;
        if (sa.adventuringArmorType != OutfitArmorType::Any) {
            entry["adventuringArmorType"] = OutfitArmorTypeName(sa.adventuringArmorType);
        }
        if (!sa.originalOutfitPlugin.empty()) entry["originalOutfitPlugin"] = sa.originalOutfitPlugin;
        if (!sa.originalOutfitLocalId.empty()) entry["originalOutfitLocalId"] = sa.originalOutfitLocalId;
        if (!sa.originalSleepOutfitPlugin.empty()) entry["originalSleepOutfitPlugin"] = sa.originalSleepOutfitPlugin;
        if (!sa.originalSleepOutfitLocalId.empty()) entry["originalSleepOutfitLocalId"] = sa.originalSleepOutfitLocalId;
        if (sa.restoreDefaultPending) entry["restoreDefaultPending"] = true;
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
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("OutfitAssignments: failed to save assignments.json: {}", error);
            return;
        }
        logger::info("OutfitAssignments: saved {} assignment row(s), including {} dormant", json["assignments"].size(), _retainedEntries.size());
    } catch (const std::exception& e) {
        logger::error("OutfitAssignments: failed to save: {}", e.what());
    }
}

bool OutfitAssignments::SaveAllowed() const
{
    std::lock_guard lock(_mutex);
    return _saveAllowed;
}

void OutfitAssignments::Assign(RE::FormID actorRuntimeId, int outfitId)
{
    std::lock_guard lock(_mutex);
    _assignments[actorRuntimeId].outfitId = outfitId;
    _assignments[actorRuntimeId].restoreDefaultPending = false;
    logger::info("OutfitAssignments: assigned outfit {} to actor 0x{:X}", outfitId, actorRuntimeId);
}

void OutfitAssignments::Unassign(RE::FormID actorRuntimeId)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it != _assignments.end() && it->second.adventuringArmorType != OutfitArmorType::Any) {
        const auto type = it->second.adventuringArmorType;
        it->second = {};
        it->second.adventuringArmorType = type;
    } else {
        _assignments.erase(actorRuntimeId);
    }
    logger::info("OutfitAssignments: unassigned actor 0x{:X}", actorRuntimeId);
}

bool OutfitAssignments::HasAssignment(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    const auto it = _assignments.find(actorRuntimeId);
    return it != _assignments.end() && it->second.HasOutfits();
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
    auto active = _assignments;
    // NPC batches (load re-apply, OBody presets, situation polls) never take the player.
    std::erase_if(active, [](const auto& entry) {
        return entry.first == Tailor::Player::kPlayerRef ||
            (!entry.second.HasOutfits() && !entry.second.restoreDefaultPending);
    });
    return active;
}

std::optional<SituationalAssignment> OutfitAssignments::ExportPlayer() const
{
    std::lock_guard lock(_mutex);
    const auto it = _assignments.find(Tailor::Player::kPlayerRef);
    if (it == _assignments.end() || !it->second.HasSettings()) return std::nullopt;
    return it->second;
}

void OutfitAssignments::ImportPlayer(const std::optional<SituationalAssignment>& assignment)
{
    std::lock_guard lock(_mutex);
    if (assignment && assignment->HasSettings()) {
        _assignments[Tailor::Player::kPlayerRef] = *assignment;
    } else {
        _assignments.erase(Tailor::Player::kPlayerRef);
    }
}

void OutfitAssignments::AssignSituation(RE::FormID actorRuntimeId, OutfitSituation situation, int outfitId)
{
    std::lock_guard lock(_mutex);
    auto& a = _assignments[actorRuntimeId];
    a.SetSlot(situation, outfitId);
    a.restoreDefaultPending = false;
    logger::info("OutfitAssignments: assigned situation {} outfit {} to actor 0x{:X}",
        static_cast<int>(situation), outfitId, actorRuntimeId);
}

void OutfitAssignments::ClearSituation(RE::FormID actorRuntimeId, OutfitSituation situation)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return;
    it->second.ClearSlot(situation);
    // Keep generic assignment and original-outfit tracking. Final unassignment
    // goes through OutfitManager::ResetOutfit so restoration completes first.
    logger::info("OutfitAssignments: cleared situation {} for actor 0x{:X}",
        static_cast<int>(situation), actorRuntimeId);
}

void OutfitAssignments::ClearAllSituations(RE::FormID actorRuntimeId)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return;
    it->second.ClearSituations();
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

void OutfitAssignments::SetAdventuringArmorType(RE::FormID actorRuntimeId, OutfitArmorType type)
{
    std::lock_guard lock(_mutex);
    auto& state = _assignments[actorRuntimeId];
    state.adventuringArmorType = type;
    if (!state.HasSettings()) _assignments.erase(actorRuntimeId);
}

OutfitArmorType OutfitAssignments::GetAdventuringArmorType(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    const auto it = _assignments.find(actorRuntimeId);
    return it == _assignments.end() ? OutfitArmorType::Any : it->second.adventuringArmorType;
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
            sa.sleepId == outfitId || sa.swimmingId == outfitId || sa.warmId == outfitId) {
            result.push_back(actorId);
        }
    }
    return result;
}

void OutfitAssignments::RemoveOutfitFromAllAssignments(int outfitId, const std::vector<RE::FormID>& restoreLater)
{
    {
        std::lock_guard lock(_mutex);
        for (auto& row : _retainedEntries) {
            bool removed = false;
            bool remaining = false;
            for (const auto* slot : {"outfitId", "adventuringId", "townId", "homeId", "sleepId", "swimmingId", "warmId"}) {
                if (row.value(slot, 0) == outfitId) { row[slot] = 0; removed = true; }
                remaining = remaining || row.value(slot, 0) > 0;
            }
            for (const auto* flag : {"adventuringRandom", "townRandom", "homeRandom", "sleepRandom", "swimmingRandom", "warmRandom"}) {
                remaining = remaining || row.value(flag, false);
            }
            if (removed && !remaining) row["restoreDefaultPending"] = true;
        }
        for (auto it = _assignments.begin(); it != _assignments.end(); ) {
            auto& sa = it->second;
            const bool hadOutfits = sa.HasOutfits();
            sa.RemoveOutfit(outfitId);
            if (hadOutfits && !sa.HasOutfits() &&
                std::find(restoreLater.begin(), restoreLater.end(), it->first) != restoreLater.end()) {
                sa.restoreDefaultPending = true;
            }

            if (!sa.HasSettings()) {
                it = _assignments.erase(it);
            } else {
                ++it;
            }
        }
    }
    Save();
    logger::info("OutfitAssignments: removed outfit {} from all assignments", outfitId);
}

bool OutfitAssignments::IsRestoreDefaultPending(RE::FormID actorRuntimeId) const
{
    std::lock_guard lock(_mutex);
    const auto it = _assignments.find(actorRuntimeId);
    return it != _assignments.end() && it->second.restoreDefaultPending;
}

void OutfitAssignments::ClearRestoreDefaultPending(RE::FormID actorRuntimeId)
{
    std::lock_guard lock(_mutex);
    const auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return;
    it->second.restoreDefaultPending = false;
    if (!it->second.HasSettings()) _assignments.erase(it);
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

bool OutfitAssignments::SetOriginalOutfitState(
    RE::FormID actorRuntimeId,
    RE::BGSOutfit* defaultOutfit,
    RE::BGSOutfit* sleepOutfit,
    bool defaultOutfitHadChange,
    bool sleepOutfitHadChange)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorRuntimeId);
    if (it == _assignments.end()) return false;

    // Build the replacement aside so an outfit that cannot be encoded changes nothing.
    auto updated = it->second;
    updated.originalOutfitPlugin.clear();
    updated.originalOutfitLocalId.clear();
    updated.originalSleepOutfitPlugin.clear();
    updated.originalSleepOutfitLocalId.clear();
    if (defaultOutfit && !EncodeOutfit(defaultOutfit, updated.originalOutfitPlugin, updated.originalOutfitLocalId)) return false;
    if (sleepOutfit && !EncodeOutfit(sleepOutfit, updated.originalSleepOutfitPlugin, updated.originalSleepOutfitLocalId)) return false;
    updated.originalDefaultOutfitKnown = updated.originalSleepOutfitKnown = true;
    updated.originalDefaultChangeStateKnown = updated.originalSleepChangeStateKnown = true;
    updated.originalDefaultOutfitHadChange = defaultOutfitHadChange;
    updated.originalSleepOutfitHadChange = sleepOutfitHadChange;

    const auto& stored = it->second;
    const bool changed =
        stored.originalOutfitPlugin != updated.originalOutfitPlugin ||
        stored.originalOutfitLocalId != updated.originalOutfitLocalId ||
        stored.originalSleepOutfitPlugin != updated.originalSleepOutfitPlugin ||
        stored.originalSleepOutfitLocalId != updated.originalSleepOutfitLocalId ||
        !stored.originalDefaultOutfitKnown || !stored.originalSleepOutfitKnown ||
        !stored.originalDefaultChangeStateKnown || !stored.originalSleepChangeStateKnown ||
        stored.originalDefaultOutfitHadChange != defaultOutfitHadChange ||
        stored.originalSleepOutfitHadChange != sleepOutfitHadChange;
    if (changed) {
        logger::info(
            "OutfitAssignments: original outfit for actor 0x{:X} is {}:{} from the NPC's own plugin record (was {}:{})",
            actorRuntimeId,
            updated.originalOutfitPlugin.empty() ? "none" : updated.originalOutfitPlugin, updated.originalOutfitLocalId,
            stored.originalOutfitPlugin.empty() ? "none" : stored.originalOutfitPlugin, stored.originalOutfitLocalId);
        it->second = std::move(updated);
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
