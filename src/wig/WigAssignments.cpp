#include "wig/WigAssignments.h"
#include "player/PlayerIds.h"
#include "persistence/AssignmentIdentity.h"
#include "persistence/JsonFile.h"

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
    std::lock_guard lock(_mutex);
    _saveAssignmentsAllowed = false;
    try {
        const auto path = GetAssignmentsPath();
        if (!std::filesystem::exists(path)) {
            _assignments.clear();
            _retainedAssignments = nlohmann::json::array();
            _inferAssignedWig.clear();
            _saveAssignmentsAllowed = true;
            return;
        }
        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        if (json.value("version", 1) != 1 || !json.contains("assignments") || !json["assignments"].is_array()) {
            throw std::runtime_error("unsupported wig assignment document");
        }
        std::unordered_map<RE::FormID, ActorWigState> parsed;
        auto retained = nlohmann::json::array();
        std::unordered_set<RE::FormID> inferAssigned;
        bool complete = true;
        std::size_t row = 0;
        for (const auto& entry : json["assignments"]) {
            ++row;
            try {
                const auto actorLocalId = Tailor::Persistence::ReadLocalFormID(entry.at("actorFormId").get<std::string>());
                const auto runtimeId = Tailor::Persistence::ActorRuntimeID(actorLocalId, entry.at("actorPlugin").get<std::string>());
                if (runtimeId == Tailor::Player::kPlayerRef) continue;

                ActorWigState state;
                const auto wigId = entry.value("wigFormId", std::string{});
                const auto wigPlugin = entry.value("wigPlugin", std::string{});
                if (wigId.empty() != wigPlugin.empty()) throw std::runtime_error("incomplete wig identity");
                if (!wigId.empty()) {
                    state.currentWig = {Tailor::Persistence::ReadLocalFormID(wigId), wigPlugin, entry.value("wigName", "")};
                    if (const auto* armor = state.currentWig.Resolve()) state.currentWig.name = SanitizeUtf8(armor->GetName());
                    state.itemAdded = entry.value("itemAdded", false);
                }
                // The wig set in the Hair Dresser. Rows saved before it was kept have no field: once the
                // situations load, a worn wig that isn't a situation wig is taken as it.
                const bool assignedKnown = entry.contains("assignedWigFormId");
                const auto assignedId = entry.value("assignedWigFormId", std::string{});
                const auto assignedPlugin = entry.value("assignedWigPlugin", std::string{});
                if (assignedId.empty() != assignedPlugin.empty()) throw std::runtime_error("incomplete assigned wig identity");
                if (!assignedId.empty()) {
                    state.assignedWig = {Tailor::Persistence::ReadLocalFormID(assignedId), assignedPlugin, entry.value("assignedWigName", "")};
                    if (const auto* armor = state.assignedWig.Resolve()) state.assignedWig.name = SanitizeUtf8(armor->GetName());
                }
                state.hairColorR = static_cast<int16_t>(entry.value("hairColorR", -1));
                state.hairColorG = static_cast<int16_t>(entry.value("hairColorG", -1));
                state.hairColorB = static_cast<int16_t>(entry.value("hairColorB", -1));
                if (!runtimeId) {
                    retained.push_back(entry);
                    continue;
                }
                if (!state.IsEmpty()) {
                    if (!parsed.emplace(runtimeId, std::move(state)).second) throw std::runtime_error("duplicate wig assignment");
                    if (!assignedKnown) inferAssigned.insert(runtimeId);
                }
            } catch (const std::exception& e) {
                complete = false;
                logger::error("Wig assignments: row {} could not load; other rows remain active, file preserved and saves disabled: {}", row, e.what());
            }
        }
        _assignments = std::move(parsed);
        _retainedAssignments = std::move(retained);
        _inferAssignedWig = std::move(inferAssigned);
        _saveAssignmentsAllowed = complete;
        logger::info("Loaded {} wig assignments, retained {} dormant row(s)", _assignments.size(), _retainedAssignments.size());
    } catch (const std::exception& e) {
        logger::error("Failed to load wig assignments; file preserved and saves disabled: {}", e.what());
    }
}

