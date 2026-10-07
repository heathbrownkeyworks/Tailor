#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Tailor
{
    // The Settings screen's switches, kept in settings.json for every save. Each starts off.
    struct Preferences
    {
        bool disableFavorite = false;  // keep the Tailor power out of Favorites
        bool hideWeapons = false;      // weapons, shields and arrows outside Adventuring and combat
        bool hideHelmets = false;      // helmets and hoods outside Adventuring and combat
        bool operator==(const Preferences&) const = default;
    };

    // The switch a Settings row names, or nullptr for any other name.
    [[nodiscard]] inline bool* FindPreference(Preferences& preferences, std::string_view name) noexcept
    {
        if (name == "disableFavorite") return &preferences.disableFavorite;
        if (name == "hideWeapons") return &preferences.hideWeapons;
        if (name == "hideHelmets") return &preferences.hideHelmets;
        return nullptr;
    }

    // settings.json's text. A missing switch stays off. One that isn't true or false stays off and is
    // named in `ignored`, and so is a file that isn't a JSON object ("settings.json").
    [[nodiscard]] inline Preferences ParsePreferences(std::string_view text, std::vector<std::string>& ignored)
    {
        Preferences preferences;
        const auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            ignored.emplace_back("settings.json");
            return preferences;
        }
        for (const char* name : {"disableFavorite", "hideWeapons", "hideHelmets"}) {
            const auto it = json.find(name);
            if (it == json.end()) continue;
            if (it->is_boolean()) *FindPreference(preferences, name) = it->get<bool>();
            else ignored.emplace_back(name);
        }
        return preferences;
    }

    [[nodiscard]] inline std::string SerializePreferences(const Preferences& preferences)
    {
        return nlohmann::json{{"disableFavorite", preferences.disableFavorite}, {"hideWeapons", preferences.hideWeapons},
            {"hideHelmets", preferences.hideHelmets}}.dump(2);
    }
}
