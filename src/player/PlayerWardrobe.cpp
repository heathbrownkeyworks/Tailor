#include "player/PlayerWardrobe.h"
#include "events/SituationHelmets.h"
#include "player/PlayerHands.h"
#include "player/PlayerSituationPolicy.h"

#include <algorithm>

namespace Tailor::Player
{
    namespace
    {
        struct Instance
        {
            RE::TESObjectARMO* armor = nullptr;
            RE::ExtraDataList* extra = nullptr;
        };

        // Non-playable armor (skin, body-mod parts) is never the player's gear.
        bool IsGear(RE::TESObjectARMO* armor)
        {
            return armor && (armor->formFlags & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
        }

        bool IsShield(RE::TESObjectARMO* armor)
        {
            return armor && armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        }

        bool Contains(const std::vector<CopyKey>& keys, const CopyKey& key)
        {
            return std::ranges::find(keys, key) != keys.end();
        }

        // Every armor copy that has extra data, optionally of one armor. Plain copies
        // without extra data are only counted by the engine.
        std::vector<Instance> Copies(RE::Actor* actor, RE::TESObjectARMO* only = nullptr)
        {
            std::vector<Instance> result;
            auto* inventory = actor ? actor->GetInventoryChanges() : nullptr;
            if (!inventory || !inventory->entryList) return result;
            for (auto* entry : *inventory->entryList) {
                auto* armor = entry && entry->object ? entry->object->As<RE::TESObjectARMO>() : nullptr;
                if (!armor || (only && armor != only) || !entry->extraLists) continue;
                for (auto* extra : *entry->extraLists) {
                    if (extra) result.push_back({armor, extra});
                }
            }
            return result;
        }

        std::optional<CopyKey> KeyOf(const Instance& copy)
        {
            auto* id = copy.extra ? copy.extra->GetByType<RE::ExtraUniqueID>() : nullptr;
            if (!id) return std::nullopt;
            return CopyKey{copy.armor->GetFormID(), id->baseID, id->uniqueID};
        }

        // Gives a copy an ExtraUniqueID when it has none, as HeadwearPreview does, so
        // it can be found again as itself after any number of native calls.
        CopyKey Tag(RE::Actor* actor, const Instance& copy)
        {
            if (const auto key = KeyOf(copy)) return *key;
            auto* id = new RE::ExtraUniqueID(actor->GetFormID(), actor->GetInventoryChanges()->GetNextUniqueID());
            copy.extra->Add(id);
            actor->AddChange(RE::TESObjectREFR::ChangeFlags::kInventory);
            return {copy.armor->GetFormID(), id->baseID, id->uniqueID};
        }

        // Native calls can rebuild extra lists: keep keys, never old pointers. The wig Tailor
        // keeps on the player is not gear: it follows the wig rules.
        std::vector<CopyKey> TagWornGear(RE::Actor* actor, std::uint32_t keptWig)
        {
            std::vector<CopyKey> keys;
            for (const auto& copy : Copies(actor)) {
                if (copy.extra->GetWorn() && IsGear(copy.armor) && copy.armor->GetFormID() != keptWig) keys.push_back(Tag(actor, copy));
            }
            return keys;
        }

        // A helmet Hide Helmets took off is still part of the player's own gear: each copy it has off that
        // the player still has joins `gear`, once.
        void AddTakenHelmets(RE::Actor* player, std::vector<CopyKey>& gear)
        {
            for (const auto& copy : SituationHelmets::GetSingleton().Copies(player->GetFormID())) {
                if (Tailor::Wigs::HeadwearCopyState(player, copy) == Tailor::Situations::CopyState::Gone) continue;
                const CopyKey key{copy.form, copy.owner, copy.unique};
                if (!Contains(gear, key)) gear.push_back(key);
            }
        }

        Instance Find(RE::Actor* actor, const CopyKey& key)
        {
            if (!key.unique) return {};
            for (const auto& copy : Copies(actor)) {
                if (const auto found = KeyOf(copy); found && *found == key) return copy;
            }
            return {};
        }

        bool IsWorn(RE::Actor* actor, const CopyKey& key)
        {
            const auto copy = Find(actor, key);
            return copy.extra && copy.extra->GetWorn();
        }

        void Equip(RE::Actor* actor, const CopyKey& key)
        {
            const auto copy = Find(actor, key);
            if (copy.extra && !copy.extra->GetWorn()) {
                RE::ActorEquipManager::GetSingleton()->EquipObject(actor, copy.armor, copy.extra, 1, nullptr, false, false, false, true);
            }
        }

        void Unequip(RE::Actor* actor, const CopyKey& key)
        {
            const auto copy = Find(actor, key);
            if (copy.extra && copy.extra->GetWorn()) {
                RE::ActorEquipManager::GetSingleton()->UnequipObject(actor, copy.armor, copy.extra, 1, nullptr, false, false, false, true);
            }
        }

        // Removes exactly this copy, worn or not. A copy the player sold, dropped or
        // stored is simply gone, and nothing else is taken in its place.
        void TakeBack(RE::Actor* actor, const CopyKey& key)
        {
            Unequip(actor, key);
            const auto copy = Find(actor, key);
            if (copy.armor) actor->RemoveItem(copy.armor, 1, RE::ITEM_REMOVE_REASON::kRemove, copy.extra, nullptr);
        }

        // Enchanted beats tempered beats plain, so the player's own work applies.
        int Rank(RE::ExtraDataList* extra)
        {
            int rank = 0;
            if (extra->HasType<RE::ExtraEnchantment>()) rank += 2;
            if (const auto* health = extra->GetByType<RE::ExtraHealth>(); health && health->health > 1.0f) rank += 1;
            return rank;
        }

        std::int32_t Count(RE::Actor* actor, RE::TESObjectARMO* armor)
        {
            const auto counts = actor->GetInventoryCounts();
            const auto it = counts.find(armor);
            return it == counts.end() ? 0 : it->second;
        }

        // After the equip manager put on a copy of its choosing: that copy, tagged.
        std::optional<CopyKey> TagWorn(RE::Actor* actor, RE::TESObjectARMO* armor, const std::vector<CopyKey>& taken)
        {
            for (const auto& copy : Copies(actor, armor)) {
                if (!copy.extra->GetWorn()) continue;
                const auto key = Tag(actor, copy);
                if (!Contains(taken, key)) return key;
            }
            return std::nullopt;
        }
    }

