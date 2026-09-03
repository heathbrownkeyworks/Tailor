#include "wig/WigAssignments.h"

WigAssignments& WigAssignments::GetSingleton()
{
    static WigAssignments singleton;
    return singleton;
}

std::filesystem::path WigAssignments::GetAssignmentsPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Wiggy");
    std::filesystem::create_directories(path);
    return path / "assignments.json";
}

void WigAssignments::Load()
{
    int prunedCount = 0;

    {
        auto path = GetAssignmentsPath();
        logger::info("Assignments path: {}", path.string());

        if (!std::filesystem::exists(path)) {
            logger::info("No assignments file found, starting with empty assignments");
            try {
                nlohmann::json json;
                json["version"] = 1;
                json["assignments"] = nlohmann::json::array();
                std::ofstream file(path);
                if (!file.is_open()) {
                    logger::error("Failed to create assignments file at {}", path.string());
                    return;
                }
                file << json.dump(2);
                file.flush();
                logger::info("Created empty assignments file at {}", path.string());
            }
            catch (const std::exception& e) {
                logger::error("Exception creating assignments file: {}", e.what());
            }
            return;
        }

        // Parse into temp map first — only swap on success to prevent data loss
        std::unordered_map<RE::FormID, ActorWigState> parsed;

        try {
            std::ifstream file(path);
            auto json = nlohmann::json::parse(file);

            if (!json.contains("assignments")) {
                logger::warn("Assignments file missing 'assignments' key");
                return;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                logger::error("TESDataHandler not available during assignments load");
                return;
            }

            for (auto& entry : json["assignments"]) {
                auto actorFormId = std::stoul(entry["actorFormId"].get<std::string>(), nullptr, 16);
                auto actorPlugin = entry["actorPlugin"].get<std::string>();

                // Resolve actor
                auto* actorForm = dataHandler->LookupForm(actorFormId, actorPlugin);
                if (!actorForm) {
                    logger::warn("Pruning assignment: actor 0x{:06X} from '{}' not found",
                        actorFormId, actorPlugin);
                    prunedCount++;
                    continue;
                }

                ActorWigState wigState;

                // Wig fields are optional (color-only assignments have no wig)
                auto wigFormIdStr = entry.value("wigFormId", std::string{});
                auto wigPlugin = entry.value("wigPlugin", std::string{});

                if (!wigFormIdStr.empty() && !wigPlugin.empty()) {
                    WigEntry wig;
                    wig.formId = std::stoul(wigFormIdStr, nullptr, 16);
                    wig.plugin = wigPlugin;
                    wig.name = entry.value("wigName", "");

                    auto* armor = wig.Resolve();
                    if (!armor) {
                        logger::warn("Pruning assignment: wig '{}' from '{}' not found",
                            wig.name, wigPlugin);
                    } else {
                        wig.name = SanitizeUtf8(armor->GetName());
                        wigState.currentWig = std::move(wig);
                        wigState.itemAdded = entry.value("itemAdded", false);
                    }
                }

                // Hair color fields (optional)
                wigState.hairColorR = static_cast<int16_t>(entry.value("hairColorR", -1));
                wigState.hairColorG = static_cast<int16_t>(entry.value("hairColorG", -1));
                wigState.hairColorB = static_cast<int16_t>(entry.value("hairColorB", -1));

                // Only store if there's something meaningful (wig or color)
                if (wigState.currentWig.formId != 0 || wigState.HasHairColor()) {
                    parsed[actorForm->GetFormID()] = std::move(wigState);
                }
            }

            logger::info("Loaded {} wig assignments from {}", parsed.size(), path.string());
        }
        catch (const std::exception& e) {
            logger::error("Failed to load wig assignments: {}", e.what());
            return;  // keep existing _assignments intact
        }

        // Success — swap into live map
        std::lock_guard lock(_mutex);
        _assignments = std::move(parsed);
    }

    // Clean stale entries (lock released, Save() acquires its own)
    if (prunedCount > 0) {
        logger::info("Pruned {} stale assignment(s) — saving cleaned file", prunedCount);
        Save();
    }
}

