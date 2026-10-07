#include "wig/CustomColorLibrary.h"

#include <fstream>
#include "persistence/JsonFile.h"

CustomColorLibrary& CustomColorLibrary::GetSingleton()
{
    static CustomColorLibrary singleton;
    return singleton;
}

std::filesystem::path CustomColorLibrary::GetPath() const
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Wiggy");
    std::filesystem::create_directories(path);
    return path / "customcolors.json";
}

void CustomColorLibrary::Load()
{
    std::lock_guard lock(_mutex);
    // Saves stay off until this load has read the whole file, as for the wig files. Adding or
    // deleting a color still works for the session.
    _saveAllowed = false;
    try {
        const auto path = GetPath();
        logger::info("CustomColorLibrary: loading from {}", path.string());

        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            // A file Tailor can't check is not a missing file: saves stay off.
            if (error) throw std::filesystem::filesystem_error("could not check customcolors.json", path, error);
            _colors.clear();
            _saveAllowed = true;
            logger::info("CustomColorLibrary: no customcolors.json found, starting empty");
            return;
        }

        std::ifstream file(path);
        const auto json = nlohmann::json::parse(file);
        if (!json.is_object() || (json.contains("version") && json["version"] != 1) ||
            !json.contains("colors") || !json["colors"].is_array()) {
            throw std::runtime_error("unsupported custom color document");
        }

        std::vector<RGBColor> parsed;
        bool complete = true;
        std::size_t row = 0;
        for (const auto& entry : json["colors"]) {
            ++row;
            try {
                if (!entry.is_object()) throw std::runtime_error("the row is not a color");
                // A channel that is missing or isn't a whole number makes the row a bad one.
                const auto channel = [&entry](const char* name) {
                    const auto& value = entry.at(name);
                    if (!value.is_number_integer()) throw std::runtime_error(std::format("\"{}\" is not a whole number", name));
                    return value.get<std::int64_t>();
                };
                const auto r = channel("r"), g = channel("g"), b = channel("b");
                // Out of range is skipped without turning saves off, as it always was.
                if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                    logger::warn("CustomColorLibrary: skipping out-of-range color ({},{},{})", r, g, b);
                    continue;
                }
                parsed.push_back({static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)});
            } catch (const std::exception& e) {
                complete = false;
                logger::error("CustomColorLibrary: color row {} could not load; the other colors stay, customcolors.json is kept as it is and saves are off: {}", row, e.what());
            }
        }
        _colors = std::move(parsed);
        _saveAllowed = complete;
        logger::info("CustomColorLibrary: loaded {} custom color(s)", _colors.size());
    } catch (const std::exception& e) {
        logger::error("CustomColorLibrary: could not load customcolors.json; the file is kept as it is and saves are off: {}", e.what());
    }
}

void CustomColorLibrary::Save() const
{
    std::lock_guard lock(_mutex);
    if (!_saveAllowed) {
        logger::error("CustomColorLibrary: save skipped because customcolors.json was not loaded completely");
        return;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["colors"] = nlohmann::json::array();

    for (auto& c : _colors) {
        nlohmann::json entry;
        entry["r"] = static_cast<int>(c.r);
        entry["g"] = static_cast<int>(c.g);
        entry["b"] = static_cast<int>(c.b);
        json["colors"].push_back(entry);
    }

    try {
        auto path = GetPath();
        const auto contents = json.dump(2);
        std::string error;
        if (!Tailor::Persistence::WriteJsonFile(path, contents, error)) {
            logger::error("CustomColorLibrary: failed to save {}: {}", path.string(), error);
            return;
        }
        logger::info("CustomColorLibrary: saved {} custom color(s) to {}", _colors.size(), path.string());
    }
    catch (const std::exception& e) {
        logger::error("CustomColorLibrary: failed to save: {}", e.what());
    }
}

std::vector<CustomColorLibrary::RGBColor> CustomColorLibrary::GetColors() const
{
    std::lock_guard lock(_mutex);
    return _colors;  // copy
}

bool CustomColorLibrary::AddColor(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::lock_guard lock(_mutex);
    for (auto& c : _colors) {
        if (c.r == r && c.g == g && c.b == b) {
            return false;  // duplicate
        }
    }
    _colors.push_back({r, g, b});
    return true;
}

bool CustomColorLibrary::RemoveColor(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::lock_guard lock(_mutex);
    for (auto it = _colors.begin(); it != _colors.end(); ++it) {
        if (it->r == r && it->g == g && it->b == b) {
            _colors.erase(it);
            return true;
        }
    }
    return false;
}
