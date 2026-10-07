#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Tailor::Player
{
    // Seconds the game runs out of combat before the player changes back.
    inline constexpr float kLeaveCombatSeconds = 5.0f;
    // A longer gap between two polls means the game was paused, loading or in Tailor: it doesn't count.
    inline constexpr float kMaxPollGapSeconds = 0.5f;
    // What a rule wants when it leaves the outfit alone. Outfit IDs start at 1; 0 is Own Gear.
    inline constexpr int kKeepOutfit = -1;

    // What dresses the player now. A transformed player is left alone, and combat outranks water.
    enum class PlayerRule { Suspended, Fight, Swim, Land };

    [[nodiscard]] constexpr PlayerRule DecidePlayerRule(bool dressable, bool fighting, bool swimming) noexcept
    {
        if (!dressable) return PlayerRule::Suspended;
        if (fighting) return PlayerRule::Fight;
        if (swimming) return PlayerRule::Swim;
        return PlayerRule::Land;
    }

    // Knee-deep: the water's surface this far above the player's feet, about half a metre.
    inline constexpr float kKneeDeep = 36.0f;

    // In water while the engine says the player swims, or once the water is knee-deep; then until
    // they are fully out, the surface at or below their feet, so wading along a shore doesn't switch
    // outfits back and forth. Feet in the air while wading (a jump, a stumble) haven't left the water.
    // Where there is no water the game reports the lowest float (-NI_INFINITY), which no depth test
    // passes, and a non-finite height counts as none: then only swimming counts.
    [[nodiscard]] inline bool PlayerInWater(bool wasInWater, bool swimming, bool airborne, float waterHeight, float feetZ) noexcept
    {
        if (swimming) return true;
        if (wasInWater && airborne) return true;
        if (!std::isfinite(waterHeight) || !std::isfinite(feetZ)) return false;
        const float depth = waterHeight - feetZ;
        return wasInWater ? depth > 0.0f : depth >= kKneeDeep;
    }

    // The outfit a rule wants: an outfit ID, Own Gear (0) or kKeepOutfit. A fight wears the
    // Adventuring slot, else Own Gear. Water without a Swimming outfit leaves the clothing
    // alone. On land the situation's chain ends in Own Gear.
    [[nodiscard]] constexpr int PlayerOutfitFor(PlayerRule rule, int slotId, int landId) noexcept
    {
        switch (rule) {
        case PlayerRule::Fight: return slotId > 0 ? slotId : 0;
        case PlayerRule::Swim: return slotId > 0 ? slotId : kKeepOutfit;
        case PlayerRule::Land: return landId > 0 ? landId : 0;
        default: return kKeepOutfit;
        }
    }

    // A redress (turning back from beast form, an edited outfit) puts the recorded outfit on again
    // where the rule itself would leave the outfit alone.
    [[nodiscard]] constexpr int PlayerRedressOutfit(int outfitId, int wornId) noexcept
    {
        return outfitId == kKeepOutfit ? wornId : outfitId;
    }

    // Combat and swimming never change the wig.
    [[nodiscard]] constexpr bool PlayerRuleChangesWig(PlayerRule rule) noexcept
    {
        return rule == PlayerRule::Land;
    }

    // The wig step on land: at a change of situation, or for Tailor's own actions and the reconciles.
    // A fight or a swim in between is no change of situation, so a wig the player took off stays off.
    [[nodiscard]] constexpr bool PlayerWigStepRuns(PlayerRule rule, bool automatic, bool situationChanged) noexcept
    {
        return PlayerRuleChangesWig(rule) && (!automatic || situationChanged);
    }

    // Whether the rule changes the outfit at all. On land the outfit always follows the chain,
    // which without situation outfits is the regular outfit, else Own Gear; a fight and water
    // change it only for a player with situation outfits.
    [[nodiscard]] constexpr bool PlayerRuleManagesOutfit(PlayerRule rule, bool situationOutfits) noexcept
    {
        return rule == PlayerRule::Land || (situationOutfits && rule != PlayerRule::Suspended);
    }

    // Beast form takes all of the player's gear off and gives nothing back. While their own gear is
    // what they wear (no Tailor outfit, no preview), the poll notes it...
    [[nodiscard]] constexpr bool PlayerNotesOwnGear(bool previewing, int wornOutfitId, bool ownGearRecorded) noexcept
    {
        return !previewing && wornOutfitId == 0 && !ownGearRecorded;
    }

    // ...and turning back makes the note the Own Gear record. A Tailor outfit's own record already
    // holds what came off, and an empty note has nothing to give back.
    [[nodiscard]] constexpr bool PlayerAdoptsOwnGearNote(int wornOutfitId, bool ownGearRecorded, bool noted) noexcept
    {
        return wornOutfitId == 0 && !ownGearRecorded && noted;
    }

    // Polls a situation change waits for headgear a closed Tailor session left to put back: two seconds.
    inline constexpr int kMaxHeadwearWaits = 8;

    // A restore that keeps failing would be retried, and could log, on every poll: past the cap the
    // change stops waiting and dresses the player as it did before the wait.
    [[nodiscard]] constexpr bool PlayerWaitsForHeadwear(int waits) noexcept
    {
        return waits < kMaxHeadwearWaits;
    }

    // Units the player may move from where they woke and still be getting up: about 3.7 m.
    inline constexpr float kLeaveBedDistance = 256.0f;

    // Vanilla beds don't put the player into bed, so they wake up in their Sleep outfit instead: the
    // note holds where they woke, and their situation is Sleep until they walk away from it.
    struct PlayerWakeUp
    {
        bool active = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        // The interior cell, or the worldspace outdoors, where they woke; 0 while not known (a note from
        // the co-save, until the poll sees the player). Interior coordinates are the cell's own, so another
        // interior's entrance can land near the bed's coordinates: another space means they left.
        std::uint32_t space = 0;

        constexpr void Woke(float wokeX, float wokeY, float wokeZ, std::uint32_t wokeSpace = 0) noexcept
        {
            active = true;
            x = wokeX;
            y = wokeY;
            z = wokeZ;
            space = wokeSpace;
        }

        // In another space, or more than kLeaveBedDistance from where they woke, in any direction, stairs
        // included. A space not known on either side leaves the distance alone.
        [[nodiscard]] constexpr bool WalkedAway(float posX, float posY, float posZ, std::uint32_t posSpace = 0) const noexcept
        {
            if (space != 0 && posSpace != 0 && posSpace != space) return true;
            const float dx = posX - x, dy = posY - y, dz = posZ - z;
            return dx * dx + dy * dy + dz * dz > kLeaveBedDistance * kLeaveBedDistance;
        }

        bool operator==(const PlayerWakeUp&) const = default;
    };

    // Combat, and the five seconds the game runs after it.
    struct PlayerFight
    {
        bool fighting = false;
        float calmSeconds = 0.0f;

        // Returns true when the player starts or stops fighting.
        constexpr bool Update(bool inCombat, float elapsedSeconds) noexcept
        {
            const bool was = fighting;
            if (inCombat) {
                fighting = true;
                calmSeconds = 0.0f;
            } else if (fighting) {
                calmSeconds += std::clamp(elapsedSeconds, 0.0f, kMaxPollGapSeconds);
                if (calmSeconds >= kLeaveCombatSeconds) {
                    fighting = false;
                    calmSeconds = 0.0f;
                }
            }
            return fighting != was;
        }
    };

    // Tailor leaves a werewolf or Vampire Lord alone and dresses the player when they turn back.
    struct PlayerBeastForm
    {
        bool transformed = false;

        // True once, on the poll that finds the player back in a playable race.
        constexpr bool TurnedBack(bool inBeastForm) noexcept
        {
            const bool back = transformed && !inBeastForm;
            transformed = inBeastForm;
            return back;
        }
    };
}
