#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>

// Who an outfit is made for. The numbers are the engine's own sex values
// (RE::SEX kMale 0, kFemale 1; kNone -1), so an NPC compares directly.
enum class OutfitSex : int { Unisex = -1, Male = 0, Female = 1 };

// A Unisex outfit fits every NPC. An NPC whose sex is unknown (-1) is not
// filtered, which is how every outfit behaved before outfits carried a sex.
constexpr bool OutfitFits(OutfitSex outfit, int npcSex)
{
    return outfit == OutfitSex::Unisex || npcSex < 0 || static_cast<int>(outfit) == npcSex;
}

// Only the integers -1, 0 and 1 name a sex. Anything else is unreadable.
inline std::optional<OutfitSex> ReadOutfitSex(const nlohmann::json& value)
{
    if (!value.is_number_integer()) return std::nullopt;
    const auto number = value.get<std::int64_t>();
    if (number < -1 || number > 1) return std::nullopt;
    return static_cast<OutfitSex>(number);
}

constexpr const char* OutfitSexName(OutfitSex sex)
{
    return sex == OutfitSex::Female ? "Female" : sex == OutfitSex::Male ? "Male" : "Unisex";
}
