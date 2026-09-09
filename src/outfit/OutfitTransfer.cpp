#include "outfit/OutfitTransfer.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitStore.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <set>

namespace
{
    using Json = nlohmann::json;
    namespace fs = std::filesystem;
    fs::path Root() { return fs::path("Data/SKSE/Plugins/Tailor"); }
    std::string Lower(std::string value)
    {
        for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return value;
    }
    bool Protected(const std::string& filename)
    {
        const auto lower = Lower(filename);
        return lower == "assignments.json" || lower == "library.json" || lower == "outfits.json" ||
            lower == "blacklist.json" || lower.starts_with("library.pre-unisex-defaults.");
    }
    bool InvalidFilenameChars(const std::string& name)
    {
        return std::any_of(name.begin(), name.end(), [](unsigned char c) {
            return c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
        });
    }
    bool KnownKey(const std::string& key)
    {
        return key == "heavy" || key == "light" || key == "clothing" || key == "adventuring" ||
            key == "town" || key == "home" || key == "sleep";
    }
    Json ItemsJson(const CustomOutfit& outfit)
    {
        auto items = Json::array();
        for (const auto& item : outfit.items) items.push_back({{"formId", item.formId}, {"name", item.name}, {"plugin", item.plugin}});
        return items;
    }
    std::vector<std::string> CategoriesFor(int id, const std::vector<OutfitCategory>& categories)
    {
        std::set<std::string> keys;
        for (const auto& category : categories) {
            auto key = OutfitTransfer::CategoryKey(category);
            if (!key.empty() && std::find(category.outfitIds.begin(), category.outfitIds.end(), id) != category.outfitIds.end()) keys.insert(std::move(key));
        }
        return {keys.begin(), keys.end()};
    }
    using Identity = std::pair<std::string, std::set<std::pair<std::string, RE::FormID>>>;
    Identity OutfitIdentity(const CustomOutfit& outfit)
    {
        Identity result{outfit.name, {}};
        for (const auto& item : outfit.items) result.second.emplace(Lower(item.plugin), item.formId);
        return result;
    }
    void WriteNew(const fs::path& path, const std::string& content)
    {
        // Exclusive creation also protects against repeated/racing export requests.
        std::ofstream file(path, std::ios::binary | std::ios::out | std::ios::noreplace);
        if (!file.is_open()) throw std::runtime_error("Cannot create the file. Use a new name and check folder permissions.");
        file << content;
        file.flush();
        const bool written = file.good();
        file.close();
        if (!written || file.fail()) {
            std::error_code ignored;
            fs::remove(path, ignored);
            throw std::runtime_error("Could not finish writing the file. Check available disk space and folder permissions.");
        }
    }
    void CommitFiles(const std::string& outfits, const std::string& library)
    {
        // Prepare both files and exact backups before replacing either document.
        // Append outfits first so interruption cannot invalidate existing memberships.
        const auto stage = Root() / (".import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directory(stage)) throw std::runtime_error("Could not create the import staging folder.");
        const auto outfitPath = Root() / "outfits.json";
        const auto libraryPath = Root() / "library.json";
        bool replacedOutfits = false;
        const bool hadOutfits = fs::exists(outfitPath);
        try {
            if (hadOutfits) fs::copy_file(outfitPath, stage / "outfits.backup");
            if (fs::exists(libraryPath)) fs::copy_file(libraryPath, stage / "library.backup");
            WriteNew(stage / "outfits.next", outfits);
            WriteNew(stage / "library.next", library);
            fs::rename(stage / "outfits.next", outfitPath);
            replacedOutfits = true;
            fs::rename(stage / "library.next", libraryPath);
        } catch (...) {
            if (replacedOutfits) {
                std::error_code error;
                if (hadOutfits) fs::rename(stage / "outfits.backup", outfitPath, error);
                else fs::remove(outfitPath, error);
                if (error) throw std::runtime_error("Import failed and automatic recovery failed. Original files are in " + stage.string());
            }
            std::error_code ignored;
            fs::remove_all(stage, ignored);
            throw;
        }
        std::error_code ignored;
        fs::remove_all(stage, ignored);
    }
}

std::string OutfitTransfer::CategoryKey(const OutfitCategory& category)
{
    if (!category.isDefault) return {};
    if (!category.situationType.empty()) return KnownKey(category.situationType) ? category.situationType : "";
    return category.armorType != OutfitArmorType::Any ? std::string(OutfitArmorTypeName(category.armorType)) : "";
}

std::string OutfitTransfer::ValidateExportName(const std::string& name)
{
    if (name.empty() || name.find_first_not_of(' ') == std::string::npos) return "Enter a filename before exporting.";
    if (name.size() > 120) return "Use a shorter filename.";
    if (name.front() == ' ' || name.back() == ' ') return "Remove spaces at the start or end of the filename.";
    if (name.find('.') != std::string::npos) return "Enter a name without dots or a file extension. Tailor adds .json.";
    if (InvalidFilenameChars(name)) return "Use a filename without paths or special characters: < > : \" / \\ | ? *";
    if (Protected(name + ".json")) return "That filename is reserved for Tailor. Choose another name.";
    const auto lower = Lower(name);
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul" || lower == "conin$" || lower == "conout$" ||
        (lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) && lower[3] >= '0' && lower[3] <= '9')) {
        return "That filename is reserved by Windows. Choose another name.";
    }
    return {};
}

