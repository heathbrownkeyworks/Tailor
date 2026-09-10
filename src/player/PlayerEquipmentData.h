#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

namespace Tailor::Player
{
    inline constexpr std::uint32_t ReferenceID = 0x14;
    enum class Channel : std::size_t { Outfit, Wig };

    // Identify an individual inventory copy, never just its base armor form.
    struct Instance
    {
        std::uint32_t form = 0;
        std::uint32_t owner = 0;
        std::uint16_t unique = 0;
        bool left = false;
        bool operator==(const Instance&) const = default;
    };
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Instance, form, owner, unique, left)

    struct EquipmentSnapshot
    {
        std::vector<Instance> worn;
        std::array<std::uint32_t, 2> hands{}; // left, right; includes spells
    };
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EquipmentSnapshot, worn, hands)

    struct EquipmentState
    {
        bool hasBaseline = false;
        EquipmentSnapshot baseline;
        std::vector<Instance> supplied;
        std::uint32_t slots = 0;
    };
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EquipmentState, hasBaseline, baseline, supplied, slots)

    inline bool SameCopy(const Instance& a, const Instance& b)
    {
        return a.form == b.form && a.owner == b.owner && a.unique == b.unique;
    }

    inline bool ContainsCopy(const std::vector<Instance>& items, const Instance& item)
    {
        for (const auto& current : items) if (SameCopy(current, item)) return true;
        return false;
    }
}