    PlayerWardrobe& PlayerWardrobe::GetSingleton()
    {
        static PlayerWardrobe singleton;
        return singleton;
    }

    bool PlayerWardrobe::DressItems(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items)
    {
        // Own Gear is what the player wore before Tailor dressed them. Recorded now, it replaces
        // the poll's note from before any beast form.
        if (!_state.ownGearRecorded) {
            _state.ownGear = TagWornGear(player, _state.keptWig);
            AddTakenHelmets(player, _state.ownGear);
            _state.ownGearRecorded = true;
            _state.ownGearNote.clear();
        }
        const auto worn = TagWornGear(player, _state.keptWig);
        const bool shield = std::ranges::any_of(items, IsShield);
        if (shield) {
            for (const auto& item : TakeHandsForShield(player)) {
                if (std::ranges::find(_state.displaced, item) == _state.displaced.end()) _state.displaced.push_back(item);
            }
        }

        bool complete = true;
        std::vector<CopyKey> chosen;
        for (auto* armor : items) {
            if (!armor) continue;
            const auto form = armor->GetFormID();
            if (std::ranges::any_of(chosen, [form](const CopyKey& key) { return key.form == form; })) continue;
            std::optional<CopyKey> pick;
            // 1. A copy already worn, the player's or Tailor's.
            for (const auto& key : worn) {
                if (key.form == form) { pick = key; break; }
            }
            // 2. The copy the player wore in Own Gear.
            if (!pick) {
                for (const auto& key : _state.ownGear) {
                    if (key.form == form && Find(player, key).armor) { pick = key; break; }
                }
            }
            // 3. The player's own best copy with extra data.
            if (!pick) {
                Instance best;
                int bestRank = -1;
                for (const auto& copy : Copies(player, armor)) {
                    if (const auto key = KeyOf(copy); key && Contains(_state.tailorCopies, *key)) continue;
                    if (const int rank = Rank(copy.extra); rank > bestRank) {
                        best = copy;
                        bestRank = rank;
                    }
                }
                if (best.extra) {
                    // One list can hold a stack; tag only the copy that ends up worn.
                    RE::ActorEquipManager::GetSingleton()->EquipObject(player, armor, best.extra, 1, nullptr, false, false, false, true);
                    pick = TagWorn(player, armor, chosen);
                }
            }
            // 4. A Tailor copy already in the inventory.
            if (!pick) {
                for (const auto& key : _state.tailorCopies) {
                    if (key.form == form && Find(player, key).armor) { pick = key; break; }
                }
            }
            if (pick) {
                Equip(player, *pick);
            } else if (Count(player, armor) > 0) {
                // Plain copies carry no extra data: the equip manager picks one.
                RE::ActorEquipManager::GetSingleton()->EquipObject(player, armor, nullptr, 1, nullptr, false, false, false, true);
                pick = TagWorn(player, armor, chosen);
            } else {
                // 5. The player owns none: add one, tagged as Tailor's.
                player->AddObjectToContainer(armor, nullptr, 1, nullptr);
                RE::ActorEquipManager::GetSingleton()->EquipObject(player, armor, nullptr, 1, nullptr, false, false, false, true);
                pick = TagWorn(player, armor, chosen);
                if (pick) {
                    _state.tailorCopies.push_back(*pick);
                } else {
                    // Refused (race, a locked slot): the copy goes straight back.
                    player->RemoveItem(armor, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                }
            }
            if (pick) chosen.push_back(*pick);
            if (!pick || !IsWorn(player, *pick)) {
                complete = false;
                logger::warn("PlayerWardrobe: could not put on {:08X}", form);
            }
        }

        // Everything else the player wore comes off; the check after dressing looks at these again.
        std::vector<CopyKey> takenOff;
        for (const auto& key : worn) {
            if (Contains(chosen, key)) continue;
            Unequip(player, key);
            takenOff.push_back(key);
        }
        // Tailor's copies the new look does not use go back. During a preview the
        // copies from before it stay, so Cancel can put them back on.
        std::vector<CopyKey> kept;
        for (const auto& key : _state.tailorCopies) {
            if (Contains(chosen, key) || (_preview && Contains(_preview->state.tailorCopies, key))) {
                kept.push_back(key);
            } else {
                TakeBack(player, key);
            }
        }
        _state.tailorCopies = std::move(kept);
        // Hands a shield took come back once no shield is worn.
        if (!shield && !_state.displaced.empty()) _state.displaced = ReturnHands(player, _state.displaced);
        _chosen = std::move(chosen);
        _takenOff = std::move(takenOff);
        return complete;
    }

    bool PlayerWardrobe::Dress(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items, int outfitId,
        std::vector<PieceKey> pieces)
    {
        std::lock_guard lock(_mutex);
        if (!player) return false;
        const bool complete = DressItems(player, items);
        _state.wornOutfitId = outfitId;
        _state.wornOutfitPieces = std::move(pieces);
        return complete;
    }

    bool PlayerWardrobe::RestoreOwnGear(RE::Actor* player)
    {
        std::lock_guard lock(_mutex);
        if (!player) return false;
        _state.wornOutfitId = 0;
        _state.wornOutfitPieces.clear();
        // Tailor never dressed the player: what they wear is their own.
        if (!_state.ownGearRecorded) return true;
        for (const auto& key : TagWornGear(player, _state.keptWig)) {
            if (!Contains(_state.ownGear, key)) Unequip(player, key);
        }
        std::vector<CopyKey> kept;
        for (const auto& key : _state.tailorCopies) {
            if (_preview && Contains(_preview->state.tailorCopies, key)) kept.push_back(key);
            else TakeBack(player, key);
        }
        _state.tailorCopies = std::move(kept);
        bool complete = true;
        for (const auto& key : _state.ownGear) {
            if (!Find(player, key).armor) continue;  // sold or dropped since
            Equip(player, key);
            if (!IsWorn(player, key)) complete = false;
        }
        if (!_state.displaced.empty()) _state.displaced = ReturnHands(player, _state.displaced);
        _state.ownGear.clear();
        _state.ownGearRecorded = false;
        _chosen.clear();
        _takenOff.clear();
        return complete;
    }

    void PlayerWardrobe::NoteOwnGear(RE::Actor* player)
    {
        std::lock_guard lock(_mutex);
        if (!player || !PlayerNotesOwnGear(_preview.has_value(), _state.wornOutfitId, _state.ownGearRecorded)) return;
        _state.ownGearNote = TagWornGear(player, _state.keptWig);
        AddTakenHelmets(player, _state.ownGearNote);
    }

    bool PlayerWardrobe::AdoptOwnGearNote()
    {
        std::lock_guard lock(_mutex);
        if (!PlayerAdoptsOwnGearNote(_state.wornOutfitId, _state.ownGearRecorded, !_state.ownGearNote.empty())) return false;
        _state.ownGear = std::move(_state.ownGearNote);
        _state.ownGearNote.clear();
        _state.ownGearRecorded = true;
        return true;
    }

    void PlayerWardrobe::RetireWig(RE::Actor* player, std::uint32_t form)
    {
        if (!form) return;
        std::vector<CopyKey> worn;
        for (const auto& copy : Copies(player)) {
            if (copy.armor->GetFormID() == form && copy.extra->GetWorn()) worn.push_back(Tag(player, copy));
        }
        for (const auto& key : worn) Unequip(player, key);
        std::vector<CopyKey> kept;
        for (const auto& key : _state.wigCopies) {
            if (key.form == form) TakeBack(player, key);
            else kept.push_back(key);
        }
        _state.wigCopies = std::move(kept);
    }

    bool PlayerWardrobe::SetWig(RE::Actor* player, RE::TESObjectARMO* wig, bool wear)
    {
        std::lock_guard lock(_mutex);
        if (!player || !wig) return false;
        const auto form = wig->GetFormID();
        if (_state.keptWig != form) RetireWig(player, _state.keptWig);
        _state.keptWig = form;
        if (!wear) {
            // Hidden under headgear: off, but kept, copies and all, for when the headgear comes off.
            std::vector<CopyKey> worn;
            for (const auto& copy : Copies(player, wig)) {
                if (copy.extra->GetWorn()) worn.push_back(Tag(player, copy));
            }
            for (const auto& key : worn) Unequip(player, key);
            return true;
        }
        const auto worn = [&] {
            return std::ranges::any_of(Copies(player, wig), [](const Instance& copy) { return copy.extra->GetWorn(); });
        };
        if (worn()) return true;
        if (Count(player, wig) > 0) {
            // A copy the player owns, or Tailor's from before: the equip manager picks it.
            RE::ActorEquipManager::GetSingleton()->EquipObject(player, wig, nullptr, 1, nullptr, false, false, false, true);
        } else {
            // The player owns none: add one, tagged as Tailor's.
            player->AddObjectToContainer(wig, nullptr, 1, nullptr);
            RE::ActorEquipManager::GetSingleton()->EquipObject(player, wig, nullptr, 1, nullptr, false, false, false, true);
            if (const auto key = TagWorn(player, wig, {})) {
                _state.wigCopies.push_back(*key);
            } else {
                // Refused (race, a locked slot): the copy goes straight back.
                player->RemoveItem(wig, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            }
        }
        if (worn()) return true;
        logger::warn("PlayerWardrobe: could not put on wig {:08X}", form);
        return false;
    }

    void PlayerWardrobe::RemoveWig(RE::Actor* player)
    {
        std::lock_guard lock(_mutex);
        if (player) RetireWig(player, _state.keptWig);
        _state.keptWig = 0;
    }

    bool PlayerWardrobe::RemoveWigs(RE::Actor* player, const std::vector<std::uint32_t>& wigForms)
    {
        std::lock_guard lock(_mutex);
        if (!player) return false;
        std::vector<CopyKey> worn;
        for (const auto& copy : Copies(player)) {
            if (copy.extra->GetWorn() && std::ranges::find(wigForms, copy.armor->GetFormID()) != wigForms.end()) {
                worn.push_back(Tag(player, copy));
            }
        }
        for (const auto& key : worn) Unequip(player, key);
        for (const auto& key : _state.wigCopies) TakeBack(player, key);
        _state.wigCopies.clear();
        _state.keptWig = 0;
        return std::ranges::none_of(worn, [&](const CopyKey& key) { return IsWorn(player, key); });
    }

    bool PlayerWardrobe::BeginPreview(RE::Actor* player)
    {
        std::lock_guard lock(_mutex);
        if (!player || _preview) return false;
        _preview = Snapshot{_state, TagWornGear(player, _state.keptWig)};
        return true;
    }

    bool PlayerWardrobe::PreviewItems(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items)
    {
        std::lock_guard lock(_mutex);
        if (!player || !_preview) return false;
        return DressItems(player, items);
    }

    bool PlayerWardrobe::EndPreview(RE::Actor* player, bool keep, int outfitId, std::vector<PieceKey> pieces)
    {
        std::lock_guard lock(_mutex);
        if (!_preview) return true;
        auto snapshot = std::move(*_preview);
        _preview.reset();
        if (!player) return false;
        if (keep) {
            // The previewed look is now the outfit; copies it does not use go back.
            std::vector<CopyKey> kept;
            for (const auto& key : _state.tailorCopies) {
                if (Contains(_chosen, key)) kept.push_back(key);
                else TakeBack(player, key);
            }
            _state.tailorCopies = std::move(kept);
            _state.wornOutfitId = outfitId;
            _state.wornOutfitPieces = std::move(pieces);
            return true;
        }
        // Put back exactly what was worn before the preview.
        for (const auto& key : TagWornGear(player, _state.keptWig)) {
            if (!Contains(snapshot.worn, key)) Unequip(player, key);
        }
        for (const auto& key : _state.tailorCopies) {
            if (!Contains(snapshot.state.tailorCopies, key)) TakeBack(player, key);
        }
        bool complete = true;
        for (const auto& key : snapshot.worn) {
            if (!Find(player, key).armor) continue;
            Equip(player, key);
            if (!IsWorn(player, key)) complete = false;
        }
        // Hands a preview shield took go back now; hands taken before it stay taken.
        std::vector<DisplacedItem> previewHands;
        for (const auto& item : _state.displaced) {
            if (std::ranges::find(snapshot.state.displaced, item) == snapshot.state.displaced.end()) previewHands.push_back(item);
        }
        if (!previewHands.empty()) (void)ReturnHands(player, previewHands);
        // The wig has rules of its own; an outfit preview never changes it.
        snapshot.state.keptWig = _state.keptWig;
        snapshot.state.wigCopies = _state.wigCopies;
        _state = std::move(snapshot.state);
        _chosen.clear();
        _takenOff.clear();
        return complete;
    }

    void PlayerWardrobe::DropPreview()
    {
        std::lock_guard lock(_mutex);
        _preview.reset();
    }

    bool PlayerWardrobe::IsPreviewing() const
    {
        std::lock_guard lock(_mutex);
        return _preview.has_value();
    }

    int PlayerWardrobe::WornOutfitId() const
    {
        std::lock_guard lock(_mutex);
        return _state.wornOutfitId;
    }

    WardrobeState PlayerWardrobe::State() const
    {
        std::lock_guard lock(_mutex);
        return _state;
    }

    std::vector<CopyKey> PlayerWardrobe::TakenOff() const
    {
        std::lock_guard lock(_mutex);
        return _takenOff;
    }

    std::vector<std::uint32_t> PlayerWardrobe::WornForms(RE::Actor* player, const std::vector<CopyKey>& copies) const
    {
        std::lock_guard lock(_mutex);
        std::vector<std::uint32_t> forms;
        if (!player) return forms;
        for (const auto& key : copies) {
            // The wig rules put the kept wig on and take it off, so it being worn is never an outfit failure.
            if (key.form != _state.keptWig && IsWorn(player, key)) forms.push_back(key.form);
        }
        return forms;
    }

    std::vector<std::uint32_t> PlayerWardrobe::ChosenForms() const
    {
        std::lock_guard lock(_mutex);
        std::vector<std::uint32_t> forms;
        for (const auto& key : _chosen) forms.push_back(key.form);
        return forms;
    }

    void PlayerWardrobe::SetState(WardrobeState state)
    {
        std::lock_guard lock(_mutex);
        _state = std::move(state);
        _preview.reset();
        _chosen.clear();
        _takenOff.clear();
    }

    void PlayerWardrobe::Clear()
    {
        SetState({});
    }
}
