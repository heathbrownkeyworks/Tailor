#include "outfit/OutfitLibrary.h"

#include <algorithm>
#include <unordered_set>

OutfitLibrary& OutfitLibrary::GetSingleton()
{
    static OutfitLibrary singleton;
    return singleton;
}

std::filesystem::path OutfitLibrary::GetLibraryPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Tailor");
    std::filesystem::create_directories(path);
    return path / "library.json";
}

void OutfitLibrary::CreateDefaults()
{
    _categories.clear();
    _nextId = 1;

    auto addDefault = [&](const std::string& name) {
        OutfitCategory cat;
        cat.id = _nextId++;
        cat.name = name;
        cat.isDefault = true;
        _categories.push_back(std::move(cat));
    };

    addDefault("Heavy Armor");
    addDefault("Light Armor");
    addDefault("Clothing");

    auto addSituationDefault = [&](const std::string& name, const std::string& sitType) {
        OutfitCategory cat;
        cat.id = _nextId++;
        cat.name = name;
        cat.isDefault = true;
        cat.situationType = sitType;
        _categories.push_back(std::move(cat));
    };

    addSituationDefault("Adventuring", "adventuring");
    addSituationDefault("Town",        "town");
    addSituationDefault("Home",        "home");
    addSituationDefault("Sleep",       "sleep");

    logger::info("OutfitLibrary: created {} default categories", _categories.size());
}

void OutfitLibrary::Load()
{
    bool needsMigration = false;

    {
        std::lock_guard lock(_mutex);

        auto path = GetLibraryPath();
        logger::info("OutfitLibrary: loading from {}", path.string());

        if (!std::filesystem::exists(path)) {
            logger::info("OutfitLibrary: no library.json found, creating defaults");
            CreateDefaults();

            try {
                nlohmann::json json;
                json["version"] = 2;
                json["nextId"] = _nextId;
                json["categories"] = nlohmann::json::array();

                for (auto& cat : _categories) {
                    nlohmann::json catJson;
                    catJson["id"] = cat.id;
                    catJson["name"] = cat.name;
                    catJson["sex"] = cat.sex;
                    catJson["isDefault"] = cat.isDefault;
                    if (!cat.situationType.empty()) catJson["situationType"] = cat.situationType;
                    catJson["outfitIds"] = nlohmann::json::array();
                    json["categories"].push_back(catJson);
                }

                std::ofstream file(path);
                if (file.is_open()) {
                    file << json.dump(2);
                    file.flush();
                    logger::info("OutfitLibrary: wrote default library.json");
                } else {
                    logger::error("OutfitLibrary: failed to create library.json at {}", path.string());
                }
            } catch (const std::exception& e) {
                logger::error("OutfitLibrary: exception writing defaults: {}", e.what());
            }
            return;
        }

        try {
            std::ifstream file(path);
            auto json = nlohmann::json::parse(file);

            _categories.clear();
            _nextId = json.value("nextId", 1);

            if (json.contains("categories") && json["categories"].is_array()) {
                for (auto& catJson : json["categories"]) {
                    OutfitCategory cat;
                    cat.id = catJson.value("id", 0);
                    cat.name = catJson.value("name", std::string{});
                    cat.sex = catJson.value("sex", std::string{});
                    cat.isDefault = catJson.value("isDefault", false);
                    cat.situationType = catJson.value("situationType", std::string{});

                    if (catJson.contains("outfitIds") && catJson["outfitIds"].is_array()) {
                        for (auto& idJson : catJson["outfitIds"]) {
                            cat.outfitIds.push_back(idJson.get<int>());
                        }
                    }

                    _categories.push_back(std::move(cat));
                }
            }

            logger::info("OutfitLibrary: loaded {} categories (nextId={})", _categories.size(), _nextId);

            // Keep existing categories, IDs, memberships and legacy metadata intact.
            // Older libraries may lack situations entirely; add one shared pool per missing type.
            auto hasSitCat = [&](const std::string& sitType) {
                for (auto& cat : _categories) {
                    if (cat.situationType == sitType) return true;
                }
                return false;
            };
            auto addSitCat = [&](const std::string& name, const std::string& sitType) {
                if (hasSitCat(sitType)) return;
                OutfitCategory cat;
                cat.id = _nextId++;
                cat.name = name;
                cat.isDefault = true;
                cat.situationType = sitType;
                _categories.push_back(std::move(cat));
                needsMigration = true;
                logger::info("OutfitLibrary: added shared situation category '{}'", name);
            };

            auto situationNames = std::vector<std::pair<std::string, std::string>>{
                {"Adventuring", "adventuring"}, {"Town", "town"},
                {"Home",        "home"},        {"Sleep","sleep"}
            };
            for (auto& [name, sitType] : situationNames) {
                addSitCat(name, sitType);
            }
        } catch (const std::exception& e) {
            logger::error("OutfitLibrary: failed to parse library.json: {}", e.what());
            CreateDefaults();
        }
    }  // lock released

    if (needsMigration) {
        Save();
    }
}

