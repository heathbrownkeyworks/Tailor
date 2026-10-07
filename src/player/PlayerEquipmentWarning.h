#pragma once

#include <format>
#include <string>
#include <vector>

namespace Tailor::Player
{
    // "A", "A and B", "A, B and C" or "A, B and 3 more": short enough for a toast.
    inline std::string NameList(const std::vector<std::string>& names)
    {
        if (names.empty()) return {};
        if (names.size() == 1) return names[0];
        if (names.size() == 2) return names[0] + " and " + names[1];
        if (names.size() == 3) return names[0] + ", " + names[1] + " and " + names[2];
        return std::format("{}, {} and {} more", names[0], names[1], names.size() - 2);
    }

    // The toast after dressing the player: the pieces that did not go on or show, and the pieces
    // that would not come off. Empty when both lists are.
    inline std::string EquipmentWarning(const std::vector<std::string>& notShown, const std::vector<std::string>& stayedOn)
    {
        std::string text;
        if (!notShown.empty()) text += NameList(notShown) + " could not be equipped or displayed. ";
        if (!stayedOn.empty()) text += NameList(stayedOn) + " would not come off. ";
        if (!text.empty()) text += "Check for conflicting items.";
        return text;
    }
}
