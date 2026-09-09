#pragma once

#include <string_view>

enum class OutfitArmorType { Any, Heavy, Light, Clothing };

inline OutfitArmorType ParseOutfitArmorType(std::string_view value)
{
    if (value == "heavy") return OutfitArmorType::Heavy;
    if (value == "light") return OutfitArmorType::Light;
    if (value == "clothing") return OutfitArmorType::Clothing;
    return OutfitArmorType::Any;
}

inline const char* OutfitArmorTypeName(OutfitArmorType type)
{
    switch (type) {
    case OutfitArmorType::Heavy: return "heavy";
    case OutfitArmorType::Light: return "light";
    case OutfitArmorType::Clothing: return "clothing";
    default: return "any";
    }
}
