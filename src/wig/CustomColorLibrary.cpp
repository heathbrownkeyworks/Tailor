#include "wig/CustomColorLibrary.h"

#include <fstream>

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
    _colors.clear();

    auto path = GetPath();
    logger::info("CustomColorLibrary: loading from {}", path.string());

    if (!std::filesystem::exists(path)) {
        logger::info("CustomColorLibrary: no customcolors.json found, starting empty");
        return;
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            logger::error("CustomColorLibrary: failed to open {} for reading", path.string());
            return;
        }

        nlohmann::json json;
        file >> json;

        if (!json.contains("colors") || !json["colors"].is_array()) {
            logger::warn("CustomColorLibrary: no 'colors' array in {}", path.string());
            return;
        }

        for (auto& entry : json["colors"]) {
            if (!entry.is_object()) continue;
            int r = entry.value("r", -1);
            int g = entry.value("g", -1);
            int b = entry.value("b", -1);
            if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                logger::warn("CustomColorLibrary: skipping out-of-range color ({},{},{})", r, g, b);
                continue;
            }
            _colors.push_back({
                static_cast<std::uint8_t>(r),
                static_cast<std::uint8_t>(g),
                static_cast<std::uint8_t>(b)
            });
        }

        logger::info("CustomColorLibrary: loaded {} custom color(s)", _colors.size());
    }
    catch (const std::exception& e) {
        logger::error("CustomColorLibrary: failed to parse {}: {}", path.string(), e.what());
    }
}

void CustomColorLibrary::Save() const
{
    std::lock_guard lock(_mutex);

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
        std::ofstream file(path);
        if (!file.is_open()) {
            logger::error("CustomColorLibrary: failed to open {} for writing", path.string());
            return;
        }
        file << json.dump(2);
        file.flush();
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
