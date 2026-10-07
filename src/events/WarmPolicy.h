#pragma once

#include <cstdint>
#include <optional>

namespace Tailor::Situations
{
    // Weather classification bits as the weather record stores them: 1 pleasant, 2 cloudy, 4 rainy,
    // 8 snow. The same categories Papyrus's Weather.GetClassification() reports as 0 to 3.
    inline constexpr std::uint8_t kColdWeatherFlags = 0x2 | 0x4 | 0x8;
    inline constexpr std::uint8_t kSnowWeatherFlag = 0x8;

    // Cloudy, rainy or snowy weather. During a change of weather the sky blends from the last weather to
    // the current one (0 to 1): the last one decides until the new one is halfway in, so nobody takes
    // their cloak off while the snow is still falling.
    [[nodiscard]] constexpr bool IsColdWeather(std::uint8_t currentFlags, std::uint8_t lastFlags,
        bool changing, float currentPct) noexcept
    {
        const std::uint8_t flags = changing && currentPct < 0.5f ? lastFlags : currentFlags;
        return (flags & kColdWeatherFlags) != 0;
    }

    // A snowy region: snow weathers hold at least a quarter of its weather chance. Its clear days count
    // too, since snowy places have "sunny" weathers of their own, classified pleasant.
    [[nodiscard]] constexpr bool IsSnowyRegion(std::uint64_t snowChance, std::uint64_t totalChance) noexcept
    {
        return totalChance > 0 && snowChance * 4 >= totalChance;
    }

    // What one NPC re-dress does: whether it dresses them at all, puts their outfit on, and runs their wig step.
    struct NpcRedress
    {
        bool dress = true;
        bool putOutfitOn = true;
        bool runWigStep = false;
    };

    // `cached` is the situation the NPC was last dressed for (none after a load or a forced re-evaluation),
    // `outfitOn` whether the outfit this situation resolves to is the one they were last dressed in, and
    // `wigStepDue` whether their wig step last ran for another wig situation, or hasn't run since.
    // - Nothing changed: nothing is dressed, except that Warm keeps its outfit while the location beneath it
    //   changes, and the wig still follows the location.
    // - Warm starting or ending, or a move between Home and Town, with the outfit already on puts nothing on
    //   again (an NPC without a Home outfit wears Town's at home).
    // - The wig follows the situation beneath Warm, which has no wig: the step runs when that changed since it
    //   last ran, or for a re-dress in the same situation (a new daily pick, an edited outfit). Warm starting
    //   or ending never runs it.
    template<class Situation>
    [[nodiscard]] constexpr NpcRedress DecideNpcRedress(std::optional<Situation> cached, Situation situation,
        bool outfitOn, bool hasWigSituations, bool wigStepDue) noexcept
    {
        const bool sameSituation = cached && *cached == situation;
        if (sameSituation && outfitOn) {
            return {false, false, situation == Situation::Warm && hasWigSituations && wigStepDue};
        }
        const bool warmFlip = cached && !sameSituation && (*cached == Situation::Warm || situation == Situation::Warm);
        const bool homeTownFlip = cached && !sameSituation &&
            ((*cached == Situation::Home && situation == Situation::Town) || (*cached == Situation::Town && situation == Situation::Home));
        return {true, !((warmFlip || homeTownFlip) && outfitOn), hasWigSituations && (sameSituation || wigStepDue)};
    }

    // The poll's cold state for one NPC. `seen` is the state it last recorded, none on its first look, when
    // `cachedWarm` (whether the NPC was last dressed for Warm) stands in for it. While a swim or a fight holds
    // the NPC the recorded state stays, so a change during it re-dresses them once it ends.
    struct ColdObservation
    {
        bool changed = false;
        bool record = false;
    };

    [[nodiscard]] constexpr ColdObservation ObserveCold(std::optional<bool> seen, bool cachedWarm, bool cold,
        bool overridden) noexcept
    {
        return {(seen ? *seen : cachedWarm) != cold, !seen || !overridden};
    }
}
