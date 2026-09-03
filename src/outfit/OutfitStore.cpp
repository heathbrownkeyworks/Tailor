#include "outfit/OutfitStore.h"

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
    std::lock_guard lock(_mutex);

    auto path = GetStorePath();
    logger::info("OutfitStore: loading from {}", path.string());

    if (!std::filesystem::exists(path)) {
        logger::info("OutfitStore: no outfits.json found, starting empty");
        _outfits.clear();
        _nextId = 1;

        // Write empty file
        try {
            nlohmann::json json;
            json["version"] = 1;
            json["nextId"] = 1;
            json["outfits"] = nlohmann::json::array();
            std::ofstream file(path);
            if (file.is_open()) {
                file << json.dump(2);
                file.flush();
            }
        } catch (const std::exception& e) {
            logger::error("OutfitStore: exception writing empty file: {}", e.what());
        }
        return;
    }

    try {
        std::ifstream file(path);
        auto json = nlohmann::json::parse(file);

        _outfits.clear();
        _nextId = json.value("nextId", 1);

        if (json.contains("outfits") && json["outfits"].is_array()) {
            for (auto& oj : json["outfits"]) {
                CustomOutfit outfit;
                outfit.id = oj.value("id", 0);
                outfit.name = oj.value("name", std::string{});

                if (oj.contains("items") && oj["items"].is_array()) {
                    for (auto& ij : oj["items"]) {
                        ArmorItem item;
                        item.formId = ij.value("formId", static_cast<RE::FormID>(0));
                        item.plugin = ij.value("plugin", std::string{});
                        item.name = ij.value("name", std::string{});
                        outfit.items.push_back(std::move(item));
                    }
                }

                _outfits.push_back(std::move(outfit));
            }
        }

        logger::info("OutfitStore: loaded {} custom outfits (nextId={})", _outfits.size(), _nextId);
    } catch (const std::exception& e) {
        logger::error("OutfitStore: failed to parse outfits.json: {}", e.what());
        _outfits.clear();
        _nextId = 1;
    }
}

void OutfitStore::Save() const
{
    std::lock_guard lock(_mutex);

    nlohmann::json json;
    json["version"] = 1;
    json["nextId"] = _nextId;
    json["outfits"] = nlohmann::json::array();

    for (auto& outfit : _outfits) {
        nlohmann::json oj;
        oj["id"] = outfit.id;
        oj["name"] = outfit.name;
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

    try {
        auto path = GetStorePath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("OutfitStore: failed to open outfits.json for writing");
            return;
        }
        file << json.dump(2);
        file.flush();
        logger::info("OutfitStore: saved {} custom outfits", _outfits.size());
    } catch (const std::exception& e) {
        logger::error("OutfitStore: failed to save outfits.json: {}", e.what());
    }
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

int OutfitStore::AddOutfit(const std::string& name, const std::vector<ArmorItem>& items)
{
    std::lock_guard lock(_mutex);

    CustomOutfit outfit;
    outfit.id = _nextId++;
    outfit.name = name;
    outfit.items = items;

    int id = outfit.id;
    _outfits.push_back(std::move(outfit));

    logger::info("OutfitStore: created outfit '{}' (id={}, {} items)", name, id, items.size());
    return id;
}

bool OutfitStore::UpdateOutfit(int id, const std::string& name, const std::vector<ArmorItem>& items)
{
    std::lock_guard lock(_mutex);
    for (auto& outfit : _outfits) {
        if (outfit.id == id) {
            outfit.name = name;
            outfit.items = items;
            logger::info("OutfitStore: updated outfit '{}' (id={}, {} items)", name, id, items.size());
            return true;
        }
    }
    logger::warn("OutfitStore: UpdateOutfit — id {} not found", id);
    return false;
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
