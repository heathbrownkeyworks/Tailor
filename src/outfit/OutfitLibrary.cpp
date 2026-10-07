#include "outfit/OutfitLibrary.h"

#include "outfit/OutfitNamePolicy.h"
#include "persistence/JsonFile.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace
{
    OutfitArmorType LegacyArmorType(const OutfitCategory& category)
    {
        if (!category.isDefault || !category.situationType.empty()) return OutfitArmorType::Any;
        // Released defaults reserved 1/2/3 for Heavy/Light/Clothing and 4/5/6
        // for their former male counterparts. Preserve renamed defaults.
        if (category.id >= 1 && category.id <= 6) {
            constexpr OutfitArmorType types[] = {
                OutfitArmorType::Heavy, OutfitArmorType::Light, OutfitArmorType::Clothing };
            return types[(category.id - 1) % 3];
        }
        if (category.name == "Heavy Armor") return OutfitArmorType::Heavy;
        if (category.name == "Light Armor") return OutfitArmorType::Light;
        if (category.name == "Clothing") return OutfitArmorType::Clothing;
        return OutfitArmorType::Any;
    }

    // Tailor never hands out INT_MAX; a counter that reaches it means no id is left.
    constexpr int kNoIdLeft = (std::numeric_limits<int>::max)();
}

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
        cat.armorType = LegacyArmorType(cat);
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
    addSituationDefault("Swimming",    "swimming");
    addSituationDefault("Warm",        "warm");

    logger::info("OutfitLibrary: created {} default categories", _categories.size());
}

