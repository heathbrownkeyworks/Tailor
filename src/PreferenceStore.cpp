#include "PreferenceStore.h"

#include <Windows.h>
#include <fstream>
#include <sstream>

PreferenceStore& PreferenceStore::GetSingleton()
{
    static PreferenceStore singleton;
    return singleton;
}

std::filesystem::path PreferenceStore::Path()
{
    return std::filesystem::path("Data/SKSE/Plugins/Tailor") / "settings.json";
}

void PreferenceStore::Load()
{
    std::scoped_lock lock(_mutex);
    _preferences = {};
    const auto path = Path();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        // Tailor.ini's retired AutoFavorite=0 kept the power out of Favorites: Disable Tailor Favorite carries it on.
        if (GetPrivateProfileIntA("Power", "AutoFavorite", 1, "Data/SKSE/Plugins/Tailor.ini") == 0) {
            _preferences.disableFavorite = true;
            logger::info("Preferences: Tailor.ini has AutoFavorite=0, so Disable Tailor Favorite starts on");
            Write(_preferences);
            return;
        }
        logger::info("Preferences: no settings.json yet; every setting is off");
        return;
    }
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    std::vector<std::string> ignored;
    _preferences = Tailor::ParsePreferences(text.str(), ignored);
    for (const auto& name : ignored) logger::warn("Preferences: '{}' could not be read; it is off", name);
    logger::info("Preferences: disableFavorite={}, hideWeapons={}, hideHelmets={}",
        _preferences.disableFavorite, _preferences.hideWeapons, _preferences.hideHelmets);
}

Tailor::Preferences PreferenceStore::Get() const
{
    std::scoped_lock lock(_mutex);
    return _preferences;
}

bool PreferenceStore::Set(std::string_view name, bool on)
{
    std::scoped_lock lock(_mutex);
    auto preferences = _preferences;
    auto* value = Tailor::FindPreference(preferences, name);
    if (!value) {
        logger::warn("Preferences: unknown setting '{}'", name);
        return false;
    }
    *value = on;
    // A switch that could not be saved stays as it was, as the screen still shows it.
    if (!Write(preferences)) return false;
    _preferences = preferences;
    logger::info("Preferences: {} is now {}", name, on ? "on" : "off");
    return true;
}

bool PreferenceStore::Write(const Tailor::Preferences& preferences)
{
    const auto path = Path();
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(temporary, std::ios::trunc);
        file << Tailor::SerializePreferences(preferences);
        file.close();
        if (!file) {
            logger::error("Preferences: failed to write {}", temporary.string());
            return false;
        }
        try {
            std::filesystem::rename(temporary, path);
        } catch (const std::filesystem::filesystem_error&) {
            // Where renaming over the old file is refused: copy over it, then remove the temporary one.
            std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(temporary);
        }
    } catch (const std::exception& e) {
        logger::error("Preferences: failed to save settings.json: {}", e.what());
        return false;
    }
    return true;
}
