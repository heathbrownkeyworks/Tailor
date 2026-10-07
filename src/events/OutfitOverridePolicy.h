#pragma once

#include <optional>

namespace Tailor::Situations
{
    enum class OutfitOverrideResult { Inactive, Unchanged, Applied, Restored, Retry };

    template <class Snapshot>
    struct OutfitOverride
    {
        std::optional<Snapshot> previous;
        int appliedId = 0;

        // Retain the snapshot through failed swaps and restores: even a failed
        // application may already have changed the actor's equipment.
        template <class Capture, class Apply, class Restore>
        OutfitOverrideResult Update(int outfitId, Capture capture, Apply apply, Restore restore)
        {
            if (outfitId <= 0) {
                if (!previous) return OutfitOverrideResult::Inactive;
                appliedId = 0;
                if (!restore(*previous)) return OutfitOverrideResult::Retry;
                previous.reset();
                return OutfitOverrideResult::Restored;
            }
            if (previous && appliedId == outfitId) return OutfitOverrideResult::Unchanged;
            if (!previous) previous = capture();
            if (!previous || !apply(outfitId)) return OutfitOverrideResult::Retry;
            appliedId = outfitId;
            return OutfitOverrideResult::Applied;
        }
    };
}
