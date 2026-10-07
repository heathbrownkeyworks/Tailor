#pragma once
#include "../Theme.h"

#include <optional>

namespace Tailor::ImGuiUI
{
    // The Settings screen's switches as the bridge last published them.
    struct SettingsSwitches
    {
        bool disableFavorite = false;
        bool hideWeapons = false;
        bool hideHelmets = false;
    };

    // A switch the user flipped: its name as the bridge knows it, and its new value.
    struct SettingsChange
    {
        const char* name = nullptr;
        bool on = false;
    };

    // The preferences page. Returns Back; a flipped switch lands in `change`.
    bool DrawSettingsScreen(const Fonts& fonts, float scale, const SettingsSwitches& switches, std::optional<SettingsChange>& change);
}
