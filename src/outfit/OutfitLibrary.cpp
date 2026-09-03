#include "outfit/OutfitLibrary.h"

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

    auto addDefault = [&](const std::string& name, const std::string& sex) {
        OutfitCategory cat;
        cat.id = _nextId++;
        cat.name = name;
        cat.sex = sex;
        cat.isDefault = true;
        _categories.push_back(std::move(cat));
    };

    addDefault("Heavy Armor", "female");
    addDefault("Light Armor", "female");
    addDefault("Clothing", "female");
    addDefault("Heavy Armor", "male");
    addDefault("Light Armor", "male");
    addDefault("Clothing", "male");

    // Situation pool categories — sex-specific (v2.0+) so NPCs only see outfits matching their sex.
    auto addSituationDefault = [&](const std::string& name, const std::string& sitType, const std::string& sex) {
        OutfitCategory cat;
        cat.id = _nextId++;
        cat.name = name;
        cat.sex = sex;
        cat.isDefault = true;
        cat.situationType = sitType;
        _categories.push_back(std::move(cat));
    };

    addSituationDefault("Adventuring", "adventuring", "female");
    addSituationDefault("Town",        "town",        "female");
    addSituationDefault("Home",        "home",        "female");
    addSituationDefault("Sleep",       "sleep",       "female");
    addSituationDefault("Adventuring", "adventuring", "male");
    addSituationDefault("Town",        "town",        "male");
    addSituationDefault("Home",        "home",        "male");
    addSituationDefault("Sleep",       "sleep",       "male");

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

            // v2.0 migration: situation pool categories are now sex-specific (one female + one male per situationType).
            // Any legacy entry with sex="" gets re-tagged as "female" (preserves the data in the female pool);
            // a matching empty "male" entry is added if missing.
            auto hasSitCatForSex = [&](const std::string& sitType, const std::string& sex) {
                for (auto& cat : _categories) {
                    if (cat.situationType == sitType && cat.sex == sex) return true;
                }
                return false;
            };
            auto addSitCatForSex = [&](const std::string& name, const std::string& sitType, const std::string& sex) {
                if (hasSitCatForSex(sitType, sex)) return;
                OutfitCategory cat;
                cat.id = _nextId++;
                cat.name = name;
                cat.sex = sex;
                cat.isDefault = true;
                cat.situationType = sitType;
                _categories.push_back(std::move(cat));
                needsMigration = true;
                logger::info("OutfitLibrary: migrated — added situation category '{}' ({})", name, sex);
            };

            // Step 1: re-tag any legacy unisex situation pool as female (preserves existing assignments)
            for (auto& cat : _categories) {
                if (!cat.situationType.empty() && cat.sex.empty()) {
                    cat.sex = "female";
                    needsMigration = true;
                    logger::info("OutfitLibrary: migrated — relabeled '{}' situation pool sex \"\" -> \"female\"", cat.name);
                }
            }
            // Step 2: ensure both sexes have a pool for each situationType
            auto situationNames = std::vector<std::pair<std::string, std::string>>{
                {"Adventuring", "adventuring"}, {"Town", "town"},
                {"Home",        "home"},        {"Sleep","sleep"}
            };
            for (auto& [name, sitType] : situationNames) {
                addSitCatForSex(name, sitType, "female");
                addSitCatForSex(name, sitType, "male");
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

std::vector<OutfitCategory> OutfitLibrary::GetCategoriesForSex(const std::string& sex) const
{
    std::lock_guard lock(_mutex);
    std::vector<OutfitCategory> result;
    for (auto& cat : _categories) {
        if (cat.sex == sex || cat.sex.empty()) {
            result.push_back(cat);
        }
    }
    return result;
}

const OutfitCategory* OutfitLibrary::GetCategoryBySituationType(const std::string& sitType, const std::string& sex) const
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        if (cat.situationType == sitType && cat.sex == sex) {
            return &cat;
        }
    }
    return nullptr;
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

int OutfitLibrary::AddCategory(const std::string& name, const std::string& sex)
{
    std::lock_guard lock(_mutex);

    OutfitCategory cat;
    cat.id = _nextId++;
    cat.name = name;
    cat.sex = sex;
    cat.isDefault = false;

    int id = cat.id;
    _categories.push_back(std::move(cat));

    logger::info("OutfitLibrary: added category '{}' ({}) with id {}", name, sex, id);
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
