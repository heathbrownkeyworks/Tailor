#pragma once

#include <RE/A/ActorState.h>

namespace Tailor::Situations
{
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
