#include "wig/HeadwearEquipment.h"
#include "wig/WigEquipment.h"

namespace Tailor::Wigs
{
    namespace
    {
        constexpr std::uint32_t hairAndHead = (1u << 0) | (1u << 1) | (1u << 11);
        constexpr std::uint32_t headParts = hairAndHead | (1u << 12) | (1u << 13);

        std::uint32_t RenderedHeadSlots(RE::Actor* actor, RE::TESObjectARMO* armor)
        {
            auto* npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc || !armor) return 0;
            const auto sex = npc->GetSex();
            if (sex != RE::SEX::kMale && sex != RE::SEX::kFemale) return 0;
            std::uint32_t slots = 0;
            for (auto* addon : armor->armorAddons) {
                if (!addon || !npc->GetRace() || !addon->IsValidRace(npc->GetRace())) continue;
                const char* model = addon->bipedModels[static_cast<std::size_t>(sex)].GetModel();
                if (model && *model) slots |= addon->GetSlotMask().underlying();
            }
            return slots & headParts;
        }

        struct Instance
        {
            RE::TESObjectARMO* armor = nullptr;
            RE::ExtraDataList* extra = nullptr;
        };

        template<class Key>
        Instance Find(RE::Actor* actor, const Key& key)
        {
            auto* inventory = actor ? actor->GetInventoryChanges() : nullptr;
            if (!inventory || !inventory->entryList) return {};
            for (auto* entry : *inventory->entryList) {
                if (!entry || !entry->object || entry->object->GetFormID() != key.form || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    auto* id = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                    if (id && id->baseID == key.owner && id->uniqueID == key.unique)
                        return {entry->object->As<RE::TESObjectARMO>(), extra};
                }
            }
            return {};
        }

        std::vector<Instance> WornArmor(RE::Actor* actor)
        {
            std::vector<Instance> result;
            auto* inventory = actor ? actor->GetInventoryChanges() : nullptr;
            if (!inventory || !inventory->entryList) return result;
            for (auto* entry : *inventory->entryList) {
                auto* armor = entry && entry->object ? entry->object->As<RE::TESObjectARMO>() : nullptr;
                if (!armor || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    if (extra && extra->GetWorn()) result.push_back({armor, extra});
                }
            }
            return result;
        }
    }

    bool HeadwearConflicts(RE::Actor* actor, RE::TESObjectARMO* wig, RE::TESObjectARMO* armor)
    {
        if (!wig || !armor || wig == armor) return false;
        const auto armorSlots = armor->GetSlotMask().underlying();
        const auto modelSlots = RenderedHeadSlots(actor, armor);
        // A head-slot helmet still wins even when it leaves the hair bit free.
        return ((armorSlots | modelSlots) & 1u) != 0 ||
            (wig->GetSlotMask().underlying() & armorSlots) != 0 ||
            (RenderedHeadSlots(actor, wig) & modelSlots) != 0;
    }

    bool OutfitHidesWig(RE::Actor* actor, RE::TESObjectARMO* wig, RE::TESObjectARMO* previousWig)
    {
        if (!actor || !wig) return false;
        for (const auto& item : WornArmor(actor)) {
            if (item.armor != previousWig && HeadwearConflicts(actor, wig, item.armor)) return true;
        }
        // Include the intended NPC outfit even if its helmet was rejected by a
        // legacy protected wig or its queued equip has not run yet.
        auto* npc = actor->GetActorBase();
        if (!actor->IsPlayerRef() && npc && npc->defaultOutfit) {
            for (auto* item : npc->defaultOutfit->outfitItems) {
                if (item && HeadwearConflicts(actor, wig, item->As<RE::TESObjectARMO>())) return true;
            }
        }
        return false;
    }

    bool SuspendWig(RE::Actor* actor, RE::TESObjectARMO* wig)
    {
        if (!actor || !wig) return true;
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!manager) return false;
        ReleaseWigProtection(actor, wig);
        // Native calls can remove inventory entries through listeners. Re-scan
        // each time and never dereference the old extra list after the call.
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const auto worn = WornArmor(actor);
            const auto it = std::find_if(worn.begin(), worn.end(), [&](const auto& item) { return item.armor == wig; });
            if (it == worn.end()) return true;
            manager->UnequipObject(actor, wig, it->extra, 1, nullptr, false, false, false, true);
            const auto remaining = WornArmor(actor);
            if (std::any_of(remaining.begin(), remaining.end(), [&](const auto& item) { return item.armor == wig && item.extra == it->extra; })) break;
        }
        logger::warn("Headwear: could not suspend wig {:08X} on {:08X}", wig->GetFormID(), actor->GetFormID());
        EquipProtectedWig(actor, wig);
        return false;
    }

    bool HeadwearPreview::Hide(RE::Actor* actor, RE::TESObjectARMO* wig)
    {
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!actor || !manager) return false;
        auto* inventory = actor->GetInventoryChanges();
        if (!inventory) return false;
        // Identify every copy before the first native call. Unique IDs preserve
        // enchantment/temper instances and prevent pointer-reuse restoration.
        std::vector<Copy> pending;
        for (const auto& item : WornArmor(actor)) {
            if (item.armor == wig) continue;
            if (!HeadwearConflicts(actor, wig, item.armor) &&
                !((item.armor->GetSlotMask().underlying() | RenderedHeadSlots(actor, item.armor)) & hairAndHead)) continue;
            if (item.extra->HasType<RE::ExtraCannotWear>()) return false;
            auto* id = item.extra->GetByType<RE::ExtraUniqueID>();
            if (!id) {
                id = new RE::ExtraUniqueID(actor->GetFormID(), inventory->GetNextUniqueID());
                item.extra->Add(id);
                actor->AddChange(RE::TESObjectREFR::ChangeFlags::kInventory);
            }
            pending.push_back({item.armor->GetFormID(), id->baseID, id->uniqueID});
        }
        for (const auto& key : pending) {
            auto item = Find(actor, key);
            if (!item.extra || !item.extra->GetWorn()) continue;
            if (std::find(_displaced.begin(), _displaced.end(), key) == _displaced.end()) _displaced.push_back(key);
            manager->UnequipObject(actor, item.armor, item.extra, 1, nullptr, false, false, false, true);
            item = Find(actor, key);
            if (item.extra && item.extra->GetWorn()) return false;
        }
        return true;
    }

    bool HeadwearPreview::Restore(RE::Actor* actor)
    {
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!actor || !manager) return _displaced.empty();
        std::vector<Copy> pending;
        for (const auto& key : _displaced) {
            auto item = Find(actor, key);
            if (!item.extra) continue; // legitimately removed or replaced; never manufacture a copy
            if (!item.extra->GetWorn()) {
                manager->EquipObject(actor, item.armor, item.extra, 1, nullptr, false, false, false, true);
                item = Find(actor, key);
            }
            if (item.extra && !item.extra->GetWorn()) pending.push_back(key);
        }
        _displaced = std::move(pending);
        return _displaced.empty();
    }
}