void WigAssignments::Save() const
{
    std::lock_guard lock(_mutex);

    nlohmann::json json;
    json["version"] = 1;
    json["assignments"] = nlohmann::json::array();

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    for (auto& [actorRuntimeId, state] : _assignments) {
        // Decompose runtime FormID to local FormID + plugin (same method as OutfitAssignments)
        auto* actorForm = RE::TESForm::LookupByID(actorRuntimeId);
        if (!actorForm) {
            logger::warn("Save: actor 0x{:08X} no longer exists, skipping", actorRuntimeId);
            continue;
        }

        uint8_t modIndex = (actorRuntimeId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex != 0xFE) {
            sourceFile = dataHandler->LookupLoadedModByIndex(modIndex);
            localFormId = actorRuntimeId & 0x00FFFFFF;
        } else {
            uint16_t eslIndex = static_cast<uint16_t>((actorRuntimeId & 0x00FFF000) >> 12);
            sourceFile = dataHandler->LookupLoadedLightModByIndex(eslIndex);
            localFormId = actorRuntimeId & 0x00000FFF;
        }

        if (!sourceFile) {
            logger::warn("Save: could not resolve plugin for actor 0x{:08X}, skipping", actorRuntimeId);
            continue;
        }

        nlohmann::json entry;
        entry["actorFormId"] = std::format("0x{:06X}", localFormId);
        entry["actorPlugin"] = std::string(sourceFile->GetFilename());

        // Wig fields (only if a wig is assigned)
        if (state.currentWig.formId != 0 && !state.currentWig.plugin.empty()) {
            entry["wigFormId"] = std::format("0x{:06X}", state.currentWig.formId);
            entry["wigPlugin"] = state.currentWig.plugin;
            entry["wigName"] = state.currentWig.name;
            entry["itemAdded"] = state.itemAdded;
        }

        // Hair color fields (only if a color override is active)
        if (state.HasHairColor()) {
            entry["hairColorR"] = static_cast<int>(state.hairColorR);
            entry["hairColorG"] = static_cast<int>(state.hairColorG);
            entry["hairColorB"] = static_cast<int>(state.hairColorB);
        }

        json["assignments"].push_back(entry);
    }

    try {
        auto path = GetAssignmentsPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("Failed to open assignments file for writing: {}", path.string());
            return;
        }
        file << json.dump(2);
        file.flush();
        logger::info("Saved {} wig assignments to {}", json["assignments"].size(), path.string());
    }
    catch (const std::exception& e) {
        logger::error("Failed to save wig assignments: {}", e.what());
    }
}

void WigAssignments::SetAssignment(RE::FormID actorFormId, const WigEntry& wig, bool itemAdded)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        // Preserve existing hair color when changing wigs
        it->second.currentWig = wig;
        it->second.itemAdded = itemAdded;
    } else {
        ActorWigState state;
        state.currentWig = wig;
        state.itemAdded = itemAdded;
        _assignments[actorFormId] = std::move(state);
    }
}

void WigAssignments::MarkItemAdded(RE::FormID actorFormId)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        it->second.itemAdded = true;
    }
}

void WigAssignments::ClearAssignment(RE::FormID actorFormId)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        it->second.currentWig = WigEntry{};
        it->second.itemAdded = false;
        // If no hair color either, remove the entry entirely
        if (!it->second.HasHairColor()) {
            _assignments.erase(it);
        }
    }
    logger::info("Cleared wig assignment for actor {:08X}", actorFormId);
}

std::optional<WigEntry> WigAssignments::GetAssignment(RE::FormID actorFormId) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        return it->second.currentWig;
    }
    return std::nullopt;
}

bool WigAssignments::HasAssignment(RE::FormID actorFormId) const
{
    std::lock_guard lock(_mutex);
    return _assignments.contains(actorFormId);
}

std::unordered_map<RE::FormID, ActorWigState> WigAssignments::GetAll() const
{
    std::lock_guard lock(_mutex);
    return _assignments;
}

void WigAssignments::Clear()
{
    std::lock_guard lock(_mutex);
    _assignments.clear();
}

