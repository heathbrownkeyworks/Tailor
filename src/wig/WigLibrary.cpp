#include "wig/WigLibrary.h"
#include "persistence/AssignmentIdentity.h"
#include "persistence/JsonFile.h"

WigLibrary& WigLibrary::GetSingleton()
{
    static WigLibrary singleton;
    return singleton;
}

std::filesystem::path WigLibrary::GetLibraryPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Wiggy");
    std::filesystem::create_directories(path);
    return path / "library.json";
}

void WigLibrary::Load()
{
    std::lock_guard lock(_mutex);
    ++_revision;
    _saveAllowed = false;
    try {
        const auto path = GetLibraryPath();
        if (!std::filesystem::exists(path)) {
            for (auto& category : _categories) category.clear();
            _saveAllowed = true;
            return;
        }
        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        if (json.value("version", 1) != 1 || !json.contains("categories") || !json["categories"].is_object()) {
            throw std::runtime_error("unsupported wig library document");
        }
        std::array<std::vector<WigEntry>, kCategoryCount> parsed;
        bool complete = true;
        for (size_t i = 0; i < kCategoryCount; ++i) {
            const auto key = std::string(kCategoryNames[i]);
            if (!json["categories"].contains(key)) continue;
            if (!json["categories"][key].is_array()) {
                complete = false;
                logger::error("Wig library: invalid category '{}'; other categories remain active, file preserved and saves disabled", key);
                continue;
            }
            std::size_t row = 0;
            for (const auto& entry : json["categories"][key]) {
                ++row;
                try {
                    WigEntry wig{Tailor::Persistence::ReadLocalFormID(entry.at("formId").get<std::string>()),
                        entry.at("plugin").get<std::string>(), entry.value("name", "")};
                    if (wig.plugin.empty()) throw std::runtime_error("missing wig plugin");
                    if (const auto* armor = wig.Resolve()) wig.name = SanitizeUtf8(armor->GetName());
                    parsed[i].push_back(std::move(wig));
                } catch (const std::exception& e) {
                    complete = false;
                    logger::error("Wig library: category '{}' row {} could not load; other rows remain active, file preserved and saves disabled: {}", key, row, e.what());
                }
            }
        }
        _categories = std::move(parsed);
        _saveAllowed = complete;
        logger::info("Wig library loaded; unavailable wigs retained");
    } catch (const std::exception& e) {
        logger::error("Failed to load wig library; file preserved and saves disabled: {}", e.what());
    }
}

void WigLibrary::Save() const
{
    std::lock_guard lock(_mutex);
    if (!_saveAllowed) {
        logger::error("Wig library: save skipped because the file was not loaded successfully");
        return;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["categories"] = nlohmann::json::object();

    for (size_t i = 0; i < kCategoryCount; ++i) {
        auto key = std::string(kCategoryNames[i]);
        json["categories"][key] = nlohmann::json::array();

        for (auto& wig : _categories[i]) {
            nlohmann::json entry;
            entry["formId"] = std::format("0x{:06X}", wig.formId);
            entry["plugin"] = wig.plugin;
            entry["name"] = wig.name;
            json["categories"][key].push_back(entry);
        }
    }

    try {
        auto path = GetLibraryPath();
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("Failed to save wig library to {}: {}", path.string(), error);
            return;
        }
        logger::info("Wig library saved to {}", path.string());
    }
    catch (const std::exception& e) {
        logger::error("Failed to save wig library: {}", e.what());
    }
}

std::vector<WigEntry> WigLibrary::GetCategory(WigCategory cat) const
{
    std::lock_guard lock(_mutex);
    return _categories[static_cast<size_t>(cat)];
}

size_t WigLibrary::GetCategoryCount(WigCategory cat) const
{
    std::lock_guard lock(_mutex);
    return _categories[static_cast<size_t>(cat)].size();
}

void WigLibrary::AddWig(WigCategory cat, const WigEntry& entry)
{
    std::lock_guard lock(_mutex);

    // Reject if the wig already exists in any category
    for (size_t i = 0; i < kCategoryCount; ++i) {
        for (auto& existing : _categories[i]) {
            if (existing == entry) {
                logger::info("Wig '{}' already in category {} — skipping add to {}",
                    entry.name, CategoryToString(static_cast<WigCategory>(i)), CategoryToString(cat));
                return;
            }
        }
    }

    _categories[static_cast<size_t>(cat)].push_back(entry);
    ++_revision;
    logger::info("Added wig '{}' to category {}", entry.name, CategoryToString(cat));
}

bool WigLibrary::RemoveWig(WigCategory cat, const WigEntry& entry)
{
    std::lock_guard lock(_mutex);
    auto& vec = _categories[static_cast<size_t>(cat)];

    auto it = std::find(vec.begin(), vec.end(), entry);
    if (it != vec.end()) {
        logger::info("Removed wig '{}' from category {}", entry.name, CategoryToString(cat));
        vec.erase(it);
        ++_revision;
        return true;
    }
    return false;
}

bool WigLibrary::HasWig(WigCategory cat, const WigEntry& entry) const
{
    std::lock_guard lock(_mutex);
    auto& vec = _categories[static_cast<size_t>(cat)];
    return std::find(vec.begin(), vec.end(), entry) != vec.end();
}

void WigLibrary::Clear()
{
    std::lock_guard lock(_mutex);
    for (auto& cat : _categories) {
        cat.clear();
    }
    ++_revision;
}

std::vector<RE::TESObjectARMO*> WigLibrary::ResolvedWigs() const
{
    std::vector<RE::TESObjectARMO*> wigs;
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        for (const auto& wig : GetCategory(static_cast<WigCategory>(i))) {
            if (auto* armor = wig.Resolve()) wigs.push_back(armor);
        }
    }
    return wigs;
}

std::uint64_t WigLibrary::Revision() const
{
    std::lock_guard lock(_mutex);
    return _revision;
}
