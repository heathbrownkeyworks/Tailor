#pragma once

#include "outfit/OutfitNamePolicy.h"
#include "outfit/OutfitSex.h"
#include "outfit/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Tailor::Api
{
    // The API's gender filter: -1 all outfits; 0 and 1 the outfits a man or a woman can wear, unisex
    // included (the fit rule every NPC goes by). Any other value matches nothing.
    [[nodiscard]] constexpr bool FitsGenderFilter(OutfitSex sex, int filter) noexcept
    {
        if (filter == -1) return true;
        if (filter != 0 && filter != 1) return false;
        return OutfitFits(sex, filter);
    }

    // An outfit or category name from a script or another plugin, as the API matches and stores it: bytes that
    // aren't valid UTF-8 become '?' (Tailor's rule for every name it reads from the game, since its JSON files and
    // screens can't write them), and surrounding spaces go. A later call with the same bytes finds what an earlier
    // one made.
    [[nodiscard]] inline std::string ApiName(std::string_view name)
    {
        return Outfits::TrimName(SanitizeUtf8(std::string(name).c_str()));
    }

    // Body slots 30-61 are the ARMO biped mask's bits 0-31; any other number is no slot.
    [[nodiscard]] constexpr std::uint32_t SlotBit(int slot) noexcept
    {
        return slot >= 30 && slot <= 61 ? 1u << (slot - 30) : 0u;
    }

    inline constexpr const char* kSituationNames[] = {"", "adventuring", "town", "home", "sleep", "swimming", "warm"};

    // "adventuring".."warm", any capitals and surrounding spaces, as OutfitSituation's 1..6.
    [[nodiscard]] inline std::optional<int> SituationFromName(std::string_view name)
    {
        std::string key;
        for (const char c : name) {
            if (c == ' ' || c == '\t') continue;
            key += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        for (int i = 1; i <= 6; ++i) {
            if (key == kSituationNames[i]) return i;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline const char* SituationName(int situation) noexcept
    {
        return situation >= 1 && situation <= 6 ? kSituationNames[situation] : "";
    }

    // HasSituation: someone has their own outfit for a situation when Tailor would put it on them. Set to Random,
    // that needs an outfit they can wear in the situation's pool; otherwise the fixed outfit must be one they can
    // wear (and, for Adventuring, one their armor type allows).
    [[nodiscard]] constexpr bool HasOwnChoice(bool random, bool fixedWearable, bool poolHasWearable) noexcept
    {
        return random ? poolHasWearable : fixedWearable;
    }

    // An override as the co-save keeps it: the person (a FormID as the game had it at saving, resolved
    // at load), the Tailor outfit id, and the situation it was set for (OverrideWithSituation; 0 when it was set for
    // an outfit or a category).
    struct OverrideEntry
    {
        std::uint32_t actor = 0;
        int outfit = 0;
        int situation = 0;
    };

    // Set for Swimming or Sleep, an override is that look: it hides weapons, shields, quivers and torches as
    // Tailor's own Swimming and Sleep looks do. Set for any other situation, an outfit or a category, it hides none.
    [[nodiscard]] constexpr bool OverrideHidesWeapons(int situation) noexcept
    {
        return situation == 4 || situation == 5;
    }
    static_assert(std::string_view(kSituationNames[4]) == "sleep" && std::string_view(kSituationNames[5]) == "swimming");

    // The look a mod's override gives someone: only while they wear it (their override's outfit is the one on them);
    // then it decides alone, the Swimming or Sleep look when set for one and none otherwise. Nothing when they don't
    // wear it (not dressed in it yet, or in their Adventuring outfit for a fight), so Tailor's own looks decide.
    [[nodiscard]] constexpr std::optional<bool> OverrideWeaponsLook(std::optional<int> overrideOutfit, int situation, int wornOutfit) noexcept
    {
        if (!overrideOutfit || *overrideOutfit != wornOutfit) return std::nullopt;
        return OverrideHidesWeapons(situation);
    }

    [[nodiscard]] inline std::string EncodeOverrides(const std::vector<OverrideEntry>& entries)
    {
        auto json = nlohmann::json::array();
        for (const auto& entry : entries) {
            json.push_back({{"actor", entry.actor}, {"outfit", entry.outfit}, {"situation", entry.situation}});
        }
        return json.dump();
    }

    // An entry that can't be read, or whose outfit id isn't an outfit (below 1), is skipped and counted;
    // so is a payload that isn't a JSON array. A situation that is missing (a record from before overrides kept one)
    // or isn't a situation leaves the override with none.
    [[nodiscard]] inline std::vector<OverrideEntry> DecodeOverrides(std::string_view payload, std::size_t& skipped)
    {
        std::vector<OverrideEntry> entries;
        const auto json = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (json.is_discarded() || !json.is_array()) {
            ++skipped;
            return entries;
        }
        for (const auto& item : json) {
            try {
                OverrideEntry entry{item.at("actor").get<std::uint32_t>(), item.at("outfit").get<int>()};
                if (entry.outfit < 1) { ++skipped; continue; }
                if (const auto situation = item.find("situation"); situation != item.end() && situation->is_number_integer()) {
                    entry.situation = situation->get<int>();
                    if (*SituationName(entry.situation) == '\0') entry.situation = 0;
                }
                entries.push_back(entry);
            } catch (const std::exception&) {
                ++skipped;
            }
        }
        return entries;
    }

    // Change events: what was last seen of one person. The first sighting records without sending, so a
    // load or a new game doesn't announce everyone.
    struct Seen
    {
        std::optional<int> outfit;
        std::optional<int> situation;
    };
    struct Change
    {
        bool outfit = false;
        bool situation = false;
    };
    // An outfit not known yet (nullopt: Tailor is still putting it back on) keeps the last known one and reports
    // no outfit change, so the first one known after a load is a first sighting, and someone put back in the
    // same outfit reports nothing. The situation is compared either way.
    [[nodiscard]] inline Change Observe(Seen& seen, std::optional<int> outfit, int situation)
    {
        const Change change{outfit && seen.outfit && *seen.outfit != *outfit, seen.situation && *seen.situation != situation};
        if (outfit) seen.outfit = outfit;
        seen.situation = situation;
        return change;
    }

    // Tells each listener registered when telling began. A listener may add or remove listeners while it is
    // told: one removed then is not told again, and one added then hears the next change.
    template <class Listener, class Tell>
    void TellListeners(const std::vector<Listener*>& listeners, Tell tell)
    {
        const auto registered = listeners;
        for (auto* listener : registered) {
            if (std::find(listeners.begin(), listeners.end(), listener) != listeners.end()) tell(listener);
        }
    }
}
