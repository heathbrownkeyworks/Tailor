#include "player/PlayerEquipment.h"
#include "outfit/PreviewOutfitCleanup.h"

namespace Tailor::Player
{
    namespace
    {
        RE::PlayerCharacter* Actor() { return RE::PlayerCharacter::GetSingleton(); }

        RE::ExtraDataList* NewItemData()
        {
            // CommonLib declares no usable ExtraDataList constructor. Its runtime
            // layout is two null pointers and an unlocked BSReadWriteLock, with a
            // BaseExtraList vtable added in AE 1.6.629. Take that vtable from the
            // engine-created player list; never copy its entries or lock state.
            const bool hasVtable = REL::Module::IsAE() &&
                REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_629;
            auto* extra = RE::calloc<RE::ExtraDataList>(1, hasVtable ? 0x20 : 0x18);
            if (extra && hasVtable) std::memcpy(extra, &Actor()->extraList, sizeof(void*));
            return extra;
        }

        RE::ExtraDataList* Find(const Instance& key)
        {
            auto* actor = Actor();
            auto* inventory = actor ? actor->GetInventoryChanges() : nullptr;
            if (!inventory || !inventory->entryList) return nullptr;
            for (auto* entry : *inventory->entryList) {
                if (!entry || !entry->object || entry->object->GetFormID() != key.form || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    auto* id = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                    if (id && id->baseID == key.owner && id->uniqueID == key.unique) return extra;
                }
            }
            return nullptr;
        }

        Instance Identify(RE::TESBoundObject* object, RE::ExtraDataList* extra)
        {
            auto* id = extra->GetByType<RE::ExtraUniqueID>();
            if (!id) {
                id = new RE::ExtraUniqueID(ReferenceID, Actor()->GetInventoryChanges()->GetNextUniqueID());
                extra->Add(id);
                Actor()->AddChange(RE::TESObjectREFR::ChangeFlags::kInventory);
            }
            return {object->GetFormID(), id->baseID, id->uniqueID, extra->HasType<RE::ExtraWornLeft>()};
        }

        bool Unequip(const Instance& key)
        {
            auto* extra = Find(key);
            if (!extra || !extra->GetWorn()) return true;
            auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(key.form);
            if (!object) return false;
            return Outfits::UnequipArmorInstance(Actor(), object, extra);
        }

