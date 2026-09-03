#include "wig/WigLibrary.h"

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
    int prunedCount = 0;

    {
        std::lock_guard lock(_mutex);

        auto path = GetLibraryPath();
        logger::info("Library path: {}", path.string());

        if (!std::filesystem::exists(path)) {
            logger::info("No library file found, creating empty library");
            try {
                nlohmann::json json;
                json["version"] = 1;
                json["categories"] = nlohmann::json::object();
                std::ofstream file(path);
                if (!file.is_open()) {
                    logger::error("Failed to create library file at {}", path.string());
                    return;
                }
                file << json.dump(2);
                file.flush();
                logger::info("Created empty library at {}", path.string());
            }
            catch (const std::exception& e) {
                logger::error("Exception creating library file: {}", e.what());
            }
            return;
        }

        try {
            std::ifstream file(path);
            auto json = nlohmann::json::parse(file);

            for (size_t i = 0; i < kCategoryCount; ++i) {
                auto key = std::string(kCategoryNames[i]);
                _categories[i].clear();

                if (!json.contains("categories") || !json["categories"].contains(key)) {
                    continue;
                }

                for (auto& entry : json["categories"][key]) {
                    WigEntry wig;
                    wig.formId = std::stoul(entry["formId"].get<std::string>(), nullptr, 16);
                    wig.plugin = entry["plugin"].get<std::string>();
                    wig.name = entry.value("name", "");

                    auto* armor = wig.Resolve();
                    if (!armor) {
                        logger::warn("Pruning stale wig '{}' from '{}' — plugin not loaded",
                            wig.name, wig.plugin);
                        prunedCount++;
                        continue;
                    }
                    wig.name = SanitizeUtf8(armor->GetName());
                    _categories[i].push_back(std::move(wig));
                }
            }

            logger::info("Wig library loaded from {}", path.string());
        }
        catch (const std::exception& e) {
            logger::error("Failed to load wig library: {}", e.what());
        }
    }

    // Clean stale entries from the JSON file (lock released, Save() acquires its own)
    if (prunedCount > 0) {
        logger::info("Pruned {} stale wig(s) from uninstalled mods — saving cleaned library", prunedCount);
        Save();
    }
}

void WigLibrary::Save() const
{
    std::lock_guard lock(_mutex);

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
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("Failed to open library file for writing: {}", path.string());
            return;
        }
        file << json.dump(2);
        file.flush();
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
}
