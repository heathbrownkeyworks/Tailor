#include "wig/WigEquipment.h"

namespace Tailor::Wigs
{
    namespace
    {
        bool SetWornProtection(RE::Actor* actor, RE::TESObjectARMO* armor, bool protect)
        {
            bool worn = false;
            bool changed = false;
            auto* inventory = actor->GetInventoryChanges();
            if (!inventory || !inventory->entryList) return false;
            for (auto* entry : *inventory->entryList) {
                if (!entry || entry->object != armor || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    if (!extra || !extra->GetWorn()) continue;
                    worn = true;
                    if (protect) {
                        if (!extra->HasType<RE::ExtraCannotWear>()) {
                            extra->Add(new RE::ExtraCannotWear());
                            changed = true;
                        }
                    } else {
                        changed |= extra->RemoveByType(RE::ExtraDataType::kCannotWear);
                    }
                }
            }
            if (changed) actor->AddChange(RE::TESObjectREFR::ChangeFlags::kInventory);
            return worn;
        }
    }

    bool EquipProtectedWig(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (!actor || !armor) return false;
        // An already-worn item can bypass the native equip path. Apply the
        // protection directly so old saves gain it without a visible re-equip.
        if (SetWornProtection(actor, armor, true)) return true;
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!manager) return false;
        // CommonLib names PreventRemoval "forceEquip". ExtraCannotWear means
        // prevent removal when worn, and prevent equip when unworn.
        // Complete under the caller's change/recovery guard. A queued wig could
        // otherwise land after a subsequent helmet equip or wig-screen exit.
        manager->EquipObject(actor, armor, nullptr, 1, nullptr, false, true, false, true);
        return true;
    }

    void ReleaseWigProtection(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        if (actor && armor) SetWornProtection(actor, armor, false);
    }
}