void OutfitLibrary::Load()
{
    bool needsMigration = false;

    {
        std::lock_guard lock(_mutex);
        // Saves stay off until this load has read the whole file.
        _saveAllowed = false;
        try {
            const auto path = GetLibraryPath();
            logger::info("OutfitLibrary: loading from {}", path.string());

            std::error_code error;
            if (!std::filesystem::exists(path, error)) {
                // A file Tailor can't check is not a missing file: saves stay off.
                if (error) throw std::filesystem::filesystem_error("could not check library.json", path, error);
                // No file yet: the default categories, written the first time something changes.
                logger::info("OutfitLibrary: no library.json found, using the default categories");
                CreateDefaults();
                _saveAllowed = true;
                return;
            }

            std::ifstream file(path);
            const auto json = nlohmann::json::parse(file);
            // Version 1 is what 705ef0945 wrote; version 2 is today's.
            if (!json.is_object() || (json.contains("version") && json["version"] != 1 && json["version"] != 2) ||
                !json.contains("categories") || !json["categories"].is_array()) {
                throw std::runtime_error("unsupported library document");
            }

            // Each row on its own, so a bad row never drops the categories around it. A bad row's id stays reserved
            // when the row gives it as a whole number. Rows that share an id stay as they are (UNRESTRICTED_OUTFITS.md).
            std::vector<OutfitCategory> parsed;
            bool complete = true;
            int maxSeen = 0;
            std::size_t row = 0;
            for (const auto& catJson : json["categories"]) {
                ++row;
                try {
                    const auto& categoryId = catJson.at("id");
                    if (!categoryId.is_number_integer()) throw std::runtime_error("the id is not a whole number");
                    const auto id = categoryId.get<std::int64_t>();
                    if (id <= 0 || id > kNoIdLeft) throw std::runtime_error("the id is not a number from 1 up");

                    OutfitCategory cat;
                    cat.id = static_cast<int>(id);
                    cat.name = catJson.value("name", std::string{});
                    cat.sex = catJson.value("sex", std::string{});
                    cat.isDefault = catJson.value("isDefault", false);
                    cat.situationType = catJson.value("situationType", std::string{});
                    cat.armorType = catJson.contains("armorType") && catJson["armorType"].is_string()
                        ? ParseOutfitArmorType(catJson["armorType"].get<std::string>()) : LegacyArmorType(cat);

                    if (catJson.contains("outfitIds")) {
                        if (!catJson["outfitIds"].is_array()) throw std::runtime_error("outfitIds is not a list");
                        for (const auto& outfitId : catJson["outfitIds"]) cat.outfitIds.push_back(outfitId.get<int>());
                    }

                    maxSeen = (std::max)(maxSeen, cat.id);
                    parsed.push_back(std::move(cat));
                } catch (const std::exception& e) {
                    complete = false;
                    if (catJson.is_object() && catJson.contains("id") && catJson["id"].is_number_integer() && catJson["id"].get<std::int64_t>() > 0) {
                        maxSeen = (std::max)(maxSeen, static_cast<int>((std::min)(catJson["id"].get<std::int64_t>(), std::int64_t{kNoIdLeft})));
                    }
                    logger::error("OutfitLibrary: category row {} could not load; the other categories stay, library.json is kept as it is and saves are off: {}", row, e.what());
                }
            }

            // A counter of the wrong type counts as missing. Reconciled before anything below hands out an id.
            const auto stored = json.contains("nextId") && json["nextId"].is_number_integer()
                ? json["nextId"].get<std::int64_t>() : std::int64_t{1};
            const auto nextId = Tailor::Persistence::ReconcileNextId(stored, maxSeen);
            if (!nextId) logger::warn("OutfitLibrary: no category ids are left; no new category can be created");
            _categories = std::move(parsed);
            _nextId = nextId.value_or(kNoIdLeft);
            _saveAllowed = complete;
            logger::info("OutfitLibrary: loaded {} categories (nextId={})", _categories.size(), _nextId);

            // Only a complete load merges or adds anything.
            if (complete) {
                needsMigration = MergeLegacyDefaultCategories();

                // Older libraries may lack situations entirely; add one shared pool per missing type.
                auto hasSitCat = [&](const std::string& sitType) {
                    for (auto& cat : _categories) {
                        if (cat.situationType == sitType) return true;
                    }
                    return false;
                };
                auto addSitCat = [&](const std::string& name, const std::string& sitType) {
                    if (hasSitCat(sitType)) return;
                    if (_nextId >= kNoIdLeft) {
                        logger::warn("OutfitLibrary: no category id is left for the '{}' pool", name);
                        return;
                    }
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
                    {"Home",        "home"},        {"Sleep","sleep"}, {"Swimming", "swimming"}, {"Warm", "warm"}
                };
                for (auto& [name, sitType] : situationNames) {
                    addSitCat(name, sitType);
                }
            }
        } catch (const std::exception& e) {
            // The defaults show, but nothing is written over the file.
            logger::error("OutfitLibrary: could not load library.json; showing the default categories, the file is kept as it is and saves are off: {}", e.what());
            CreateDefaults();
            _saveAllowed = false;
            needsMigration = false;
        }
    }  // lock released

    if (needsMigration) {
        Save();
    }
}

std::string OutfitLibrary::Serialize(const std::vector<OutfitCategory>& categories, int nextId)
{
    nlohmann::json json;
    json["version"] = 2;
    json["nextId"] = nextId;
    json["categories"] = nlohmann::json::array();

    for (auto& cat : categories) {
        nlohmann::json catJson;
        catJson["id"] = cat.id;
        catJson["name"] = cat.name;
        catJson["sex"] = cat.sex;
        catJson["isDefault"] = cat.isDefault;
        if (!cat.situationType.empty()) catJson["situationType"] = cat.situationType;
        if (cat.armorType != OutfitArmorType::Any) catJson["armorType"] = OutfitArmorTypeName(cat.armorType);
        catJson["outfitIds"] = cat.outfitIds;
        json["categories"].push_back(catJson);
    }

    return json.dump(2);
}

bool OutfitLibrary::Save() const
{
    std::lock_guard lock(_mutex);
    if (!_saveAllowed) {
        logger::error("OutfitLibrary: save skipped because library.json was not loaded completely");
        return false;
    }
    try {
        // Written out in full before any file is written: a name JSON can't hold (bytes that aren't valid UTF-8)
        // throws here and leaves the file as it was.
        const auto text = Serialize(_categories, _nextId);
        const auto path = GetLibraryPath();
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, text, error)) {
            logger::error("OutfitLibrary: failed to save library.json: {}", error);
            return false;
        }
        logger::info("OutfitLibrary: saved {} categories to {}", _categories.size(), path.string());
        return true;
    } catch (const std::exception& e) {
        logger::error("OutfitLibrary: failed to save library.json: {}", e.what());
        return false;
    }
}