void OutfitLibrary::Save() const
{
    std::lock_guard lock(_mutex);

    nlohmann::json json;
    json["version"] = 2;
    json["nextId"] = _nextId;
    json["categories"] = nlohmann::json::array();

    for (auto& cat : _categories) {
        nlohmann::json catJson;
        catJson["id"] = cat.id;
        catJson["name"] = cat.name;
        catJson["sex"] = cat.sex;
        catJson["isDefault"] = cat.isDefault;
        if (!cat.situationType.empty()) catJson["situationType"] = cat.situationType;
        catJson["outfitIds"] = cat.outfitIds;
        json["categories"].push_back(catJson);
    }

    try {
        auto path = GetLibraryPath();
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("OutfitLibrary: failed to open library.json for writing: {}", path.string());
            return;
        }
        file << json.dump(2);
        file.flush();
        logger::info("OutfitLibrary: saved {} categories to {}", _categories.size(), path.string());
    } catch (const std::exception& e) {
        logger::error("OutfitLibrary: failed to save library.json: {}", e.what());
    }
}

// --- Category accessors ---

const std::vector<OutfitCategory>& OutfitLibrary::GetCategories() const
{
    return _categories;
}

std::string OutfitLibrary::GetCategoryDisplayName(int id) const
{
    std::lock_guard lock(_mutex);
    // Distinguish preserved same-name categories without renaming saved records.
    std::unordered_set<std::string> reserved, used;
    for (const auto& cat : _categories) reserved.insert(cat.name);
    for (const auto& cat : _categories) {
        auto label = cat.name;
        if (used.contains(label)) {
            int suffix = 2;
            do {
                label = cat.name + " (" + std::to_string(suffix++) + ")";
            } while (used.contains(label) || reserved.contains(label));
        }
        used.insert(label);
        if (cat.id == id) return label;
    }
    return {};
}

std::vector<int> OutfitLibrary::GetSituationOutfitIds(const std::string& sitType) const
{
    std::lock_guard lock(_mutex);
    std::vector<int> result;
    for (const auto& cat : _categories) {
        if (cat.situationType == sitType) {
            for (int id : cat.outfitIds) {
                if (id > 0 && std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
            }
        }
    }
    return result;
}

const OutfitCategory* OutfitLibrary::GetCategoryById(int id) const
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        if (cat.id == id) {
            return &cat;
        }
    }
    return nullptr;
}

// --- Category mutations ---

int OutfitLibrary::AddCategory(const std::string& name)
{
    std::lock_guard lock(_mutex);

    OutfitCategory cat;
    cat.id = _nextId++;
    cat.name = name;
    cat.isDefault = false;

    int id = cat.id;
    _categories.push_back(std::move(cat));

    logger::info("OutfitLibrary: added category '{}' with id {}", name, id);
    return id;
}

bool OutfitLibrary::RenameCategory(int id, const std::string& newName)
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        if (cat.id == id) {
            logger::info("OutfitLibrary: renamed category {} from '{}' to '{}'", id, cat.name, newName);
            cat.name = newName;
            return true;
        }
    }
    logger::warn("OutfitLibrary: RenameCategory — id {} not found", id);
    return false;
}

bool OutfitLibrary::DeleteCategory(int id)
{
    std::lock_guard lock(_mutex);
    for (auto it = _categories.begin(); it != _categories.end(); ++it) {
        if (it->id == id) {
            if (it->isDefault) {
                logger::warn("OutfitLibrary: cannot delete default category '{}' (id {})", it->name, id);
                return false;
            }
            logger::info("OutfitLibrary: deleted category '{}' (id {})", it->name, id);
            _categories.erase(it);
            return true;
        }
    }
    logger::warn("OutfitLibrary: DeleteCategory — id {} not found", id);
    return false;
}

// --- Outfit-in-category mutations ---

bool OutfitLibrary::AddOutfitToCategory(int categoryId, int outfitId)
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        if (cat.id == categoryId) {
            for (auto existingId : cat.outfitIds) {
                if (existingId == outfitId) {
                    logger::info("OutfitLibrary: outfit {} already in category '{}'", outfitId, cat.name);
                    return false;
                }
            }
            cat.outfitIds.push_back(outfitId);
            logger::info("OutfitLibrary: added outfit {} to category '{}'", outfitId, cat.name);
            return true;
        }
    }
    logger::warn("OutfitLibrary: AddOutfitToCategory — category id {} not found", categoryId);
    return false;
}

bool OutfitLibrary::RemoveOutfitFromCategory(int categoryId, int outfitId)
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        if (cat.id == categoryId) {
            for (auto it = cat.outfitIds.begin(); it != cat.outfitIds.end(); ++it) {
                if (*it == outfitId) {
                    logger::info("OutfitLibrary: removed outfit {} from category '{}'", outfitId, cat.name);
                    cat.outfitIds.erase(it);
                    return true;
                }
            }
            logger::warn("OutfitLibrary: outfit {} not found in category '{}'", outfitId, cat.name);
            return false;
        }
    }
    logger::warn("OutfitLibrary: RemoveOutfitFromCategory — category id {} not found", categoryId);
    return false;
}

void OutfitLibrary::RemoveOutfitFromAllCategories(int outfitId)
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        std::erase(cat.outfitIds, outfitId);
    }
    logger::info("OutfitLibrary: removed outfit {} from all categories", outfitId);
}

void OutfitLibrary::SetOutfitCategories(int outfitId, const std::vector<int>& categoryIds)
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        const bool selected = std::find(categoryIds.begin(), categoryIds.end(), cat.id) != categoryIds.end();
        if (!selected) {
            std::erase(cat.outfitIds, outfitId);
        } else if (std::find(cat.outfitIds.begin(), cat.outfitIds.end(), outfitId) == cat.outfitIds.end()) {
            cat.outfitIds.push_back(outfitId);
        }
    }
}
