#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Tailor::Outfits
{
    // Outfit and category names are unique ignoring capitals and surrounding spaces.
    [[nodiscard]] inline std::string TrimName(std::string_view name)
    {
        const auto first = name.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        const auto last = name.find_last_not_of(" \t\r\n");
        return std::string(name.substr(first, last - first + 1));
    }

    // A letter's small form, for comparing names ignoring capitals: ASCII, Latin-1, Latin Extended-A, Greek and
    // Cyrillic. A fixed table rather than Windows' language, so two names compare the same on every PC.
    [[nodiscard]] constexpr char32_t FoldCase(char32_t c) noexcept
    {
        const auto plus = [c](unsigned offset) { return static_cast<char32_t>(c + offset); };
        if (c >= U'A' && c <= U'Z') return plus(0x20);
        if (c < 0xC0) return c;
        if (c <= 0xDE) return c == 0xD7 ? c : plus(0x20);                             // À-Þ, not ×
        if (c >= 0x100 && c <= 0x17F) {                                                // Latin Extended-A pairs
            if (c == 0x130) return U'i';                                               // İ
            if (c == 0x178) return 0xFF;                                               // Ÿ
            if (c == 0x138 || c == 0x149 || c == 0x17F) return c;                      // ĸ, ŉ, ſ have no pair
            const bool oddCapitals = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
            return c % 2 == (oddCapitals ? 1u : 0u) ? plus(1) : c;
        }
        if (c == 0x386) return 0x3AC;                                                  // Greek with tonos
        if (c >= 0x388 && c <= 0x38A) return plus(0x25);
        if (c == 0x38C) return 0x3CC;
        if (c == 0x38E || c == 0x38F) return plus(0x3F);
        if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return plus(0x20);                 // Α-Ϋ
        if (c == 0x3C2) return 0x3C3;                                                  // final sigma
        if (c >= 0x400 && c <= 0x40F) return plus(0x50);                               // Ѐ-Џ
        if (c >= 0x410 && c <= 0x42F) return plus(0x20);                               // А-Я
        if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
            return c % 2 == 0 ? plus(1) : c;                                           // Cyrillic pairs
        if (c == 0x4C0) return 0x4CF;
        if (c >= 0x4C1 && c <= 0x4CE) return c % 2 == 1 ? plus(1) : c;
        return c;
    }

    // The name with every letter in its small form (FoldCase). Bytes that aren't valid UTF-8 stay as they are, so a
    // raw plugin name still compares by its bytes.
    [[nodiscard]] inline std::string FoldedName(std::string_view name)
    {
        std::string folded;
        folded.reserve(name.size());
        for (std::size_t i = 0; i < name.size();) {
            const auto lead = static_cast<unsigned char>(name[i]);
            const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
            char32_t c = length == 1 ? lead : length == 2 ? lead & 0x1Fu : length == 3 ? lead & 0x0Fu : lead & 0x07u;
            bool valid = length != 0 && i + length <= name.size();
            for (std::size_t k = 1; valid && k < length; ++k) {
                const auto next = static_cast<unsigned char>(name[i + k]);
                valid = (next & 0xC0) == 0x80;
                c = (c << 6) | (next & 0x3Fu);
            }
            if (!valid) {
                folded.push_back(name[i]);
                ++i;
                continue;
            }
            const char32_t lower = FoldCase(c);
            // Every letter FoldCase changes has its small form below U+0800: one or two bytes.
            if (lower == c) folded.append(name.substr(i, length));
            else if (lower < 0x80) folded.push_back(static_cast<char>(lower));
            else {
                folded.push_back(static_cast<char>(0xC0 | (lower >> 6)));
                folded.push_back(static_cast<char>(0x80 | (lower & 0x3F)));
            }
            i += length;
        }
        return folded;
    }

    [[nodiscard]] inline std::string NameKey(std::string_view name)
    {
        return FoldedName(TrimName(name));
    }

    [[nodiscard]] inline bool SameName(std::string_view a, std::string_view b)
    {
        return NameKey(a) == NameKey(b);
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