bool OutfitTransfer::IsImportFilename(const std::string& filename)
{
    return filename.size() > 5 && filename.size() <= 240 && !InvalidFilenameChars(filename) &&
        filename.front() != '.' && filename.front() != ' ' && Lower(filename).ends_with(".json") && !Protected(filename);
}

nlohmann::json OutfitTransfer::Catalog()
{
    auto& store = OutfitStore::GetSingleton();
    auto& library = OutfitLibrary::GetSingleton();
    std::scoped_lock lock(store._mutex, library._mutex);
    auto outfits = Json::array();
    for (const auto& outfit : store._outfits) {
        auto categories = CategoriesFor(outfit.id, library._categories);
        if (categories.empty() || outfit.items.empty()) continue;
        outfits.push_back({{"id", outfit.id}, {"name", outfit.name}, {"categories", categories}, {"itemCount", outfit.items.size()}});
    }
    return outfits;
}

std::vector<std::string> OutfitTransfer::ListFiles()
{
    std::vector<std::string> files;
    if (!fs::exists(Root())) return files;
    for (const auto& entry : fs::directory_iterator(Root())) {
        const auto bytes = entry.path().filename().u8string();
        const std::string name(bytes.begin(), bytes.end());
        if (IsImportFilename(name) && fs::is_regular_file(entry.symlink_status())) files.push_back(name);
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return Lower(a) < Lower(b); });
    return files;
}

nlohmann::json OutfitTransfer::Export(const std::string& name, const std::vector<int>& ids)
{
    const auto error = ValidateExportName(name);
    if (!error.empty()) throw std::runtime_error(error);
    if (ids.empty()) throw std::runtime_error("Select at least one outfit to export.");
    auto& store = OutfitStore::GetSingleton();
    auto& library = OutfitLibrary::GetSingleton();
    std::scoped_lock lock(store._mutex, library._mutex);
    auto selected = Json::array();
    const std::set<int> unique(ids.begin(), ids.end());
    for (int id : unique) {
        const auto outfit = std::find_if(store._outfits.begin(), store._outfits.end(), [id](const auto& o) { return o.id == id; });
        const auto categories = CategoriesFor(id, library._categories);
        if (outfit == store._outfits.end() || categories.empty() || outfit->items.empty()) throw std::runtime_error("A selected outfit is no longer exportable. Refresh the list and try again.");
        selected.push_back({{"name", outfit->name}, {"categories", categories}, {"items", ItemsJson(*outfit)}});
    }
    fs::create_directories(Root());
    const auto filename = name + ".json";
    WriteNew(Root() / fs::path(std::u8string(filename.begin(), filename.end())), Json({{"format", "TailorOutfitExport"}, {"version", 1}, {"outfits", selected}}).dump(2));
    return {{"ok", true}, {"operation", "export"}, {"file", filename}, {"exported", selected.size()}};
}