void WigAssignments::SetHairColor(RE::FormID actorFormId, int16_t r, int16_t g, int16_t b)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        it->second.hairColorR = r;
        it->second.hairColorG = g;
        it->second.hairColorB = b;
    } else {
        ActorWigState state;
        state.hairColorR = r;
        state.hairColorG = g;
        state.hairColorB = b;
        _assignments[actorFormId] = state;
    }
}

void WigAssignments::ClearHairColor(RE::FormID actorFormId)
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        it->second.hairColorR = -1;
        it->second.hairColorG = -1;
        it->second.hairColorB = -1;
        // If no wig either, remove the entry entirely
        if (it->second.currentWig.formId == 0 && it->second.currentWig.plugin.empty()) {
            _assignments.erase(it);
        }
    }
}

std::optional<ActorWigState> WigAssignments::GetState(RE::FormID actorFormId) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================
//  Situational Wig Assignments
// ============================================================

std::filesystem::path WigAssignments::GetSituationsPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Wiggy");
    std::filesystem::create_directories(path);
    return path / "wig_situations.json";
}

void WigAssignments::LoadSituations()
{
    int prunedCount = 0;

    {
        auto path = GetSituationsPath();
        if (!std::filesystem::exists(path)) {
            logger::info("No wig situations file found at {}", path.string());
            return;
        }

        // Parse into temp map first — only swap on success to prevent data loss
        std::unordered_map<RE::FormID, WigSituationalAssignment> parsed;

        try {
            std::ifstream file(path);
            auto json = nlohmann::json::parse(file);

            if (!json.contains("situations")) {
                logger::warn("Wig situations file missing 'situations' key");
                return;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                logger::error("TESDataHandler not available during wig situations load");
                return;
            }

            auto resolveWigEntry = [&](const nlohmann::json& obj) -> WigEntry {
                WigEntry wig;
                if (obj.is_null() || !obj.contains("plugin") || !obj.contains("localId")) {
                    return wig;
                }
                wig.formId = std::stoul(obj["localId"].get<std::string>(), nullptr, 16);
                wig.plugin = obj["plugin"].get<std::string>();
                wig.name = obj.value("name", "");

                auto* armor = wig.Resolve();
                if (!armor) {
                    logger::warn("Wig situations: wig '{}' from '{}' not found, pruning",
                        wig.name, wig.plugin);
                    return {};
                }
                wig.name = SanitizeUtf8(armor->GetName());
                return wig;
            };

            for (auto& entry : json["situations"]) {
                auto actorLocalId = std::stoul(entry["actorLocalId"].get<std::string>(), nullptr, 16);
                auto actorPlugin = entry["actorPlugin"].get<std::string>();

                auto* actorForm = dataHandler->LookupForm(actorLocalId, actorPlugin);
                if (!actorForm) {
                    logger::warn("Wig situations: pruning actor 0x{:06X} from '{}'",
                        actorLocalId, actorPlugin);
                    prunedCount++;
                    continue;
                }

                WigSituationalAssignment wsa;
                if (entry.contains("adventuring")) wsa.adventuring = resolveWigEntry(entry["adventuring"]);
                if (entry.contains("town"))        wsa.town = resolveWigEntry(entry["town"]);
                if (entry.contains("home"))        wsa.home = resolveWigEntry(entry["home"]);
                if (entry.contains("sleep"))       wsa.sleep = resolveWigEntry(entry["sleep"]);

                if (wsa.HasAnySituation()) {
                    parsed[actorForm->GetFormID()] = std::move(wsa);
                }
            }

            logger::info("Loaded {} wig situational assignments from {}", parsed.size(), path.string());
        }
        catch (const std::exception& e) {
            logger::error("Failed to load wig situations: {}", e.what());
            return;  // keep existing _situations intact
        }

        // Success — swap into live map
        std::lock_guard lock(_mutex);
        _situations = std::move(parsed);
    }

    if (prunedCount > 0) {
        logger::info("Pruned {} stale wig situation(s) — saving cleaned file", prunedCount);
        SaveSituations();
    }
}

