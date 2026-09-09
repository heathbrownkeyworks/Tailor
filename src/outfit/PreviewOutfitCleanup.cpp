#include "outfit/PreviewOutfitCleanup.h"

namespace Tailor::Outfits
{
    namespace
    {
        struct Instance
        {
            RE::TESBoundObject* object = nullptr;
            RE::ExtraDataList* extra = nullptr;
        };

        bool IsBelow(RE::NiAVObject* root, RE::NiAVObject* node)
        {
            for (auto* parent = node ? node->parent : nullptr; parent; parent = parent->parent) {
                if (parent == root) return true;
            }
            return false;
        }

        bool HasWornCopy(RE::Actor* actor, RE::TESBoundObject* object)
        {
            auto* inventory = actor->GetInventoryChanges();
            if (!inventory || !inventory->entryList) return false;
            for (auto* entry : *inventory->entryList) {
                if (!entry || entry->object != object || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    if (extra && extra->GetWorn()) return true;
                }
            }
            return false;
        }

        // Capture before native unequip: it can erase the inventory instance or
        // change a slot's item/addon while leaving its outgoing clone attached.
        // Strong references keep that exact identity valid across native calls.
        struct PreviewParts
        {
            struct Part
            {
                RE::TESBoundObject* armor;
                RE::NiPointer<RE::NiAVObject> clone;
            };
            RE::NiPointer<RE::NiAVObject> root;
            RE::BSTSmartPointer<RE::BipedAnim> biped;
            std::vector<Part> parts;

            PreviewParts(RE::Actor* actor, RE::FormID primary, RE::FormID alternate)
            {
                if (!actor) return;
                root.reset(actor->Get3D(false));
                biped = actor->GetBiped(false);
                auto* inventory = actor->GetInventoryChanges();
                if (!root || !biped || !inventory || !inventory->entryList) return;
                for (auto* entry : *inventory->entryList) {
                    if (!entry || !entry->object || !entry->object->IsArmor() || !entry->extraLists) continue;
                    const bool owned = std::any_of(entry->extraLists->begin(), entry->extraLists->end(),
                        [&](RE::ExtraDataList* extra) {
                            auto* marker = extra ? extra->GetByType<RE::ExtraOutfitItem>() : nullptr;
                            return marker && marker->id && (marker->id == primary || marker->id == alternate);
                        });
                    if (!owned) continue;
                    for (auto* slots : { &biped->objects, &biped->bufferedObjects }) {
                        for (const auto& slot : *slots) {
                            if (slot.item != entry->object || !IsBelow(root.get(), slot.partClone.get())) continue;
                            if (std::none_of(parts.begin(), parts.end(), [&](const auto& saved) {
                                    return saved.clone.get() == slot.partClone.get();
                                })) {
                                parts.push_back({ entry->object, slot.partClone });
                            }
                        }
                    }
                }
            }

            void Retire(RE::Actor* actor)
            {
                if (!root || !biped || actor->Get3D(false) != root.get() ||
                    actor->GetBiped(false).get() != biped.get()) return;
                std::size_t detached = 0, cleared = 0, retained = 0;
                for (const auto& part : parts) {
                    // Another outfit, an ordinary inventory copy, or an equipment
                    // listener may still own this armor. Leave that appearance alone.
                    if (HasWornCopy(actor, part.armor)) {
                        ++retained;
                        continue;
                    }
                    auto* clone = part.clone.get();
                    if (clone->parent) {
                        if (!IsBelow(root.get(), clone)) continue;
                        clone->parent->DetachChild(clone);
                        if (clone->parent) continue;
                        ++detached;
                    }
                    for (auto* slots : { &biped->objects, &biped->bufferedObjects }) {
                        for (auto& slot : *slots) {
                            // Preserve replacement clones and all engine-owned
                            // handles/metadata. Only retire the captured pointer.
                            if (slot.partClone.get() == clone) {
                                slot.partClone.reset();
                                ++cleared;
                            }
                        }
                    }
                }
                if (!parts.empty()) {
                    logger::info("PreviewMeshCleanup: captured {} clones; detached {}; cleared {} clone references; retained {} for worn copies",
                        parts.size(), detached, cleared, retained);
                }
            }
        };

        Instance FindPreviewItem(RE::Actor* actor, RE::FormID primary,
            RE::FormID alternate, bool wornOnly)
        {
            auto* inventory = actor ? actor->GetInventoryChanges() : nullptr;
            if (!inventory || !inventory->entryList) return {};
            for (auto* entry : *inventory->entryList) {
                if (!entry || !entry->object || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    auto* marker = extra ? extra->GetByType<RE::ExtraOutfitItem>() : nullptr;
                    if (!marker || !marker->id ||
                        (marker->id != primary && marker->id != alternate)) continue;
                    if (!wornOnly || extra->GetWorn()) return { entry->object, extra };
                }
            }
            return {};
        }
    }

    bool IsOutfitInstance(RE::Actor* actor, RE::TESBoundObject* object,
        RE::ExtraDataList* extra, RE::FormID outfit)
    {
        auto* inventory = actor ? actor->GetInventoryChanges() : nullptr;
        if (!inventory || !inventory->entryList || !extra) return false;
        for (auto* entry : *inventory->entryList) {
            if (!entry || entry->object != object || !entry->extraLists) continue;
            for (auto* current : *entry->extraLists) {
                if (current != extra) continue;
                auto* marker = current->GetByType<RE::ExtraOutfitItem>();
                return marker && marker->id == outfit;
            }
        }
        return false;
    }

    bool UnequipPreviewItems(RE::Actor* actor, RE::FormID primary, RE::FormID alternate)
    {
        PreviewParts parts(actor, primary, alternate);
        bool unequipped = false;
        // Re-scan after every native call: equipment events can mutate or remove
        // inventory entries. Never retain an iterator or dereference an old list.
        for (std::size_t count = 0; count < 256; ++count) {
            const auto item = FindPreviewItem(actor, primary, alternate, true);
            if (!item.extra) {
                unequipped = true;
                break;
            }
            auto* manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) break;
            manager->UnequipObject(actor, item.object, item.extra, 1, nullptr,
                false, false, false, true);
            // Do not remove a still-worn instance or bypass PreventRemoval.
            // Revalidate membership before inspecting an extra list again.
            if ((IsOutfitInstance(actor, item.object, item.extra, primary) ||
                 IsOutfitInstance(actor, item.object, item.extra, alternate)) &&
                item.extra->GetWorn()) break;
        }
        parts.Retire(actor);
        return unequipped;
    }

    bool HasPreviewItems(RE::Actor* actor, RE::FormID primary, RE::FormID alternate)
    {
        return FindPreviewItem(actor, primary, alternate, false).extra != nullptr;
    }
}
