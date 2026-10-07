#include "outfit/Children.h"

#include "api/ModOverrides.h"
#include "events/SituationHandler.h"
#include "events/SituationHelmets.h"
#include "events/SituationWeapons.h"
#include "outfit/ChildPolicy.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/OutfitManager.h"
#include "wig/WigAssignments.h"
#include "wig/WigManager.h"

namespace Tailor::Children
{
    namespace
    {
        // The child being released now: what the release calls must not start it again.
        RE::FormID sReleasing = 0;
        // Children whose release failed this session, so the log says it once.
        std::unordered_set<RE::FormID> sReported;

        Held HeldBy(RE::FormID id)
        {
            const auto& helmets = SituationHelmets::GetSingleton().Actors();
            Held held;
            held.outfitRow = OutfitAssignments::GetSingleton().GetAssignment(id) != nullptr;
            held.wigRow = WigAssignments::GetSingleton().GetState(id).has_value();
            held.situationWigs = WigAssignments::GetSingleton().HasAnySituation(id);
            held.modOverride = ModOverrides::GetSingleton().Contains(id);
            held.returningToOwn = OutfitManager::GetSingleton().IsReturningToOwnOutfit(id);
            held.helmets = std::ranges::find(helmets, id) != helmets.end();
            return held;
        }

        bool Release(RE::Actor* actor)
        {
            const auto id = actor->GetFormID();
            auto* situations = SituationHandler::GetSingleton();
            // Helmets go back, as the same copies, when the look they came off is still worn. After a load, copies
            // taken off under a Tailor look are forgotten, and the outfit restore below takes those pieces away.
            // Weapons are shown.
            SituationHelmets::GetSingleton().Update(actor, false, situations->GetAppliedOutfitId(id).value_or(0));
            SituationWeapons::GetSingleton().Show(id);

            // Tailor's wig off and the natural hair color back, before the outfit changes, so no wig goes back on.
            auto& wigs = WigManager::GetSingleton();
            auto& wigRows = WigAssignments::GetSingleton();
            const auto wigState = wigRows.GetState(id);
            // Default Hair also deletes every other mod's hair-slot armor from the inventory, so it runs only for a
            // child Tailor holds a wig for: worn, assigned or by situation. A hair color alone gets only its reset.
            const bool wigHeld = wigState && (wigState->currentWig.formId != 0 || wigState->assignedWig.formId != 0);
            if (wigHeld || wigRows.HasAnySituation(id)) {
                if (!wigs.ResetToDefaultHair(actor)) return false;
            }
            if (wigState && wigState->HasHairColor()) {
                // Cleared from the row before the reset, as ResetHairColor's callers must.
                wigRows.ClearHairColor(id);
                wigRows.Save();
                wigs.ResetHairColor(actor);
                wigs.ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
            }

            // Their own outfit back, only when Tailor dressed them: an outfit, a situation or random flag, a pending
            // restore, an override or the return mark. A row holding only an armor type never dressed anyone.
            auto& outfits = OutfitManager::GetSingleton();
            auto& rows = OutfitAssignments::GetSingleton();
            const bool dressed = rows.HasAssignment(id) || rows.IsRestoreDefaultPending(id) ||
                ModOverrides::GetSingleton().Contains(id) || outfits.IsReturningToOwnOutfit(id);
            if (dressed) {
                if (!outfits.RestoreOriginalOutfit(actor, true)) return false;
                OutfitManager::NotifyOutfitChanged(actor);
            }

            // Every record of them.
            situations->ClearOutfitOverrides(id);
            ModOverrides::GetSingleton().Clear(id);
            outfits.ForgetReturnToOwnOutfit(id);
            if (rows.GetAssignment(id) != nullptr) {
                rows.SetAdventuringArmorType(id, OutfitArmorType::Any);
                rows.Unassign(id);
                rows.ClearRestoreDefaultPending(id);
                rows.Save();
            }
            return true;
        }
    }

    bool Skip(RE::Actor* actor)
    {
        if (!actor || !actor->IsChild()) return false;
        const auto id = actor->GetFormID();
        if (sReleasing == id || !NeedsRelease(HeldBy(id)) || !actor->Is3DLoaded() || actor->IsDead()) return true;
        sReleasing = id;
        const bool released = Release(actor);
        sReleasing = 0;
        if (released) {
            sReported.erase(id);
            logger::info("Children: gave {} ({:08X}) their own outfit and hair back; Tailor doesn't dress children",
                actor->GetDisplayFullName(), id);
        } else if (sReported.insert(id).second) {
            logger::warn("Children: couldn't give {} ({:08X}) their own outfit and hair back yet; trying again later",
                actor->GetDisplayFullName(), id);
        }
        return true;
    }

    bool CrosshairIsChild()
    {
        auto* pick = RE::CrosshairPickData::GetSingleton();
        if (!pick) return false;
        // As OutfitManager::UpdateTargetFromCrosshair reads it: the first actor among the targets.
        for (auto& handle : pick->target) {
            const auto ref = handle.get();
            if (auto* actor = ref ? ref->As<RE::Actor>() : nullptr) return actor->IsChild();
        }
        return false;
    }
}
