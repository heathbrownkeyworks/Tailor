#include "wig/WigInventory.h"
#include "wig/HeadwearEquipment.h"

namespace Tailor::Wigs
{
    bool RemoveInventoryWigs(RE::Actor* actor, const std::unordered_set<std::uint32_t>& wigForms)
    {
        if (!actor || actor->IsPlayerRef() || !actor->Is3DLoaded()) return false;

        // Counts include base inventory plus reference changes. Snapshot forms,
        // never inventory entry/extra-list pointers across native mutations.
        const auto inventory = actor->GetInventoryCounts();
        for (const auto& [object, count] : inventory) {
            auto* armor = object ? object->As<RE::TESObjectARMO>() : nullptr;
            if (!armor || !wigForms.contains(armor->GetFormID())) continue;
            if (count <= 0 && !actor->GetWornArmor(armor->GetFormID())) continue;
            if (!SuspendWig(actor, armor)) return false;

            const auto liveCounts = actor->GetInventoryCounts();
            const auto it = liveCounts.find(armor);
            if (it != liveCounts.end() && it->second > 0) {
                actor->RemoveItem(armor, it->second, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            }
            if (actor->GetWornArmor(armor->GetFormID())) return false;
        }
        // A refused removal or immediate external re-add is a failure, not a
        // successful reset. Leave saved tracking available for another attempt.
        for (const auto& [object, count] : actor->GetInventoryCounts()) {
            if (object && count > 0 && wigForms.contains(object->GetFormID())) return false;
        }
        return true;
    }
}
