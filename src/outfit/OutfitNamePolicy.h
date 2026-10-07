#pragma once

#include <string>
#include <string_view>

namespace Tailor::Outfits
{
    // Outfit and category names are unique ignoring capitals (ASCII) and surrounding spaces.
    [[nodiscard]] inline std::string TrimName(std::string_view name)
    {
        const auto first = name.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        const auto last = name.find_last_not_of(" \t\r\n");
        return std::string(name.substr(first, last - first + 1));
    }

    [[nodiscard]] inline std::string NameKey(std::string_view name)
    {
        auto key = TrimName(name);
        for (auto& c : key) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return key;
    }

    [[nodiscard]] inline bool SameName(std::string_view a, std::string_view b)
    {
        return NameKey(a) == NameKey(b);
    }

    // List order: names ignoring capitals; the same name in other capitals by its bytes, so the order is fixed.
    [[nodiscard]] inline bool NameBefore(std::string_view a, std::string_view b)
    {
        const auto keyA = NameKey(a), keyB = NameKey(b);
        return keyA != keyB ? keyA < keyB : a < b;
    }

    // The first free "base (2)", "base (3)", ... for a duplicate; `taken(key)` says whether a name key is in use.
    template<class Taken> [[nodiscard]] std::string NumberedName(const std::string& base, Taken taken)
    {
        for (int n = 2;; ++n) {
            auto name = base + " (" + std::to_string(n) + ")";
            if (!taken(NameKey(name))) return name;
        }
    }
}