        bool Equip(const Instance& key)
        {
            auto* extra = Find(key);
            auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(key.form);
            if (!extra || !object) return false;
            if (extra->GetWorn()) return true;
            auto* manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) return false;
            auto* slot = object->IsWeapon() ? RE::TESForm::LookupByID<RE::BGSEquipSlot>(key.left ? 0x13F42 : 0x13F43) : nullptr;
            manager->EquipObject(Actor(), object, extra, 1, slot, false, false, false, true);
            extra = Find(key);
            return extra && extra->GetWorn();
        }
    }

    Equipment& Equipment::GetSingleton() { static Equipment instance; return instance; }

    void Equipment::FinishChange()
    {
        auto* actor = Actor();
        auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
        if (!REL::Module::IsVR() && actor && actor->Is3DLoaded() && actor->Get3D(false) &&
            process && process->middleHigh && process->high) {
            // Free camera can defer the player's ordinary equipment-model pass.
            // Run the native equipment update after the complete transaction,
            // while reentrant equip callbacks are still guarded by _changing.
            // Same entry point as CommonLib's Update3DModel_Impl and Fitting
            // Room's RefreshActor; avoid the wrapper's generic NiNode event.
            const auto before = actor->GetBiped(false);
            const bool hadBody = before && before->objects[2].partClone;
            process->Set3DUpdateFlag(RE::RESET_3D_FLAGS::kModel);
            using Update = void (*)(RE::AIProcess*, RE::Actor*);
            static REL::Relocation<Update> update{RELOCATION_ID(38404, 39395)};
            update(process, actor);
            // The player's equipment pass queues weapon attachments. Drain the
            // native queue too so existing weapons remain intact during preview.
            using Flush = void (*)(RE::PlayerCharacter*);
            static REL::Relocation<Flush> flush{RELOCATION_ID(39367, 40439)};
            flush(actor);
            const auto after = actor->GetBiped(false);
            logger::info("Player equipment models updated: bodyBefore={}, bodyAfter={}",
                hadBody, after && after->objects[2].partClone);
        }
        _changing = false;
    }

    void Equipment::RefreshModels()
    {
        if (_changing) return;
        _changing = true;
        FinishChange();
    }

    EquipmentSnapshot Equipment::Capture()
    {
        EquipmentSnapshot result;
        auto* actor = Actor();
        actor->InitInventoryIfRequired();
        auto* inventory = actor->GetInventoryChanges();
        if (inventory && inventory->entryList) {
            for (auto* entry : *inventory->entryList) {
                if (!entry || !entry->object || !entry->extraLists) continue;
                if (!entry->object->IsArmor() && !entry->object->IsWeapon()) continue;
                for (auto* extra : *entry->extraLists) {
                    if (extra && extra->GetWorn()) result.worn.push_back(Identify(entry->object, extra));
                }
            }
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (auto* item = actor->GetEquippedObject(i == 0)) result.hands[i] = item->GetFormID();
        }
        return result;
    }

    bool Equipment::Affects(Channel channel, RE::TESObjectARMO* armor) const
    {
        if (!armor) return false;
        const auto slots = armor->GetSlotMask().underlying();
        if (channel == Channel::Wig) return (slots & _state[1].slots) != 0;
        // Always retire our own outfit copies, even if an outfit includes hair
        // slots. Other hair armor remains separate, as it does for NPCs.
        for (const auto& item : _state[0].supplied) if (item.form == armor->GetFormID() && Find(item)) return true;
        constexpr auto hairSlots = (1u << 1) | (1u << 11);
        if ((slots & hairSlots) && !(slots & ~hairSlots)) return false;
        for (const auto& item : _state[1].supplied) if (item.form == armor->GetFormID() && Find(item)) return false;
        return true;
    }

    bool Equipment::BeginPreview(Channel channel)
    {
        auto* actor = Actor();
        if (!actor || !actor->Is3DLoaded() || _changing) return false;
        const auto index = static_cast<std::size_t>(channel);
        if (_preview[index] && !EndPreview(channel, false)) return false;
        _preview[index] = Preview{_state[index], Capture()};
        return true;
    }

    bool Equipment::Prune(Channel channel, const std::vector<Instance>& keep)
    {
        auto& owned = _state[static_cast<std::size_t>(channel)].supplied;
        bool complete = true;
        std::vector<Instance> retained;
        for (const auto& key : owned) {
            if (ContainsCopy(keep, key)) { retained.push_back(key); continue; }
            auto* extra = Find(key);
            if (!extra) continue; // sold, dropped or removed by another system
            if (extra->GetWorn()) { retained.push_back(key); complete = false; continue; }
            auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(key.form);
            if (object) Actor()->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kRemove, extra, nullptr);
            if (Find(key)) { retained.push_back(key); complete = false; }
        }
        owned = std::move(retained);
        return complete;
    }

    bool Equipment::Restore(Channel channel, const EquipmentSnapshot& snapshot)
    {
        const auto current = Capture();
        for (const auto& key : current.worn) {
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(key.form);
            if (Affects(channel, armor) && !ContainsCopy(snapshot.worn, key) && !Unequip(key)) return false;
        }
        bool restored = true;
        for (const auto& key : snapshot.worn) {
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(key.form);
            if (!Affects(channel, armor)) continue;
            if (Find(key)) restored = Equip(key) && restored;
            else logger::warn("Player equipment: original copy {:08X}/{} is no longer in inventory", key.form, key.unique);
        }
        if (channel == Channel::Outfit) {
            // A shield may have displaced a weapon or spell. Restore the exact
            // weapon copy; leave legitimately removed items absent.
            for (const auto& key : snapshot.worn) {
                auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(key.form);
                if (object && object->IsWeapon() && Find(key)) restored = Equip(key) && restored;
            }
            if (auto* manager = RE::ActorEquipManager::GetSingleton()) {
                for (std::size_t i = 0; i < 2; ++i) {
                    if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(snapshot.hands[i])) {
                        if (Actor()->HasSpell(spell)) manager->EquipSpell(Actor(), spell,
                            RE::TESForm::LookupByID<RE::BGSEquipSlot>(i == 0 ? 0x13F42 : 0x13F43));
                    }
                }
            }
        }
        return restored;
    }

    bool Equipment::Apply(Channel channel, const std::vector<RE::TESForm*>& items)
    {
        auto* actor = Actor();
        if (!actor || !actor->Is3DLoaded() || _changing || !RE::ActorEquipManager::GetSingleton()) return false;
        for (auto* item : items) if (!item || !item->IsArmor()) return false;
        actor->InitInventoryIfRequired();
        if (!actor->GetInventoryChanges()) return false;
        _changing = true;
        const auto index = static_cast<std::size_t>(channel);
        auto& state = _state[index];
        const auto originalState = state;
        const auto before = Capture();
        if (!state.hasBaseline) { state.baseline = before; state.hasBaseline = true; }
        std::vector<Instance> desired;
        for (auto* form : items) {
            auto* armor = form->As<RE::TESObjectARMO>();
            state.slots |= armor->GetSlotMask().underlying();
            auto it = std::find_if(state.supplied.begin(), state.supplied.end(), [&](const Instance& key) {
                return key.form == form->GetFormID() && Find(key);
            });
            if (it != state.supplied.end()) { desired.push_back(*it); continue; }
            auto* extra = NewItemData();
            if (!extra) {
                if (Prune(channel, originalState.supplied)) state = originalState;
                FinishChange();
                return false;
            }
            const auto key = Identify(armor, extra);
            state.supplied.push_back(key); // publish ownership before synchronous events
            actor->AddObjectToContainer(armor, extra, 1, nullptr);
            desired.push_back(key);
        }
        bool applied = true;
        for (const auto& key : before.worn) {
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(key.form);
            if (Affects(channel, armor) && !ContainsCopy(desired, key) && !Unequip(key)) { applied = false; break; }
        }
        if (applied) for (const auto& key : desired) if (!Equip(key)) { applied = false; break; }
        std::vector<Instance> keep = applied ? desired : originalState.supplied;
        const bool restored = applied || Restore(channel, before);
        if (_preview[index]) {
            const auto& previous = _preview[index]->original.supplied;
            keep.insert(keep.end(), previous.begin(), previous.end());
        }
        const bool cleaned = Prune(channel, keep);
        if (!applied && restored && cleaned) state = originalState;
        FinishChange();
        return applied;
    }

    bool Equipment::EndPreview(Channel channel, bool confirm)
    {
        const auto index = static_cast<std::size_t>(channel);
        if (!_preview[index]) return true;
        if (_changing || !Actor()) return false;
        _changing = true;
        if (!confirm && !Restore(channel, _preview[index]->worn)) { FinishChange(); return false; }
        const auto keep = confirm ? Capture().worn : _preview[index]->original.supplied;
        if (!Prune(channel, keep)) { FinishChange(); return false; }
        if (!confirm) _state[index] = _preview[index]->original;
        _preview[index].reset();
        FinishChange();
        return true;
    }

    bool Equipment::RestoreBaseline(Channel channel, bool forget)
    {
        if (!Actor() || _changing) return false;
        if (!EndPreview(channel, false)) return false;
        auto& state = _state[static_cast<std::size_t>(channel)];
        if (!state.hasBaseline) return true;
        _changing = true;
        const bool restored = Restore(channel, state.baseline) && Prune(channel, {});
        if (restored && forget) state = {};
        FinishChange();
        return restored;
    }

    void Equipment::Clear() { _state = {}; _preview = {}; _changing = false; }
}
