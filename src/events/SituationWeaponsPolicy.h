#pragma once

#include "preview/PreviewSessionPolicy.h"

#include <cstddef>
#include <string_view>

namespace Tailor::Situations
{
    // What the Sleep and Swimming looks hide: what the player's preview hides (weapons drawn or
    // sheathed, the ammunition in the quiver, a held torch), and the shield. Other armor stays.
    template<class FormType> [[nodiscard]] constexpr bool HiddenInSleepOrSwim(FormType type, bool shield) noexcept
    {
        return Tailor::Preview::HiddenInPlayerPreview(type) || (type == FormType::Armor && shield);
    }

    // Nodes hidden by name: the scabbards and Immersive Equipment Displays' weapon, quiver and torch
    // models the player's preview hides, and IED's shield displays ("OBJECT SHIELD [...]"). IED's
    // armor and misc displays stay. Names match regardless of case, as the game's lookups do.
    [[nodiscard]] constexpr bool HiddenNodeInSleepOrSwim(std::string_view name) noexcept
    {
        if (Tailor::Preview::HiddenNodeInPlayerPreview(name)) return true;
        constexpr std::string_view shield = "object shield [";
        if (name.size() < shield.size()) return false;
        for (std::size_t i = 0; i < shield.size(); ++i) {
            const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
            if (c != shield[i]) return false;
        }
        return true;
    }

    // Hide Weapons: the Sleep and Swimming set without the torch, which people carry for light.
    template<class FormType> [[nodiscard]] constexpr bool HiddenByHideWeapons(FormType type, bool shield) noexcept
    {
        return type != FormType::Light && HiddenInSleepOrSwim(type, shield);
    }

    // The nodes Hide Weapons hides: the Sleep and Swimming nodes but Immersive Equipment Displays'
    // torch ("OBJECT LIGHT [...]").
    [[nodiscard]] constexpr bool HiddenNodeByHideWeapons(std::string_view name) noexcept
    {
        constexpr std::string_view light = "object light [";
        bool torch = name.size() >= light.size();
        for (std::size_t i = 0; torch && i < light.size(); ++i) {
            const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
            torch = c == light[i];
        }
        return !torch && HiddenNodeInSleepOrSwim(name);
    }

    // Hidden only while the Sleep or Swimming look is on, never in combat, and never while the
    // weapons are out, coming out or going away.
    [[nodiscard]] constexpr bool WeaponsHidden(bool lookOn, bool inCombat, bool weaponsOut) noexcept
    {
        return lookOn && !inCombat && !weaponsOut;
    }

    // Weapons hide only once put away: while they are being drawn, drawn or being sheathed they
    // show, so a blade on its way to or from the scabbard is never hidden in hand.
    template<class WeaponState> [[nodiscard]] constexpr bool WeaponsPutAway(WeaponState state) noexcept
    {
        return state == WeaponState::kSheathed;
    }
}