nlohmann::json OutfitTransfer::Import(const std::string& filename)
{
    if (!IsImportFilename(filename)) throw std::runtime_error("This filename cannot be imported into Tailor.");
    const auto path = Root() / fs::path(std::u8string(filename.begin(), filename.end()));
    if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error("Choose a regular JSON file from the Tailor folder.");
    if (fs::file_size(path) > 32 * 1024 * 1024) throw std::runtime_error("The import file exceeds the 32 MB limit.");
    std::ifstream file(path, std::ios::binary);
    const auto json = Json::parse(file);
    if (!json.is_object() || json.value("format", std::string("TailorOutfitExport")) != "TailorOutfitExport" ||
        (json.contains("version") && (!json["version"].is_number_integer() || json["version"] != 1)) ||
        json.contains("nextId") || !json.contains("outfits") || !json["outfits"].is_array()) {
        throw std::runtime_error("This is not a supported Tailor outfit export file.");
    }
    auto& store = OutfitStore::GetSingleton();
    auto& library = OutfitLibrary::GetSingleton();
    std::scoped_lock lock(store._mutex, library._mutex);
    auto outfits = store._outfits;
    auto categories = library._categories;
    int nextId = store._nextId;
    std::set<Identity> identities;
    for (const auto& outfit : outfits) {
        identities.insert(OutfitIdentity(outfit));
        if (outfit.id >= nextId) {
            if (outfit.id == (std::numeric_limits<int>::max)()) throw std::runtime_error("No outfit IDs are available.");
            nextId = outfit.id + 1;
        }
    }
    int added = 0, duplicates = 0;
    auto skipped = Json::array();
    for (const auto& entry : json["outfits"]) {
        std::string name = "Unnamed outfit";
        try {
            if (!entry.is_object()) throw std::runtime_error("Invalid outfit entry.");
            name = entry.at("name").get<std::string>();
            if (name.empty() || name.find_first_not_of(" \t\r\n") == std::string::npos) throw std::runtime_error("Outfit name is empty.");
            const auto keys = entry.at("categories").get<std::vector<std::string>>();
            if (keys.empty()) throw std::runtime_error("No built-in categories are specified.");
            std::set<int> targetCategories;
            for (const auto& key : keys) {
                if (!KnownKey(key)) throw std::runtime_error("Unsupported category: " + key);
                const auto category = std::find_if(categories.begin(), categories.end(), [&](const auto& c) { return CategoryKey(c) == key; });
                if (category == categories.end()) throw std::runtime_error("Built-in category is unavailable: " + key);
                targetCategories.insert(category->id);
            }
            if (!entry.at("items").is_array() || entry["items"].empty()) throw std::runtime_error("No armor items are specified.");
            CustomOutfit outfit;
            outfit.name = name;
            for (const auto& itemJson : entry["items"]) {
                ArmorItem item;
                const auto& id = itemJson.at("formId");
                if (!id.is_number_integer() || id < 1 || id > 0xFFFFFF) throw std::runtime_error("An item has an invalid plugin-local FormID.");
                item.formId = id.get<RE::FormID>();
                item.plugin = itemJson.at("plugin").get<std::string>();
                item.name = itemJson.at("name").get<std::string>();
                const auto plugin = Lower(item.plugin);
                if (InvalidFilenameChars(item.plugin) || !(plugin.ends_with(".esp") || plugin.ends_with(".esm") || plugin.ends_with(".esl"))) throw std::runtime_error("An item has an invalid plugin filename.");
                outfit.items.push_back(std::move(item));
            }
            const auto identity = OutfitIdentity(outfit);
            if (identities.contains(identity)) { ++duplicates; continue; }
            for (const auto& item : outfit.items) {
                auto* armor = item.Resolve();
                if (!armor || armor->GetLocalFormID() != item.formId) throw std::runtime_error("Missing or invalid armor: " + item.name + " (" + item.plugin + ", local ID " + std::to_string(item.formId) + ")");
            }
            if (nextId <= 0 || nextId == (std::numeric_limits<int>::max)()) throw std::runtime_error("No outfit IDs are available.");
            outfit.id = nextId++;
            for (auto& category : categories) if (targetCategories.contains(category.id)) category.outfitIds.push_back(outfit.id);
            outfits.push_back(std::move(outfit));
            identities.insert(identity);
            ++added;
        } catch (const std::exception& error) {
            skipped.push_back({{"name", name}, {"reason", error.what()}});
        }
    }
    if (added) {
        CommitFiles(OutfitStore::Serialize(outfits, nextId), OutfitLibrary::Serialize(categories, library._nextId));
        store._outfits.swap(outfits);
        store._nextId = nextId;
        library._categories.swap(categories);
    }
    return {{"ok", true}, {"operation", "import"}, {"file", filename}, {"added", added}, {"duplicates", duplicates}, {"skipped", skipped}};
}
