#pragma once

#include "Preferences.h"

#include <filesystem>
#include <mutex>
#include <string_view>

// The Settings screen's switches for every save, in Data/SKSE/Plugins/Tailor/settings.json. Tailor
// writes the file only when a switch changes (and once before it exists, when Tailor.ini's retired
// AutoFavorite=0 turns Disable Tailor Favorite on), so a file it could not read is never overwritten
// on its own.
class PreferenceStore
{
public:
    static PreferenceStore& GetSingleton();

    void Load();
    [[nodiscard]] Tailor::Preferences Get() const;
    // Sets the switch `name` and saves the file; false, the switch unchanged, for a name the screen
    // never sends or a file that could not be saved.
    bool Set(std::string_view name, bool on);

private:
    PreferenceStore() = default;
    [[nodiscard]] static std::filesystem::path Path();
    // Writes settings.json through settings.json.tmp, so a failed write never leaves half a file. False,
    // and logged, on any failure.
    static bool Write(const Tailor::Preferences& preferences);

    Tailor::Preferences _preferences;
    mutable std::mutex _mutex;
};
