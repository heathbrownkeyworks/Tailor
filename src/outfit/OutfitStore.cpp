#include "outfit/OutfitStore.h"

#include "outfit/OutfitNamePolicy.h"
#include "persistence/JsonFile.h"
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace
{
    // The caller holds the store's lock.
    bool Taken(const std::vector<CustomOutfit>& outfits, std::string_view name, int exceptId)
    {
        return std::any_of(outfits.begin(), outfits.end(), [&](const CustomOutfit& outfit) {
            return outfit.id != exceptId && Tailor::Outfits::SameName(outfit.name, name);
        });
    }

    // Tailor never hands out INT_MAX, as Import doesn't; a counter that reaches it means no id is left.
    constexpr int kNoIdLeft = (std::numeric_limits<int>::max)();
}

OutfitStore& OutfitStore::GetSingleton()
{
    static OutfitStore singleton;
    return singleton;
}

std::filesystem::path OutfitStore::GetStorePath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Tailor");
    std::filesystem::create_directories(path);
    return path / "outfits.json";
}

void OutfitStore::Load()
{
    bool needsSave = false;

    {
        std::lock_guard lock(_mutex);
        // Saves stay off until this load has read the whole file.
        _saveAllowed = false;
        try {
            const auto path = GetStorePath();
            logger::info("OutfitStore: loading from {}", path.string());

            std::error_code error;
            if (!std::filesystem::exists(path, error)) {
                // A file Tailor can't check is not a missing file: saves stay off.
                if (error) throw std::filesystem::filesystem_error("could not check outfits.json", path, error);
                logger::info("OutfitStore: no outfits.json found, starting empty");
                _outfits.clear();
                _nextId = 1;
                _saveAllowed = true;
                return;
            }

            std::ifstream file(path);
            const auto json = nlohmann::json::parse(file);
            if (!json.is_object() || (json.contains("version") && json["version"] != 1) ||
                !json.contains("outfits") || !json["outfits"].is_array()) {
                throw std::runtime_error("unsupported outfits document");
            }

            // Each row on its own, so a bad row never drops the outfits around it. A bad row's id stays reserved
            // when the row gives it as a whole number, so no new outfit takes it.
            std::vector<CustomOutfit> parsed;
            bool complete = true;
            int maxSeen = 0;
            std::size_t row = 0;
            for (const auto& oj : json["outfits"]) {
                ++row;
                try {
                    const auto& idJson = oj.at("id");
                    if (!idJson.is_number_integer()) throw std::runtime_error("the id is not a whole number");
                    const auto id = idJson.get<std::int64_t>();
                    if (id <= 0 || id > kNoIdLeft) throw std::runtime_error("the id is not a number from 1 up");

                    CustomOutfit outfit;
                    outfit.id = static_cast<int>(id);
                    outfit.name = oj.value("name", std::string{});
                    // Outfits saved before sex existed have none, and stay Unisex.
                    if (oj.contains("sex")) {
                        if (const auto sex = ReadOutfitSex(oj["sex"])) outfit.sex = *sex;
                        else logger::warn("OutfitStore: outfit {} has an unreadable sex; it loads as Unisex", outfit.id);
                    }
                    if (oj.contains("items")) {
                        if (!oj["items"].is_array()) throw std::runtime_error("items is not a list");
                        for (const auto& ij : oj["items"]) {
                            ArmorItem item;
                            item.formId = ij.value("formId", static_cast<RE::FormID>(0));
                            item.plugin = ij.value("plugin", std::string{});
                            item.name = ij.value("name", std::string{});
                            outfit.items.push_back(std::move(item));
                        }
                    }
                    maxSeen = (std::max)(maxSeen, outfit.id);
                    parsed.push_back(std::move(outfit));
                } catch (const std::exception& e) {
                    complete = false;
                    if (oj.is_object() && oj.contains("id") && oj["id"].is_number_integer() && oj["id"].get<std::int64_t>() > 0) {
                        maxSeen = (std::max)(maxSeen, static_cast<int>((std::min)(oj["id"].get<std::int64_t>(), std::int64_t{kNoIdLeft})));
                    }
                    logger::error("OutfitStore: outfit row {} could not load; the other outfits stay, outfits.json is kept as it is and saves are off: {}", row, e.what());
                }
            }

            // A counter of the wrong type counts as missing.
            const auto stored = json.contains("nextId") && json["nextId"].is_number_integer()
                ? json["nextId"].get<std::int64_t>() : std::int64_t{1};
            const auto nextId = Tailor::Persistence::ReconcileNextId(stored, maxSeen);
            if (!nextId) logger::warn("OutfitStore: no outfit ids are left; no new outfit can be created");
            _outfits = std::move(parsed);
            _nextId = nextId.value_or(kNoIdLeft);
            _saveAllowed = complete;
            logger::info("OutfitStore: loaded {} custom outfits (nextId={})", _outfits.size(), _nextId);

            // Only a complete load changes anything, and one save after the lock writes it all.
            if (complete) {
                // Names are unique, so older duplicates get " (2)", " (3)" ... once, and the first one keeps its name.
                // A numbered name may not land on any other outfit's name, earlier or later in the file.
                std::unordered_set<std::string> allKeys, seen;
                for (const auto& outfit : _outfits) allKeys.insert(Tailor::Outfits::NameKey(outfit.name));
                for (auto& outfit : _outfits) {
                    if (!seen.contains(Tailor::Outfits::NameKey(outfit.name))) {
                        seen.insert(Tailor::Outfits::NameKey(outfit.name));
                        continue;
                    }
                    auto name = Tailor::Outfits::NumberedName(Tailor::Outfits::TrimName(outfit.name), [&](const std::string& key) {
                        return seen.contains(key) || allKeys.contains(key);
                    });
                    logger::info("OutfitStore: outfit {} shared the name '{}'; renamed to '{}'", outfit.id, outfit.name, name);
                    outfit.name = std::move(name);
                    seen.insert(Tailor::Outfits::NameKey(outfit.name));
                    needsSave = true;
                }
                // Ids are unique too: an outfit that shares an earlier outfit's id gets a new one, after a backup.
                if (RenumberDuplicateIds(path)) needsSave = true;
            }
        } catch (const std::exception& e) {
            // What loaded stays in use, but nothing is written over the file.
            _saveAllowed = false;
            needsSave = false;
            logger::error("OutfitStore: could not load outfits.json; the file is kept as it is and saves are off: {}", e.what());
        }
    }  // lock released

    if (needsSave) Save();
}