bool OutfitLibrary::SaveAllowed() const
{
    std::lock_guard lock(_mutex);
    return _saveAllowed;
}

bool OutfitLibrary::MergeLegacyDefaultCategories()
{
    auto merged = _categories;
    for (std::size_t i = 0; i < merged.size(); ++i) {
        auto& category = merged[i];
        if (!category.isDefault || (category.sex != "female" && category.sex != "male")) continue;

        const auto partner = std::find_if(merged.begin() + i + 1, merged.end(),
            [&](const OutfitCategory& other) {
                return other.isDefault && other.name == category.name &&
                    other.situationType == category.situationType &&
                    other.armorType == category.armorType &&
                    other.sex == (category.sex == "female" ? "male" : "female");
            });
        if (partner == merged.end()) continue;

        std::vector<int> outfitIds;
        for (const auto* ids : {&category.outfitIds, &partner->outfitIds}) {
            for (int id : *ids) {
                if (std::find(outfitIds.begin(), outfitIds.end(), id) == outfitIds.end()) {
                    outfitIds.push_back(id);
                }
            }
        }
        category.outfitIds = std::move(outfitIds);
        category.sex.clear();
        merged.erase(partner);
    }
    if (merged.size() == _categories.size()) return false;

    // Preserve the exact input before any migration write. A later import of another
    // legacy library gets its own backup; never overwrite an earlier backup.
    const auto path = GetLibraryPath();
    auto backup = path.parent_path() / "library.pre-unisex-defaults.json";
    std::error_code error;
    for (int suffix = 1; std::filesystem::exists(backup, error); ++suffix) {
        backup = path.parent_path() / ("library.pre-unisex-defaults." + std::to_string(suffix) + ".json");
    }
    // A backup Tailor can't check for is a backup problem, and skips the merge (LEGACY_DEFAULT_CATEGORIES.md).
    if (error) {
        logger::error("OutfitLibrary: default-category migration skipped; could not check for an earlier backup: {}", error.message());
        return false;
    }
    if (!std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none, error)) {
        logger::error("OutfitLibrary: default-category migration skipped; backup failed: {}", error.message());
        return false;
    }

    logger::info("OutfitLibrary: merged {} legacy default category pairs; original library backed up to {}",
        _categories.size() - merged.size(), backup.string());
    _categories = std::move(merged);
    // Outfit IDs and _nextId are unchanged. Assignments reference outfits, not categories.
    return true;
}

// --- Category accessors ---

const std::vector<OutfitCategory>& OutfitLibrary::GetCategories() const
{
    return _categories;
}

std::vector<std::pair<int, std::string>> OutfitLibrary::DisplayLabels() const
{
    // Distinguish preserved same-name categories without renaming saved records.
    std::unordered_set<std::string> reserved, used;
    for (const auto& cat : _categories) reserved.insert(cat.name);
    std::vector<std::pair<int, std::string>> labels;
    labels.reserve(_categories.size());
    for (const auto& cat : _categories) {
        auto label = cat.name;
        if (used.contains(label)) {
            int suffix = 2;
            do {
                label = cat.name + " (" + std::to_string(suffix++) + ")";
            } while (used.contains(label) || reserved.contains(label));
        }
        used.insert(label);
        labels.emplace_back(cat.id, std::move(label));
    }
    return labels;
}

std::string OutfitLibrary::GetCategoryDisplayName(int id) const
{
    std::lock_guard lock(_mutex);
    for (auto& [categoryId, label] : DisplayLabels()) {
        if (categoryId == id) return label;
    }
    return {};
}

std::optional<int> OutfitLibrary::FindCategoryByDisplayName(std::string_view name) const
{
    std::lock_guard lock(_mutex);
    for (auto& [categoryId, label] : DisplayLabels()) {
        if (Tailor::Outfits::SameName(label, name)) return categoryId;
    }
    return std::nullopt;
}

std::vector<OutfitCategory> OutfitLibrary::Snapshot() const
{
    std::lock_guard lock(_mutex);
    return _categories;
}

