#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Tailor::Situations
{
    // Hide Weapons and Hide Helmets act outside Adventuring and combat. `place` is the person's
    // situation with Warm replaced by the location beneath it, so Warm in a town hides and Warm in
    // the wilderness shows.
    template<class Situation> [[nodiscard]] constexpr bool HiddenOutsideAdventuring(Situation place, bool inCombat) noexcept
    {
        return place != Situation::Adventuring && !inCombat;
    }

    // Armor slot bits (the ARMO biped mask): 30 Head, 31 Hair, 32 Body.
    inline constexpr std::uint32_t kHeadSlot = 1u << 0, kHairSlot = 1u << 1, kBodySlot = 1u << 2;

    // What Hide Helmets takes off: a piece covering the head or the hair that is not body armor, so
    // helmets and separate hoods. Circlets (slot 42) and robes with a hood stay, and so does a wig
    // (`wig`: in Tailor's wig library, or the person's own Tailor wig).
    [[nodiscard]] constexpr bool HelmetToTakeOff(std::uint32_t slotMask, bool wig) noexcept
    {
        return !wig && (slotMask & (kHeadSlot | kHairSlot)) != 0 && (slotMask & kBodySlot) == 0;
    }

    // Where one copy Hide Helmets took off is now: gone from the inventory, there but off, or worn again.
    enum class CopyState { Gone, Off, Worn };
    // What putting back does with it. A copy goes back only while the person still wears the look it
    // came off (`sameLook`) and it is there and off; a copy that is gone, or from another look, is
    // forgotten (never manufactured, never worn over a newer outfit); one worn again needs nothing.
    enum class CopyFate { PutBack, Forget };
    [[nodiscard]] constexpr CopyFate DecideCopy(bool sameLook, CopyState state) noexcept
    {
        return sameLook && state == CopyState::Off ? CopyFate::PutBack : CopyFate::Forget;
    }

    // A helmet Tailor took off, as the co-save keeps it: the person, the armor, the copy's unique ID
    // (the owner and number in its ExtraUniqueID), and the look it came off (the outfit the person wore;
    // 0 for none, or the player's own gear). Form IDs are as the game had them at saving; loading
    // resolves them for the current load order.
    struct TakenHelmet
    {
        std::uint32_t actor = 0;
        std::uint32_t form = 0;
        std::uint32_t owner = 0;
        std::uint16_t unique = 0;
        std::int32_t look = 0;
        bool operator==(const TakenHelmet&) const = default;
    };

    [[nodiscard]] inline std::string EncodeTakenHelmets(const std::vector<TakenHelmet>& helmets)
    {
        auto json = nlohmann::json::array();
        for (const auto& helmet : helmets) {
            json.push_back({{"actor", helmet.actor}, {"form", helmet.form}, {"owner", helmet.owner}, {"unique", helmet.unique},
                {"look", helmet.look}});
        }
        return json.dump();
    }

    // The co-save payload. An entry that can't be read, or a payload that isn't a JSON array, is
    // skipped and counted in `skipped`. A missing look is 0.
    [[nodiscard]] inline std::vector<TakenHelmet> DecodeTakenHelmets(std::string_view payload, std::size_t& skipped)
    {
        std::vector<TakenHelmet> helmets;
        const auto json = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (json.is_discarded() || !json.is_array()) {
            ++skipped;
            return helmets;
        }
        for (const auto& entry : json) {
            try {
                const auto unique = entry.at("unique").get<std::uint32_t>();
                if (unique > 0xFFFF) throw std::out_of_range("unique");
                const auto look = entry.value("look", std::int64_t{0});
                if (look < INT32_MIN || look > INT32_MAX) throw std::out_of_range("look");
                helmets.push_back({entry.at("actor").get<std::uint32_t>(), entry.at("form").get<std::uint32_t>(),
                    entry.at("owner").get<std::uint32_t>(), static_cast<std::uint16_t>(unique), static_cast<std::int32_t>(look)});
            } catch (const std::exception&) {
                ++skipped;
            }
        }
        return helmets;
    }
}