void WigAssignments::SaveSituations() const
{
    std::lock_guard lock(_mutex);

    nlohmann::json json;
    json["version"] = 1;
    json["situations"] = nlohmann::json::array();

    auto serializeWig = [](const WigEntry& wig) -> nlohmann::json {
        if (wig.formId == 0 || wig.plugin.empty()) return nullptr;
        return {
            {"plugin", wig.plugin},
            {"localId", std::format("0x{:06X}", wig.formId)},
            {"name", wig.name}
        };
    };

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    for (auto& [actorRuntimeId, wsa] : _situations) {
        auto* actorForm = RE::TESForm::LookupByID(actorRuntimeId);
        if (!actorForm) continue;

        uint8_t modIndex = (actorRuntimeId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex != 0xFE) {
            sourceFile = dataHandler->LookupLoadedModByIndex(modIndex);
            localFormId = actorRuntimeId & 0x00FFFFFF;
        } else {
            uint16_t eslIndex = static_cast<uint16_t>((actorRuntimeId & 0x00FFF000) >> 12);
            sourceFile = dataHandler->LookupLoadedLightModByIndex(eslIndex);
            localFormId = actorRuntimeId & 0x00000FFF;
        }

        if (!sourceFile) continue;

        nlohmann::json entry;
        entry["actorLocalId"] = std::format("0x{:06X}", localFormId);
        entry["actorPlugin"] = std::string(sourceFile->GetFilename());

        auto adv = serializeWig(wsa.adventuring);
        auto twn = serializeWig(wsa.town);
        auto hom = serializeWig(wsa.home);
        auto slp = serializeWig(wsa.sleep);

        if (!adv.is_null()) entry["adventuring"] = adv;
        if (!twn.is_null()) entry["town"] = twn;
        if (!hom.is_null()) entry["home"] = hom;
        if (!slp.is_null()) entry["sleep"] = slp;

        json["situations"].push_back(entry);
    }

    try {
        auto path = GetSituationsPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("Failed to open wig situations file for writing: {}", path.string());
            return;
        }
        file << json.dump(2);
        file.flush();
        logger::info("Saved {} wig situational assignments to {}", json["situations"].size(), path.string());
    }
    catch (const std::exception& e) {
        logger::error("Failed to save wig situations: {}", e.what());
    }
}

void WigAssignments::AssignSituation(RE::FormID actorFormId, OutfitSituation situation, const WigEntry& wig)
{
    std::lock_guard lock(_mutex);
    _situations[actorFormId].SetSlot(situation, wig);

    // Clear legacy single-wig assignment (migrating to situational mode)
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        it->second.currentWig = WigEntry{};
        if (!it->second.HasHairColor()) {
            _assignments.erase(it);
        }
    }
}

void WigAssignments::ClearSituation(RE::FormID actorFormId, OutfitSituation situation)
{
    std::lock_guard lock(_mutex);
    auto it = _situations.find(actorFormId);
    if (it != _situations.end()) {
        it->second.ClearSlot(situation);
        if (!it->second.HasAnySituation()) {
            _situations.erase(it);
        }
    }
}

void WigAssignments::ClearAllSituations(RE::FormID actorFormId)
{
    std::lock_guard lock(_mutex);
    _situations.erase(actorFormId);
}

bool WigAssignments::HasAnySituation(RE::FormID actorFormId) const
{
    std::lock_guard lock(_mutex);
    auto it = _situations.find(actorFormId);
    return it != _situations.end() && it->second.HasAnySituation();
}

WigEntry WigAssignments::GetSituationWig(RE::FormID actorFormId, OutfitSituation situation) const
{
    std::lock_guard lock(_mutex);
    auto it = _situations.find(actorFormId);
    if (it != _situations.end()) {
        return it->second.GetSlot(situation);
    }
    return {};
}

const WigSituationalAssignment* WigAssignments::GetSituationalAssignment(RE::FormID actorFormId) const
{
    std::lock_guard lock(_mutex);
    auto it = _situations.find(actorFormId);
    if (it != _situations.end()) {
        return &it->second;
    }
    return nullptr;
}

std::unordered_map<RE::FormID, WigSituationalAssignment> WigAssignments::GetAllSituational() const
{
    std::lock_guard lock(_mutex);
    return _situations;
}
