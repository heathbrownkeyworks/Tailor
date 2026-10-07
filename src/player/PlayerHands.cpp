#include "player/PlayerHands.h"

#include <optional>
#include <utility>

// Windows.h's wingdi.h macro rewrites any GetObject call to GetObjectA/W;
// CommonLibSSE-NG's own Variable.h works around the same collision.
#undef GetObject

namespace Tailor::Player
{
    namespace
    {
        const RE::BGSEquipSlot* HandSlot(Hand hand)
        {
            auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
            if (!defaults) return nullptr;
            return defaults->GetObject<RE::BGSEquipSlot>(hand == Hand::Left ?
                RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip);
        }

        bool TwoHanded(RE::TESObjectWEAP* weapon)
        {
            return weapon && (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow());
        }

        bool HeldIn(RE::ExtraDataList* extra, Hand hand)
        {
            return extra && (hand == Hand::Left ? extra->HasType<RE::ExtraWornLeft>() : extra->HasType<RE::ExtraWorn>());
        }

        // The copy of this weapon or torch in the hand, tagged so it returns as itself.
        std::optional<CopyKey> TagHeld(RE::Actor* player, RE::TESForm* object, Hand hand)
        {
            auto* inventory = player->GetInventoryChanges();
            if (!object || !inventory || !inventory->entryList) return std::nullopt;
            for (auto* entry : *inventory->entryList) {
                if (!entry || entry->object != object || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    if (!HeldIn(extra, hand)) continue;
                    auto* id = extra->GetByType<RE::ExtraUniqueID>();
                    if (!id) {
                        id = new RE::ExtraUniqueID(player->GetFormID(), inventory->GetNextUniqueID());
                        extra->Add(id);
                        player->AddChange(RE::TESObjectREFR::ChangeFlags::kInventory);
                    }
                    return CopyKey{object->GetFormID(), id->baseID, id->uniqueID};
                }
            }
            return std::nullopt;
        }

        std::pair<RE::TESBoundObject*, RE::ExtraDataList*> FindCopy(RE::Actor* player, const CopyKey& key)
        {
            auto* inventory = player->GetInventoryChanges();
            if (!inventory || !inventory->entryList) return {};
            for (auto* entry : *inventory->entryList) {
                if (!entry || !entry->object || entry->object->GetFormID() != key.form || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    auto* id = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                    if (id && id->baseID == key.owner && id->uniqueID == key.unique) return {entry->object, extra};
                }
            }
            return {};
        }
    }

    std::vector<DisplacedItem> TakeHandsForShield(RE::Actor* player)
    {
        std::vector<DisplacedItem> taken;
        if (!player) return taken;
        if (auto* left = player->GetEquippedObject(true); left && !left->Is(RE::FormType::Armor)) {
            // Only a spell has no copy. A scroll is a SpellItem too, but one the player owns
            // and can use up, so it is tagged in the inventory like a weapon or torch.
            if (left->Is(RE::FormType::Spell)) {
                taken.push_back({{left->GetFormID(), 0, 0}, Hand::Left});
            } else if (const auto key = TagHeld(player, left, Hand::Left)) {
                taken.push_back({*key, Hand::Left});
            }
        }
        if (auto* right = player->GetEquippedObject(false); right && TwoHanded(right->As<RE::TESObjectWEAP>())) {
            if (const auto key = TagHeld(player, right, Hand::Right)) taken.push_back({*key, Hand::Right});
        }
        return taken;
    }

    std::vector<DisplacedItem> ReturnHands(RE::Actor* player, const std::vector<DisplacedItem>& items)
    {
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (!player || !manager) return items;
        auto* left = player->GetEquippedObject(true);
        const bool shield = left && left->Is(RE::FormType::Armor);
        std::vector<DisplacedItem> waiting;
        for (const auto& item : items) {
            // A shield still holds the left hand: both hands wait for it to come off.
            if (shield) {
                waiting.push_back(item);
                continue;
            }
            // The player has put something else in that hand since: leave it there.
            if (player->GetEquippedObject(item.hand == Hand::Left)) continue;
            if (!item.copy.unique) {
                if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(item.copy.form)) {
                    manager->EquipSpell(player, spell, HandSlot(item.hand));
                }
                continue;
            }
            // Sold or dropped since: nothing to give back.
            if (const auto [object, extra] = FindCopy(player, item.copy); object) {
                manager->EquipObject(player, object, extra, 1, HandSlot(item.hand), false, false, false, true);
            }
        }
        return waiting;
    }
}