void WigAssignments::Save() const
{
    std::lock_guard lock(_mutex);
    if (!_saveAssignmentsAllowed) {
        logger::error("Wig assignments: save skipped because the file was not loaded successfully");
        return;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["assignments"] = _retainedAssignments;

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    for (auto& [actorRuntimeId, state] : _assignments) {
        // The player's data belongs to each save's co-save, never this shared file.
        if (actorRuntimeId == Tailor::Player::kPlayerRef) continue;
        // Decompose runtime FormID to local FormID + plugin (same method as OutfitAssignments)
        uint8_t modIndex = (actorRuntimeId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex == 0xFF) {
            logger::warn("Wig assignments: runtime actor 0x{:X} has no persistent plugin identity", actorRuntimeId);
            continue;
        }
        if (modIndex != 0xFE) {
            sourceFile = dataHandler->LookupLoadedModByIndex(modIndex);
            localFormId = actorRuntimeId & 0x00FFFFFF;
        } else {
            uint16_t eslIndex = static_cast<uint16_t>((actorRuntimeId & 0x00FFF000) >> 12);
            sourceFile = dataHandler->LookupLoadedLightModByIndex(eslIndex);
            localFormId = actorRuntimeId & 0x00000FFF;
        }

        if (!sourceFile) {
            logger::error("Wig assignments: save cancelled; could not resolve plugin for actor 0x{:08X}", actorRuntimeId);
            return;
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

        // The Hair Dresser's wig, written for every row and empty when none, so a load tells a row
        // saved without one from a row saved before it was kept. A row still waiting to be inferred
        // is saved as it was, without the fields, so a later session infers it.
        if (!_inferAssignedWig.contains(actorRuntimeId)) {
            const bool assigned = state.assignedWig.formId != 0 && !state.assignedWig.plugin.empty();
            entry["assignedWigFormId"] = assigned ? std::format("0x{:06X}", state.assignedWig.formId) : std::string{};
            entry["assignedWigPlugin"] = assigned ? state.assignedWig.plugin : std::string{};
            entry["assignedWigName"] = assigned ? state.assignedWig.name : std::string{};
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
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("Failed to save wig assignments to {}: {}", path.string(), error);
            return;
        }
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
        // If no hair color or assigned wig either, remove the entry entirely
        if (it->second.IsEmpty()) {
            _assignments.erase(it);
        }
    }
    logger::info("Cleared wig assignment for actor {:08X}", actorFormId);
}

void WigAssignments::SetAssignedWig(RE::FormID actorFormId, const WigEntry& wig)
{
    std::lock_guard lock(_mutex);
    // Setting or clearing the assigned wig ends the wait for its inference, even with no row to clear.
    _inferAssignedWig.erase(actorFormId);
    const auto it = _assignments.find(actorFormId);
    if (wig.formId == 0) {
        if (it == _assignments.end()) return;
        it->second.assignedWig = WigEntry{};
        // Nothing left to keep: no worn wig and no hair color either.
        if (it->second.IsEmpty()) _assignments.erase(it);
        return;
    }
    if (it != _assignments.end()) {
        it->second.assignedWig = wig;
    } else {
        ActorWigState state;
        state.assignedWig = wig;
        _assignments[actorFormId] = std::move(state);
    }
}

void WigAssignments::InferAssignedWigs()
{
    std::lock_guard lock(_mutex);
    // An incomplete situations load would pass a situation wig off as the assigned one, and the next
    // save would make that final: the rows wait, saved without the field, for a session that loads whole.
    if (!_saveSituationsAllowed) {
        logger::info("Wig assignments: assigned wig inference waits for a complete wig situations load");
        return;
    }
    for (const auto actorId : _inferAssignedWig) {
        const auto it = _assignments.find(actorId);
        if (it == _assignments.end() || it->second.currentWig.formId == 0) continue;
        const auto situations = _situations.find(actorId);
        if (situations == _situations.end() || !situations->second.Holds(it->second.currentWig)) {
            it->second.assignedWig = it->second.currentWig;
        }
    }
    _inferAssignedWig.clear();
}

std::optional<WigEntry> WigAssignments::GetAssignment(RE::FormID actorFormId) const
{
    std::lock_guard lock(_mutex);
    auto it = _assignments.find(actorFormId);
    // An entry can hold a hair color alone, which is no wig.
    if (it != _assignments.end() && it->second.currentWig.formId != 0) {
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
    _retainedAssignments = nlohmann::json::array();
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
        // If no wig or assigned wig either, remove the entry entirely
        if (it->second.IsEmpty()) {
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
    std::lock_guard lock(_mutex);
    _saveSituationsAllowed = false;
    try {
        const auto path = GetSituationsPath();
        if (!std::filesystem::exists(path)) {
            _situations.clear();
            _retainedSituations = nlohmann::json::array();
            _saveSituationsAllowed = true;
            return;
        }
        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        if (json.value("version", 1) != 1 || !json.contains("situations") || !json["situations"].is_array()) {
            throw std::runtime_error("unsupported wig situation document");
        }
        std::unordered_map<RE::FormID, WigSituationalAssignment> parsed;
        auto retained = nlohmann::json::array();
        const auto readWig = [](const nlohmann::json& obj) -> WigEntry {
            if (obj.is_null()) return {};
            WigEntry wig{Tailor::Persistence::ReadLocalFormID(obj.at("localId").get<std::string>()),
                obj.at("plugin").get<std::string>(), obj.value("name", "")};
            if (wig.plugin.empty()) throw std::runtime_error("missing wig plugin");
            if (const auto* armor = wig.Resolve()) wig.name = SanitizeUtf8(armor->GetName());
            return wig;  // Missing armor is dormant, not an instruction to clear this slot.
        };
        bool complete = true;
        std::size_t row = 0;
        for (const auto& entry : json["situations"]) {
            ++row;
            try {
                const auto localId = Tailor::Persistence::ReadLocalFormID(entry.at("actorLocalId").get<std::string>());
                const auto runtimeId = Tailor::Persistence::ActorRuntimeID(localId, entry.at("actorPlugin").get<std::string>());
                if (runtimeId == Tailor::Player::kPlayerRef) continue;
                WigSituationalAssignment state;
                if (entry.contains("adventuring")) state.adventuring = readWig(entry["adventuring"]);
                if (entry.contains("town")) state.town = readWig(entry["town"]);
                if (entry.contains("home")) state.home = readWig(entry["home"]);
                if (entry.contains("sleep")) state.sleep = readWig(entry["sleep"]);
                if (!runtimeId) { retained.push_back(entry); continue; }
                if (state.HasAnySituation()) {
                    if (!parsed.emplace(runtimeId, std::move(state)).second) throw std::runtime_error("duplicate wig situation assignment");
                }
            } catch (const std::exception& e) {
                complete = false;
                logger::error("Wig situations: row {} could not load; other rows remain active, file preserved and saves disabled: {}", row, e.what());
            }
        }
        _situations = std::move(parsed);
        _retainedSituations = std::move(retained);
        _saveSituationsAllowed = complete;
        logger::info("Loaded {} wig situations, retained {} dormant row(s)", _situations.size(), _retainedSituations.size());
    } catch (const std::exception& e) {
        logger::error("Failed to load wig situations; file preserved and saves disabled: {}", e.what());
    }
}

void WigAssignments::SaveSituations() const
{
    std::lock_guard lock(_mutex);
    if (!_saveSituationsAllowed) {
        logger::error("Wig situations: save skipped because the file was not loaded successfully");
        return;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["situations"] = _retainedSituations;

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
        // The player's data belongs to each save's co-save, never this shared file.
        if (actorRuntimeId == Tailor::Player::kPlayerRef) continue;

        uint8_t modIndex = (actorRuntimeId >> 24) & 0xFF;
        const RE::TESFile* sourceFile = nullptr;
        RE::FormID localFormId = 0;

        if (modIndex == 0xFF) {
            logger::warn("Wig assignments: runtime actor 0x{:X} has no persistent plugin identity", actorRuntimeId);
            continue;
        }
        if (modIndex != 0xFE) {
            sourceFile = dataHandler->LookupLoadedModByIndex(modIndex);
            localFormId = actorRuntimeId & 0x00FFFFFF;
        } else {
            uint16_t eslIndex = static_cast<uint16_t>((actorRuntimeId & 0x00FFF000) >> 12);
            sourceFile = dataHandler->LookupLoadedLightModByIndex(eslIndex);
            localFormId = actorRuntimeId & 0x00000FFF;
        }

        if (!sourceFile) {
            logger::error("Wig situations: save cancelled; could not resolve plugin for actor 0x{:08X}", actorRuntimeId);
            return;
        }

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
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("Failed to save wig situations to {}: {}", path.string(), error);
            return;
        }
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

    // Clear legacy single-wig assignment (migrating to situational mode). The assigned wig stays:
    // situation wigs change only the worn wig.
    auto it = _assignments.find(actorFormId);
    if (it != _assignments.end()) {
        it->second.currentWig = WigEntry{};
        if (it->second.IsEmpty()) {
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

PlayerWigRow WigAssignments::ExportPlayer() const
{
    std::lock_guard lock(_mutex);
    PlayerWigRow row;
    if (const auto it = _assignments.find(Tailor::Player::kPlayerRef); it != _assignments.end()) row.state = it->second;
    if (const auto it = _situations.find(Tailor::Player::kPlayerRef); it != _situations.end() && it->second.HasAnySituation()) {
        row.situations = it->second;
    }
    return row;
}

void WigAssignments::ImportPlayer(const PlayerWigRow& row)
{
    std::lock_guard lock(_mutex);
    if (row.state && !row.state->IsEmpty()) {
        _assignments[Tailor::Player::kPlayerRef] = *row.state;
    } else {
        _assignments.erase(Tailor::Player::kPlayerRef);
    }
    if (row.situations && row.situations->HasAnySituation()) {
        _situations[Tailor::Player::kPlayerRef] = *row.situations;
    } else {
        _situations.erase(Tailor::Player::kPlayerRef);
    }
}
