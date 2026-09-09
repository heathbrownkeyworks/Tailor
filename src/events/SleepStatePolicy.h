#pragma once

#include <RE/A/Actor.h>

namespace Tailor::Situations
{
    inline RE::SIT_SLEEP_STATE ReadSleepState(const RE::Actor* actor) noexcept
    {
        // Direct inherited access uses the compiler's class layout, which differs
        // from engine actor layouts in a build supporting multiple runtimes.
        return actor->AsActorState()->GetSitSleepState();
    }

    // Native sit/sleep states include chairs and mounts too. Do not apply the
    // Papyrus GetSleepState() > 0 comparison directly to this combined enum.
    constexpr bool IsSleepState(RE::SIT_SLEEP_STATE state) noexcept
    {
        switch (state) {
        case RE::SIT_SLEEP_STATE::kWantToSleep:
        case RE::SIT_SLEEP_STATE::kWaitingForSleepAnim:
        case RE::SIT_SLEEP_STATE::kIsSleeping:
        case RE::SIT_SLEEP_STATE::kWantToWake:
            return true;
        default:
            return false;
        }
    }
}