bool OutfitLibrary::NameTakenLocked(std::string_view name, int exceptId) const
{
    return std::any_of(_categories.begin(), _categories.end(), [&](const OutfitCategory& cat) {
        return cat.id != exceptId && Tailor::Outfits::SameName(cat.name, name);
    });
}

bool OutfitLibrary::CategoryNameTaken(std::string_view name, int exceptId) const
{
    std::lock_guard lock(_mutex);
    return NameTakenLocked(name, exceptId);
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

std::vector<int> OutfitLibrary::FilterByArmorType(const std::vector<int>& outfitIds, OutfitArmorType type) const
{
    if (type == OutfitArmorType::Any) return outfitIds;
    std::lock_guard lock(_mutex);
    std::unordered_set<int> allowed;
    for (const auto& category : _categories) {
        if (category.armorType == type) {
            allowed.insert(category.outfitIds.begin(), category.outfitIds.end());
        }
    }
    std::vector<int> result;
    for (int id : outfitIds) {
        if (allowed.contains(id)) result.push_back(id);
    }
    return result;
}

std::vector<int> OutfitLibrary::FilterAdventuringEligible(const std::vector<int>& outfitIds, OutfitArmorType type, const Wearable& wearable) const
{
    std::vector<int> result;
    for (int id : outfitIds) if (wearable(id)) result.push_back(id);
    if (type == OutfitArmorType::Any) return result;
    auto pool = GetSituationOutfitIds("adventuring");
    std::erase_if(pool, [&](int id) { return !wearable(id); });
    auto both = FilterByArmorType(pool, type);
    // Nothing wearable is in both, so the type cannot be honored. All of Adventuring
    // the NPC can wear stands in rather than nothing; never an outfit from outside.
    if (both.empty()) both = pool;
    const std::unordered_set<int> eligible(both.begin(), both.end());
    std::erase_if(result, [&](int id) { return !eligible.contains(id); });
    return result;
}

bool OutfitLibrary::IsAdventuringEligible(int outfitId, OutfitArmorType type, const Wearable& wearable) const
{
    return outfitId > 0 && !FilterAdventuringEligible({outfitId}, type, wearable).empty();
}

bool OutfitLibrary::HasAdventuringOutfitsOfType(OutfitArmorType type, const Wearable& wearable) const
{
    auto pool = GetSituationOutfitIds("adventuring");
    std::erase_if(pool, [&](int id) { return !wearable(id); });
    return !FilterByArmorType(pool, type).empty();
}

// --- Category mutations ---

int OutfitLibrary::AddCategory(const std::string& name)
{
    std::lock_guard lock(_mutex);

    const auto trimmed = Tailor::Outfits::TrimName(name);
    if (trimmed.empty()) {
        logger::warn("OutfitLibrary: a category with no name was not created");
        return 0;
    }
    if (NameTakenLocked(trimmed, 0)) {
        logger::warn("OutfitLibrary: a category named '{}' already exists; not created", trimmed);
        return 0;
    }
    if (_nextId <= 0 || _nextId >= kNoIdLeft) {
        logger::warn("OutfitLibrary: no category ids are left; '{}' was not created", trimmed);
        return 0;
    }

    OutfitCategory cat;
    cat.id = _nextId++;
    cat.name = trimmed;
    cat.isDefault = false;

    int id = cat.id;
    _categories.push_back(std::move(cat));

    logger::info("OutfitLibrary: added category '{}' with id {}", trimmed, id);
    return id;
}

bool OutfitLibrary::RenameCategory(int id, const std::string& newName)
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        if (cat.id == id) {
            const auto trimmed = Tailor::Outfits::TrimName(newName);
            if (trimmed.empty()) {
                logger::warn("OutfitLibrary: category {} was not renamed; the name is empty", id);
                return false;
            }
            if (NameTakenLocked(trimmed, id)) {
                logger::warn("OutfitLibrary: category {} was not renamed; another category is named '{}'", id, trimmed);
                return false;
            }
            logger::info("OutfitLibrary: renamed category {} from '{}' to '{}'", id, cat.name, trimmed);
            cat.name = trimmed;
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
