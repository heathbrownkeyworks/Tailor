#pragma once
#include "MeridianUIAPI/InputAPI.h"
#include <array>
#include <optional>
#include <string_view>

namespace Tailor::Controller
{
    using Control = Meridian::UI::Input::Control;
    inline std::optional<Control> ParseControl(std::string_view name)
    {
        constexpr std::array names{"None", "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
            "Start", "Back", "LeftThumb", "RightThumb", "LeftShoulder", "RightShoulder",
            "South", "East", "West", "North"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::string_view candidate = names[i];
            if (candidate.size() != name.size()) continue;
            bool equal = true;
            for (std::size_t j = 0; j < name.size(); ++j) {
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
                equal &= lower(name[j]) == lower(candidate[j]);
            }
            if (equal) return static_cast<Control>(i);
        }
        return std::nullopt;
    }

    constexpr bool AllowedOpeningChord(Control button, Control modifier)
    {
        if (button < Control::DpadUp || button > Control::North || modifier > Control::North || button == modifier) return false;
        const auto pair = [&](Control a, Control b) {
            return (button == a && modifier == b) || (button == b && modifier == a);
        };
        return !pair(Control::LeftShoulder, Control::North) && // Horde: LB + Y
            !pair(Control::RightShoulder, Control::Back);    // Romantasy: RB + View
    }
}
