#pragma once

#include "outfit/ArmorItem.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

enum class WigCategory : uint8_t
{
    FemaleLong,
    FemaleUpdo,
    FemalePonytail,
    FemaleShort,
    MaleLong,
    MaleUpdo,
    MalePonytail,
    MaleShort,
    COUNT
};

inline constexpr size_t kCategoryCount = static_cast<size_t>(WigCategory::COUNT);

inline constexpr std::array<std::string_view, kCategoryCount> kCategoryNames = {
    "FemaleLong",
    "FemaleUpdo",
    "FemalePonytail",
    "FemaleShort",
    "MaleLong",
    "MaleUpdo",
    "MalePonytail",
    "MaleShort"
};

inline constexpr std::array<std::string_view, kCategoryCount> kCategoryDisplayNames = {
    "Female - Long",
    "Female - Updo",
    "Female - Ponytail",
    "Female - Short",
    "Male - Long",
    "Male - Updo",
    "Male - Ponytail",
    "Male - Short"
};

inline constexpr std::string_view CategoryToString(WigCategory cat)
{
    auto idx = static_cast<size_t>(cat);
    return idx < kCategoryCount ? kCategoryNames[idx] : "Unknown";
}

struct WigEntry
{
    RE::FormID  formId = 0;
    std::string plugin;
    std::string name;

    bool operator==(const WigEntry& other) const
    {
        return formId == other.formId && plugin == other.plugin;
    }

    RE::TESObjectARMO* Resolve() const
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;
        auto* form = dataHandler->LookupForm(formId, plugin);
        return form ? form->As<RE::TESObjectARMO>() : nullptr;
    }
};