std::string OutfitStore::Serialize(const std::vector<CustomOutfit>& outfits, int nextId)
{
    nlohmann::json json;
    json["version"] = 1;
    json["nextId"] = nextId;
    json["outfits"] = nlohmann::json::array();

    for (auto& outfit : outfits) {
        nlohmann::json oj;
        oj["id"] = outfit.id;
        oj["name"] = outfit.name;
        oj["sex"] = static_cast<int>(outfit.sex);
        oj["items"] = nlohmann::json::array();

        for (auto& item : outfit.items) {
            oj["items"].push_back({
                {"formId", item.formId},
                {"plugin", item.plugin},
                {"name", item.name}
            });
        }

        json["outfits"].push_back(oj);
    }

    return json.dump(2);
}

bool OutfitStore::Save() const
{
    std::lock_guard lock(_mutex);
    if (!_saveAllowed) {
        logger::error("OutfitStore: save skipped because outfits.json was not loaded completely");
        return false;
    }
    try {
        // Written out in full before any file is written: a name JSON can't hold (bytes that aren't valid UTF-8)
        // throws here and leaves the file as it was.
        const auto text = Serialize(_outfits, _nextId);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(GetStorePath(), text, error)) {
            logger::error("OutfitStore: failed to save outfits.json: {}", error);
            return false;
        }
        logger::info("OutfitStore: saved {} custom outfits", _outfits.size());
        return true;
    } catch (const std::exception& e) {
        logger::error("OutfitStore: failed to save outfits.json: {}", e.what());
        return false;
    }
}

bool OutfitStore::SaveAllowed() const
{
    std::lock_guard lock(_mutex);
    return _saveAllowed;
}

