#include "outfit/ArmorEquipmentStatus.h"

namespace Tailor::Outfits
{
    ArmorEquipmentStatus InspectArmorEquipment(RE::Actor* actor, RE::TESObjectARMO* armor)
    {
        ArmorEquipmentStatus result;
        if (!actor || !armor) return result;
        auto* inventory = actor->GetInventoryChanges();
        if (inventory && inventory->entryList) {
            for (auto* entry : *inventory->entryList) {
                if (!entry || entry->object != armor || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    if (extra && extra->GetWorn()) result.worn = true;
                }
            }
        }
        auto* npc = actor->GetActorBase();
        if (!npc || !npc->GetRace()) return result;
        const auto sex = npc->GetSex();
        if (sex != RE::SEX::kMale && sex != RE::SEX::kFemale) return result;
        const auto biped = actor->GetBiped(false);
        auto* root = actor->Get3D(false);
        std::size_t required = 0, attached = 0, visible = 0;
        for (auto* addon : armor->armorAddons) {
            if (!addon || !addon->IsValidRace(npc->GetRace())) continue;
            const auto* model = addon->bipedModels[static_cast<std::size_t>(sex)].GetModel();
            if (!model || !*model) continue;
            result.compatibleModel = true;
            ++required;
            if (!biped || !root) continue;
            // ARMA masks describe model slots. Bonemold's ARMO covers four
            // slots, but its one head addon needs only its slot-30 attachment.
            const auto slots = addon->GetSlotMask().underlying();
            bool addonAttached = false, addonVisible = false;
            for (unsigned i = 0; i < 32; ++i) {
                const auto& part = biped->objects[i];
                if (!(slots & (1u << i)) || part.item != armor || part.addon != addon || !part.partClone) continue;
                bool branchVisible = true;
                for (auto* node = part.partClone.get(); node; node = node->parent) {
                    branchVisible &= !node->GetAppCulled();
                    if (node == root) {
                        addonAttached = true;
                        addonVisible |= branchVisible;
                        break;
                    }
                }
            }
            attached += addonAttached;
            visible += addonVisible;
        }
        result.attached = required > 0 && attached == required;
        result.graphVisible = required > 0 && visible == required;
        return result;
    }
}
