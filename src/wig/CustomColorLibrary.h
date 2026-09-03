#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

// Persistent store of user-defined hair colors. Serialized to
// Data/SKSE/Plugins/Wiggy/customcolors.json. Unlike WigAssignments (which is
// per-actor), this is a global library shared across all NPCs.
class CustomColorLibrary
{
public:
    struct RGBColor {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
    };

    static CustomColorLibrary& GetSingleton();

    void Load();
    void Save() const;

    std::vector<RGBColor> GetColors() const;

    // Returns true if the color was added, false if it already existed.
    bool AddColor(std::uint8_t r, std::uint8_t g, std::uint8_t b);

    // Returns true if the color was found and removed, false otherwise.
    bool RemoveColor(std::uint8_t r, std::uint8_t g, std::uint8_t b);

private:
    CustomColorLibrary() = default;
    ~CustomColorLibrary() = default;

    CustomColorLibrary(const CustomColorLibrary&) = delete;
    CustomColorLibrary& operator=(const CustomColorLibrary&) = delete;

    std::filesystem::path GetPath() const;

    std::vector<RGBColor> _colors;
    mutable std::mutex    _mutex;
};