bool OutfitStore::RenumberDuplicateIds(const std::filesystem::path& path)
{
    std::unordered_set<int> seen;
    std::vector<std::size_t> twins;
    for (std::size_t i = 0; i < _outfits.size(); ++i) {
        if (!seen.insert(_outfits[i].id).second) twins.push_back(i);
    }
    if (twins.empty()) return false;
    if (_nextId > kNoIdLeft - static_cast<int>(twins.size())) {
        logger::error("OutfitStore: {} outfit(s) share an id, and too few ids are left to give them new ones", twins.size());
        return false;
    }

    // The file exactly as it was, first, under a name never used before.
    std::error_code error;
    auto backup = path.parent_path() / "outfits.pre-unique-ids.json";
    for (int suffix = 1; std::filesystem::exists(backup, error); ++suffix) {
        backup = path.parent_path() / ("outfits.pre-unique-ids." + std::to_string(suffix) + ".json");
    }
    if (error || !std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none, error)) {
        logger::error("OutfitStore: {} outfit(s) share an id; not renumbered, because outfits.json could not be backed up: {}",
            twins.size(), error.message());
        return false;
    }

    for (const auto index : twins) {
        auto& outfit = _outfits[index];
        const int id = _nextId++;
        logger::info("OutfitStore: outfit '{}' shared the id {} with an earlier outfit; it is now {}", outfit.name, outfit.id, id);
        outfit.id = id;
    }
    logger::info("OutfitStore: outfits.json was backed up to {} before renumbering", backup.string());
    return true;
}

const std::vector<CustomOutfit>& OutfitStore::GetOutfits() const
{
    return _outfits;
}

const CustomOutfit* OutfitStore::GetOutfitById(int id) const
{
    std::lock_guard lock(_mutex);
    for (auto& outfit : _outfits) {
        if (outfit.id == id) {
            return &outfit;
        }
    }
    return nullptr;
}

int OutfitStore::AddOutfit(const std::string& name, const std::vector<ArmorItem>& items, OutfitSex sex)
{
    std::lock_guard lock(_mutex);

    const auto trimmed = Tailor::Outfits::TrimName(name);
    if (trimmed.empty()) {
        logger::warn("OutfitStore: an outfit with no name was not created");
        return 0;
    }
    if (Taken(_outfits, trimmed, 0)) {
        logger::warn("OutfitStore: an outfit named '{}' already exists; not created", trimmed);
        return 0;
    }
    if (_nextId <= 0 || _nextId >= kNoIdLeft) {
        logger::warn("OutfitStore: no outfit ids are left; '{}' was not created", trimmed);
        return 0;
    }

    CustomOutfit outfit;
    outfit.id = _nextId++;
    outfit.name = trimmed;
    outfit.items = items;
    outfit.sex = sex;

    int id = outfit.id;
    _outfits.push_back(std::move(outfit));

    logger::info("OutfitStore: created outfit '{}' (id={}, {} items, {})", trimmed, id, items.size(), OutfitSexName(sex));
    return id;
}

bool OutfitStore::DiscardNewOutfit(int id)
{
    std::lock_guard lock(_mutex);
    if (_outfits.empty() || _outfits.back().id != id || id != _nextId - 1) {
        logger::warn("OutfitStore: outfit {} is not the one just created; it was not taken back", id);
        return false;
    }
    logger::info("OutfitStore: took back outfit '{}' (id={}); it could not be saved", _outfits.back().name, id);
    _outfits.pop_back();
    return true;
}

bool OutfitStore::UpdateOutfit(int id, const std::string& name, const std::vector<ArmorItem>& items, OutfitSex sex)
{
    std::lock_guard lock(_mutex);
    for (auto& outfit : _outfits) {
        if (outfit.id == id) {
            const auto trimmed = Tailor::Outfits::TrimName(name);
            if (trimmed.empty()) {
                logger::warn("OutfitStore: outfit {} was not renamed; the name is empty", id);
                return false;
            }
            if (Taken(_outfits, trimmed, id)) {
                logger::warn("OutfitStore: outfit {} was not renamed; another outfit is named '{}'", id, trimmed);
                return false;
            }
            outfit.name = trimmed;
            outfit.items = items;
            outfit.sex = sex;
            logger::info("OutfitStore: updated outfit '{}' (id={}, {} items, {})", trimmed, id, items.size(), OutfitSexName(sex));
            return true;
        }
    }
    logger::warn("OutfitStore: UpdateOutfit — id {} not found", id);
    return false;
}

bool OutfitStore::NameTaken(std::string_view name, int exceptId) const
{
    std::lock_guard lock(_mutex);
    return Taken(_outfits, name, exceptId);
}

std::optional<CustomOutfit> OutfitStore::FindByName(std::string_view name) const
{
    std::lock_guard lock(_mutex);
    for (const auto& outfit : _outfits) {
        if (Tailor::Outfits::SameName(outfit.name, name)) return outfit;
    }
    return std::nullopt;
}

std::optional<CustomOutfit> OutfitStore::GetOutfitCopy(int id) const
{
    std::lock_guard lock(_mutex);
    for (const auto& outfit : _outfits) {
        if (outfit.id == id) return outfit;
    }
    return std::nullopt;
}

