#pragma once

namespace Tailor::Wigs
{
    // What a situation change does to the wig.
    enum class SituationWigStep { Keep, Equip, TakeOff };

    template<class Wig> struct SituationWigChoice
    {
        SituationWigStep step = SituationWigStep::Keep;
        Wig wig{};
    };

    // The wig for a situation: its own, in Sleep the wig of the situation besides Sleep, for Home (or Sleep beside it) the Town wig, else the
    // Adventuring wig. When the actor leaves Sleep with the Sleep wig on and none of those is set, the
    // assigned wig (the Hair Dresser's) goes on, else the Sleep wig comes off for their own hair. Only
    // leaving Sleep does that: `fromSleep` means their previous situation was Sleep, or isn't known.
    // Any other change without a wig keeps the one worn, even a Sleep wig that is also another
    // situation's. `slot(s)` is the actor's wig for situation s; a wig whose formId is 0 is none.
    template<class Situation, class Wig, class Slot>
    [[nodiscard]] SituationWigChoice<Wig> ChooseSituationWig(Situation situation, Situation besidesSleep, bool fromSleep,
        Slot&& slot, const Wig& worn, const Wig& assigned)
    {
        Wig wig = slot(situation);
        const Situation byLocation = situation == Situation::Sleep ? besidesSleep : situation;
        if (wig.formId == 0 && situation == Situation::Sleep) wig = slot(besidesSleep);
        if (wig.formId == 0 && byLocation == Situation::Home) wig = slot(Situation::Town);
        if (wig.formId == 0 && situation != Situation::Adventuring) wig = slot(Situation::Adventuring);
        if (wig.formId != 0) return {SituationWigStep::Equip, wig};
        const Wig sleepWig = slot(Situation::Sleep);
        const bool sleepEnded = fromSleep && situation != Situation::Sleep && sleepWig.formId != 0 && worn == sleepWig;
        if (!sleepEnded) return {};
        if (assigned.formId != 0) return {SituationWigStep::Equip, assigned};
        return {SituationWigStep::TakeOff, Wig{}};
    }

    // Warm has no wig: under Warm the wig follows `location`, the situation by location beneath it.
    template<class Situation>
    [[nodiscard]] constexpr Situation WigSituation(Situation situation, Situation location) noexcept
    {
        return situation == Situation::Warm ? location : situation;
    }
}