std::vector<CustomOutfit> OutfitStore::Snapshot() const
{
    std::lock_guard lock(_mutex);
    return _outfits;
}

bool OutfitStore::DeleteOutfit(int id)
{
    std::lock_guard lock(_mutex);
    for (auto it = _outfits.begin(); it != _outfits.end(); ++it) {
        if (it->id == id) {
            logger::info("OutfitStore: deleted outfit '{}' (id={})", it->name, id);
            _outfits.erase(it);
            return true;
        }
    }
    logger::warn("OutfitStore: DeleteOutfit — id {} not found", id);
    return false;
}

OutfitStore::SexTagResult OutfitStore::SetOutfitSex(const std::vector<int>& ids, OutfitSex sex)
{
    std::lock_guard lock(_mutex);
    SexTagResult result;
    for (auto& outfit : _outfits) {
        if (std::find(ids.begin(), ids.end(), outfit.id) == ids.end()) continue;
        ++result.tagged;
        if (outfit.sex == sex) continue;
        outfit.sex = sex;
        result.changed.push_back(outfit.id);
    }
    logger::info("OutfitStore: set {} outfit(s) to {}; {} changed", result.tagged, OutfitSexName(sex), result.changed.size());
    return result;
}

bool OutfitStore::Fits(int id, int npcSex) const
{
    std::lock_guard lock(_mutex);
    for (const auto& outfit : _outfits) {
        if (outfit.id == id) return OutfitFits(outfit.sex, npcSex);
    }
    return false;
}

// --- Armor Scanning ---

static const std::set<std::string_view> kSkipPlugins = {};

std::vector<ModArmorList> OutfitStore::ScanArmorMods() const
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::warn("ScanArmorMods: no data handler");
        return {};
    }

    std::map<std::string, std::vector<ArmorItem>> modMap;

    auto& armors = dataHandler->GetFormArray<RE::TESObjectARMO>();
    for (auto* armor : armors) {
        if (!armor) continue;

        auto* file = armor->GetFile(0);
        if (!file) continue;

        std::string pluginName(file->GetFilename());
        if (kSkipPlugins.contains(pluginName)) continue;

        const char* fullName = armor->GetFullName();
        if (!fullName || fullName[0] == '\0') continue;

        ArmorItem item;
        item.formId = armor->GetLocalFormID();
        item.plugin = pluginName;
        item.name = SanitizeUtf8(fullName);

        modMap[item.plugin].push_back(std::move(item));
    }

    std::vector<ModArmorList> result;
    result.reserve(modMap.size());

    for (auto& [modName, items] : modMap) {
        std::sort(items.begin(), items.end(), [](const ArmorItem& a, const ArmorItem& b) {
            return a.name < b.name;
        });
        result.push_back({modName, std::move(items)});
    }

    std::sort(result.begin(), result.end(), [](const ModArmorList& a, const ModArmorList& b) {
        return a.modName < b.modName;
    });

    logger::info("ScanArmorMods: found {} mods with armor records", result.size());
    return result;
}

std::vector<ArmorItem> OutfitStore::GetArmorForPlugin(const std::string& plugin) const
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return {};

    std::vector<ArmorItem> result;
    auto& armors = dataHandler->GetFormArray<RE::TESObjectARMO>();

    for (auto* armor : armors) {
        if (!armor) continue;

        auto* file = armor->GetFile(0);
        if (!file) continue;

        if (std::string(file->GetFilename()) != plugin) continue;

        const char* fullName = armor->GetFullName();
        if (!fullName || fullName[0] == '\0') continue;

        ArmorItem item;
        item.formId = armor->GetLocalFormID();
        item.plugin = plugin;
        item.name = SanitizeUtf8(fullName);

        result.push_back(std::move(item));
    }

    std::sort(result.begin(), result.end(), [](const ArmorItem& a, const ArmorItem& b) {
        return a.name < b.name;
    });

    return result;
}

std::vector<std::string> OutfitStore::GetArmorPluginNames() const
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return {};

    std::set<std::string> pluginSet;
    auto& armors = dataHandler->GetFormArray<RE::TESObjectARMO>();

    for (auto* armor : armors) {
        if (!armor) continue;

        auto* file = armor->GetFile(0);
        if (!file) continue;

        std::string pluginName(file->GetFilename());
        if (kSkipPlugins.contains(pluginName)) continue;

        pluginSet.insert(pluginName);
    }

    return {pluginSet.begin(), pluginSet.end()};
}
