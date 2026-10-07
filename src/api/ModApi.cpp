#include "api/ModApi.h"

#include "api/ModApiPolicy.h"
#include "api/ModOverrides.h"
#include "api/TailorInterface1.h"
#include "events/CellHandler.h"
#include "events/SituationHandler.h"
#include "outfit/ArmorItem.h"
#include "outfit/OutfitArmorType.h"
#include "outfit/OutfitLibrary.h"
#include "outfit/OutfitManager.h"
#include "outfit/OutfitNamePolicy.h"
#include "outfit/OutfitStore.h"
#include "ui/TailorUI.h"
#include "wig/WigAssignments.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <random>
#include <unordered_map>

namespace Tailor::Api
{
    namespace
    {
        using Outfits::NameKey;
        using Outfits::SameName;

        // The outfit of this name, as a copy taken under the store's lock. The name is matched as ApiName gives it,
        // as the store keeps it. An empty name is no outfit here, though the store would match a legacy outfit that
        // has none.
        std::optional<CustomOutfit> Outfit(std::string_view name)
        {
            std::optional<CustomOutfit> found;
            const auto key = ApiName(name);
            if (!key.empty()) found = OutfitStore::GetSingleton().FindByName(key);
            if (!found) logger::info("Tailor API: no outfit named '{}'", key);
            return found;
        }

        std::vector<RE::TESObjectARMO*> Resolved(const CustomOutfit& outfit)
        {
            std::vector<RE::TESObjectARMO*> armors;
            for (const auto& item : outfit.items) {
                if (auto* armor = item.Resolve()) armors.push_back(armor);
            }
            return armors;
        }

        // The category Tailor's screens show under this name, as a copy taken under the library's lock. The name is
        // matched as ApiName gives it.
        std::optional<OutfitCategory> Category(std::string_view name)
        {
            auto& library = OutfitLibrary::GetSingleton();
            std::optional<OutfitCategory> found;
            const auto key = ApiName(name);
            if (!key.empty()) {
                if (const auto id = library.FindCategoryByDisplayName(key)) {
                    for (auto& category : library.Snapshot()) {
                        if (category.id == *id) {
                            found = std::move(category);
                            break;
                        }
                    }
                }
            }
            if (!found) logger::info("Tailor API: no category named '{}'", key);
            return found;
        }

        // The names of these outfits, in order, that fit the gender filter.
        std::vector<std::string> Names(const std::vector<int>& ids, int gender)
        {
            std::vector<std::string> names;
            for (const int id : ids) {
                const auto outfit = OutfitStore::GetSingleton().GetOutfitCopy(id);
                if (outfit && FitsGenderFilter(outfit->sex, gender)) names.push_back(outfit->name);
            }
            return names;
        }

        // An armor as an outfit holds it: its local FormID and plugin. One made at run time has no plugin.
        std::optional<ArmorItem> ItemOf(RE::TESObjectARMO* armor)
        {
            if (!armor) {
                logger::info("Tailor API: no armor given");
                return std::nullopt;
            }
            const auto* file = armor->GetFile(0);
            if (!file) {
                logger::info("Tailor API: armor {:08X} was made at run time and has no plugin", armor->GetFormID());
                return std::nullopt;
            }
            return ArmorItem{armor->GetLocalFormID(), std::string(file->GetFilename()), SanitizeUtf8(armor->GetName())};
        }

        bool SameItem(const ArmorItem& a, const ArmorItem& b)
        {
            return a.formId == b.formId && SameName(a.plugin, b.plugin);
        }

        // An open Tailor shows the change. Both run on the game thread, where the UI is published from.
        void RefreshOutfits()
        {
            SKSE::GetTaskInterface()->AddTask([] {
                auto& ui = TailorUI::GetSingleton();
                if (!ui.IsOpen()) return;
                ui.SendOutfits();
                ui.SendCategories();
            });
        }

        void RefreshLibrary()
        {
            SKSE::GetTaskInterface()->AddTask([] {
                auto& ui = TailorUI::GetSingleton();
                if (!ui.IsOpen()) return;
                ui.SendOutfits();
                ui.SendCategories();
                ui.SendAllCategories();
            });
        }

        // Whoever wears the outfit is dressed again, on the game thread.
        void RefitOutfits(int outfitId)
        {
            SKSE::GetTaskInterface()->AddTask([outfitId] { OutfitManager::GetSingleton().RefitOutfits({outfitId}); });
        }

        bool HasActor(RE::Actor* actor)
        {
            if (!actor) {
                logger::info("Tailor API: no actor given");
                return false;
            }
            // Tailor never handles children.
            if (actor->IsChild()) {
                logger::info("Tailor API: {:08X} is a child; Tailor doesn't handle children", actor->GetFormID());
                return false;
            }
            return true;
        }

        // The player, or an NPC Tailor can dress: alive, not a child, not a creature. HasActor refuses a child first.
        bool Overridable(RE::Actor* actor)
        {
            if (!HasActor(actor)) return false;
            if (actor->IsPlayerRef() || OutfitManager::GetSingleton().IsValidTarget(actor)) return true;
            logger::info("Tailor API: Tailor can't dress {:08X}: dead, or not a person", actor->GetFormID());
            return false;
        }

        // An outfit the actor can wear from these, picked at random; 0 when none fits.
        int RandomFitting(const std::vector<int>& ids, RE::Actor* actor)
        {
            const int sex = OutfitManager::GetNpcSex(actor);
            std::vector<int> pool;
            for (const int id : ids) {
                if (OutfitStore::GetSingleton().Fits(id, sex)) pool.push_back(id);
            }
            if (pool.empty()) return 0;
            static std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<std::size_t> pick(0, pool.size() - 1);
            return pool[pick(rng)];
        }

        // Tailor decides again, asking the override first: the player through their reconcile, an NPC
        // through the situation flow (which dresses anyone it has an override for). Each caller queues
        // this task on the game thread. Someone open in Tailor's outfit editor is dressed by the editor
        // when it closes.
        auto Dress(RE::ActorHandle handle)
        {
            return [handle] {
                const auto ref = handle.get();
                auto* actor = ref.get();
                if (!actor || OutfitManager::GetSingleton().IsCreateSessionActor(actor)) return;
                if (actor->IsPlayerRef()) OutfitManager::GetSingleton().ReconcilePlayer();
                else if (actor->Is3DLoaded()) SituationHandler::GetSingleton()->ForceApplyForSituation(actor);
            };
        }

        // Set once Tailor has loaded the outfit store and the library at kDataLoaded. Until then the stores are
        // empty, and saving them would write an empty library over the player's files.
        std::atomic<bool> sLibraryLoaded{false};

        // The building functions refuse until then; an SKSE plugin is the only caller that can come that early.
        bool LibraryLoaded(const char* function)
        {
            if (sLibraryLoaded.load()) return true;
            logger::info("Tailor API: {} called before Tailor loaded its library (call after kDataLoaded)", function);
            return false;
        }

        // Saves are off for outfits or categories this session (a file Tailor couldn't fully read at startup): the
        // building functions change nothing, as Tailor's own screens don't.
        bool LibraryWritable(const char* function)
        {
            if (OutfitStore::GetSingleton().SaveAllowed() && OutfitLibrary::GetSingleton().SaveAllowed()) return true;
            logger::info("Tailor API: {} refused; Tailor couldn't fully read outfits.json or library.json, so it won't change them this session", function);
            return false;
        }

        // The outfit's current name; "" for none, or for an outfit that no longer exists.
        std::string OutfitName(int id)
        {
            const auto outfit = id > 0 ? OutfitStore::GetSingleton().GetOutfitCopy(id) : std::nullopt;
            return outfit ? outfit->name : std::string{};
        }

        // Change events: what each watched person was last seen wearing and where, and the C++ listeners. Both
        // are touched only on the game thread: by the poll, by a load or a new game, and by the interface's
        // AddListener and RemoveListener, which callers must call there.
        std::unordered_map<RE::FormID, Seen> sSeen;
        std::vector<TailorAPI::ITailorListener1*> sListeners;

        // An override set on someone not watched yet: what they wear now is recorded as last seen, as the poll would
        // record it (nothing while Tailor is still putting their outfit back on), so the override going on is told.
        // Their situation is left for the poll, which records it without an event. Called on the game thread, by the
        // OverrideWith functions before they set the override.
        void WatchForOverride(RE::Actor* actor)
        {
            const auto [seen, added] = sSeen.try_emplace(actor->GetFormID());
            if (!added) return;
            auto* situations = SituationHandler::GetSingleton();
            if (!situations->OutfitPending(actor)) seen->second.outfit = situations->CurrentOutfitId(actor);
        }

        // Papyrus hears a change as an SKSE mod event sent by the actor.
        void SendModEvent(const char* name, RE::Actor* actor, const std::string& text)
        {
            if (auto* source = SKSE::GetModCallbackEventSource()) {
                const SKSE::ModCallbackEvent event{RE::BSFixedString(name), RE::BSFixedString(text.c_str()), 0.0f, actor};
                source->SendEvent(std::addressof(event));
            }
        }
    }

    float GetApiVersion()
    {
        return 1.0f;
    }

    bool DoesOutfitExist(std::string_view outfit)
    {
        return Outfit(outfit).has_value();
    }

    int GetOutfitGender(std::string_view outfit)
    {
        const auto found = Outfit(outfit);
        return found ? static_cast<int>(found->sex) : -2;
    }

    std::vector<RE::TESObjectARMO*> GetOutfitArmors(std::string_view outfit)
    {
        const auto found = Outfit(outfit);
        return found ? Resolved(*found) : std::vector<RE::TESObjectARMO*>{};
    }

    bool OutfitHasKeyword(std::string_view outfit, RE::BGSKeyword* keyword)
    {
        const auto found = Outfit(outfit);
        if (!found) return false;
        if (!keyword) {
            logger::info("Tailor API: no keyword given");
            return false;
        }
        return std::ranges::any_of(Resolved(*found), [keyword](RE::TESObjectARMO* armor) { return armor->HasKeyword(keyword); });
    }

    bool OutfitUsesSlot(std::string_view outfit, int slot)
    {
        const auto found = Outfit(outfit);
        if (!found) return false;
        const auto bit = SlotBit(slot);
        if (!bit) {
            logger::info("Tailor API: {} is not a body slot (30-61)", slot);
            return false;
        }
        return std::ranges::any_of(Resolved(*found), [bit](RE::TESObjectARMO* armor) { return (armor->GetSlotMask().underlying() & bit) != 0; });
    }

    bool IsOutfitInCategory(std::string_view outfit, std::string_view category)
    {
        const auto found = Outfit(outfit);
        if (!found) return false;
        const auto target = Category(category);
        if (!target) return false;
        return std::ranges::find(target->outfitIds, found->id) != target->outfitIds.end();
    }

    bool DoesCategoryExist(std::string_view category)
    {
        return Category(category).has_value();
    }

    std::vector<std::string> GetOutfitsByCategory(std::string_view category, int gender)
    {
        const auto target = Category(category);
        return target ? Names(target->outfitIds, gender) : std::vector<std::string>{};
    }

    int GetOutfitCount(std::string_view category, int gender)
    {
        return static_cast<int>(GetOutfitsByCategory(category, gender).size());
    }

    std::vector<std::string> GetAdventuringOutfits(std::string_view armorType, int gender)
    {
        auto& library = OutfitLibrary::GetSingleton();
        auto ids = library.GetSituationOutfitIds("adventuring");
        const auto key = NameKey(armorType);
        if (!key.empty()) {
            const auto type = ParseOutfitArmorType(key);
            if (type == OutfitArmorType::Any) {
                logger::info("Tailor API: '{}' is not an armor type (clothing, light or heavy)", armorType);
                return {};
            }
            ids = library.FilterByArmorType(ids, type);
        }
        return Names(ids, gender);
    }

    std::vector<std::string> GetOutfitsByArmorKeyword(RE::BGSKeyword* keyword, int gender)
    {
        std::vector<std::string> names;
        if (!keyword) {
            logger::info("Tailor API: no keyword given");
            return names;
        }
        for (const auto& outfit : OutfitStore::GetSingleton().Snapshot()) {
            if (!FitsGenderFilter(outfit.sex, gender)) continue;
            if (std::ranges::any_of(Resolved(outfit), [keyword](RE::TESObjectARMO* armor) { return armor->HasKeyword(keyword); })) {
                names.push_back(outfit.name);
            }
        }
        return names;
    }

    bool CreateOutfit(std::string_view outfit, std::string_view category, int gender)
    {
        if (!LibraryLoaded("CreateOutfit")) return false;
        if (!LibraryWritable("CreateOutfit")) return false;
        if (gender < -1 || gender > 1) {
            logger::info("Tailor API: {} is not a gender (-1 unisex, 0 male, 1 female); outfit not created", gender);
            return false;
        }
        std::optional<OutfitCategory> target;
        if (!ApiName(category).empty()) {
            target = Category(category);
            if (!target) return false;
        }

        // The store refuses an empty or taken name and says why.
        auto& store = OutfitStore::GetSingleton();
        const int id = store.AddOutfit(ApiName(outfit), {}, static_cast<OutfitSex>(gender));
        if (id == 0) return false;
        // A new outfit that couldn't be saved is taken back: false means nothing was created.
        if (!store.Save()) {
            store.DiscardNewOutfit(id);
            return false;
        }

        auto& library = OutfitLibrary::GetSingleton();
        if (target && library.AddOutfitToCategory(target->id, id)) {
            // Taken back too, rather than left empty in a pool, where it could be picked and dress someone in nothing.
            if (!library.Save()) {
                library.RemoveOutfitFromCategory(target->id, id);
                store.DiscardNewOutfit(id);
                if (!store.Save()) {
                    logger::error("Tailor API: CreateOutfit could not save outfits.json again after taking back '{}'; until the next save there, the file still lists it, with no category", ApiName(outfit));
                }
                return false;
            }
            RefreshLibrary();
        } else {
            RefreshOutfits();
        }
        return true;
    }

    bool AddArmorToOutfit(std::string_view outfit, RE::TESObjectARMO* armor)
    {
        if (!LibraryLoaded("AddArmorToOutfit")) return false;
        if (!LibraryWritable("AddArmorToOutfit")) return false;
        const auto item = ItemOf(armor);
        if (!item) return false;
        auto found = Outfit(outfit);
        if (!found) return false;
        if (std::ranges::any_of(found->items, [&](const ArmorItem& have) { return SameItem(have, *item); })) return true;

        found->items.push_back(*item);
        auto& store = OutfitStore::GetSingleton();
        if (!store.UpdateOutfit(found->id, found->name, found->items, found->sex)) return false;
        store.Save();
        RefreshOutfits();
        RefitOutfits(found->id);
        return true;
    }

    bool RemoveArmorFromOutfit(std::string_view outfit, RE::TESObjectARMO* armor)
    {
        if (!LibraryLoaded("RemoveArmorFromOutfit")) return false;
        if (!LibraryWritable("RemoveArmorFromOutfit")) return false;
        const auto item = ItemOf(armor);
        if (!item) return false;
        auto found = Outfit(outfit);
        if (!found) return false;
        const auto removed = std::erase_if(found->items, [&](const ArmorItem& have) { return SameItem(have, *item); });
        if (removed == 0) {
            logger::info("Tailor API: outfit '{}' does not have armor {:08X}", found->name, armor->GetFormID());
            return false;
        }

        auto& store = OutfitStore::GetSingleton();
        if (!store.UpdateOutfit(found->id, found->name, found->items, found->sex)) return false;
        store.Save();
        RefreshOutfits();
        RefitOutfits(found->id);
        return true;
    }

    bool CreateCustomCategory(std::string_view category)
    {
        if (!LibraryLoaded("CreateCustomCategory")) return false;
        if (!LibraryWritable("CreateCustomCategory")) return false;
        // The library refuses an empty or taken name and says why.
        auto& library = OutfitLibrary::GetSingleton();
        const int id = library.AddCategory(ApiName(category));
        if (id == 0) return false;
        // A new category that couldn't be saved is taken back: false means nothing was created.
        if (!library.Save()) {
            library.DeleteCategory(id);
            return false;
        }
        RefreshLibrary();
        return true;
    }

    bool AddCategory(std::string_view outfit, std::string_view category)
    {
        if (!LibraryLoaded("AddCategory")) return false;
        if (!LibraryWritable("AddCategory")) return false;
        const auto found = Outfit(outfit);
        if (!found) return false;
        const auto target = Category(category);
        if (!target) return false;
        if (std::ranges::find(target->outfitIds, found->id) != target->outfitIds.end()) return true;

        auto& library = OutfitLibrary::GetSingleton();
        if (!library.AddOutfitToCategory(target->id, found->id)) return false;
        library.Save();
        RefreshLibrary();
        return true;
    }

    std::string GetOutfit(RE::Actor* actor)
    {
        if (!HasActor(actor)) return {};
        return OutfitName(SituationHandler::GetSingleton()->CurrentOutfitId(actor));
    }

    std::string GetSituation(RE::Actor* actor)
    {
        if (!HasActor(actor)) return {};
        return SituationName(static_cast<int>(SituationHandler::GetSingleton()->EvaluateSituation(actor, true, true)));
    }

    bool HasSituation(RE::Actor* actor, std::string_view situation)
    {
        if (!HasActor(actor)) return false;
        const auto parsed = SituationFromName(situation);
        if (!parsed) {
            logger::info("Tailor API: '{}' is not a situation (adventuring, town, home, sleep, swimming or warm)", situation);
            return false;
        }
        return SituationHandler::GetSingleton()->HasSituationChoice(actor->GetFormID(), static_cast<OutfitSituation>(*parsed));
    }

    bool OverrideWithOutfit(RE::Actor* actor, std::string_view outfit)
    {
        if (!Overridable(actor)) return false;
        const auto found = Outfit(outfit);
        if (!found) return false;
        if (!OutfitStore::GetSingleton().Fits(found->id, OutfitManager::GetNpcSex(actor))) {
            logger::info("Tailor API: outfit '{}' doesn't fit {:08X}", found->name, actor->GetFormID());
            return false;
        }
        WatchForOverride(actor);
        ModOverrides::GetSingleton().Set(actor->GetFormID(), found->id);
        SKSE::GetTaskInterface()->AddTask(Dress(actor->GetHandle()));
        return true;
    }

    bool OverrideWithSituation(RE::Actor* actor, std::string_view situation)
    {
        if (!Overridable(actor)) return false;
        const auto parsed = SituationFromName(situation);
        if (!parsed) {
            logger::info("Tailor API: '{}' is not a situation (adventuring, town, home, sleep, swimming or warm)", situation);
            return false;
        }
        // Their own choice for the situation first, then the situation's pool.
        int id = SituationHandler::GetSingleton()->SituationChoice(actor->GetFormID(), static_cast<OutfitSituation>(*parsed));
        if (id == 0) id = RandomFitting(OutfitLibrary::GetSingleton().GetSituationOutfitIds(SituationName(*parsed)), actor);
        if (id == 0) {
            logger::info("Tailor API: no {} outfit fits {:08X}", SituationName(*parsed), actor->GetFormID());
            return false;
        }
        WatchForOverride(actor);
        // Set for Swimming or Sleep it is that look too: it hides their weapons (SituationHandler::OverrideLook).
        ModOverrides::GetSingleton().Set(actor->GetFormID(), id, *parsed);
        SKSE::GetTaskInterface()->AddTask(Dress(actor->GetHandle()));
        return true;
    }

    bool OverrideWithCategory(RE::Actor* actor, std::string_view category)
    {
        if (!Overridable(actor)) return false;
        const auto target = Category(category);
        if (!target) return false;
        const int id = RandomFitting(target->outfitIds, actor);
        if (id == 0) {
            logger::info("Tailor API: no outfit in category '{}' fits {:08X}", category, actor->GetFormID());
            return false;
        }
        WatchForOverride(actor);
        ModOverrides::GetSingleton().Set(actor->GetFormID(), id);
        SKSE::GetTaskInterface()->AddTask(Dress(actor->GetHandle()));
        return true;
    }

    bool HasOutfitOverride(RE::Actor* actor)
    {
        return HasActor(actor) && ModOverrides::GetSingleton().Contains(actor->GetFormID());
    }

    bool ClearOutfitOverride(RE::Actor* actor)
    {
        if (!HasActor(actor)) return false;
        if (!ModOverrides::GetSingleton().Clear(actor->GetFormID())) return false;
        // An NPC Tailor keeps no outfit for is marked to get their own outfit back before the task is queued: if the
        // task can't find them, the re-equip queue still gives it back when they next load. A Default Outfit return
        // already waiting does the same.
        const auto actorId = actor->GetFormID();
        const auto& assigned = OutfitAssignments::GetSingleton();
        if (!actor->IsPlayerRef() && !actor->IsDead() && !assigned.HasAssignment(actorId) && !assigned.IsRestoreDefaultPending(actorId)) {
            OutfitManager::GetSingleton().MarkReturnToOwnOutfit(actorId);
        }
        SKSE::GetTaskInterface()->AddTask([handle = actor->GetHandle(), actorId] {
            const auto ref = handle.get();
            auto* person = ref.get();
            if (!person) return;
            auto* situations = SituationHandler::GetSingleton();
            auto& manager = OutfitManager::GetSingleton();
            // Someone open in Tailor's outfit editor is dressed by the editor when it closes.
            const bool editing = manager.IsCreateSessionActor(person);
            // A player whose only situation was the override keeps no stale look: their assigned outfit or
            // Own Gear goes on.
            if (person->IsPlayerRef()) {
                if (!situations->PlayerHasSituations()) situations->ForgetPlayerSituationState();
                if (!editing) manager.ReconcilePlayer();
                return;
            }
            if (person->IsDead()) return;
            // Tailor keeps an outfit for them: their situations or regular outfit dress them.
            auto& assignments = OutfitAssignments::GetSingleton();
            if (assignments.HasAssignment(actorId)) {
                // From nothing remembered: a swim starting in water must take their land outfit as the one to put
                // back, not the outfit of the override just cleared, which is what they still wear.
                if (!editing) {
                    situations->ClearCachedSituation(actorId);
                    // With only a regular outfit and no wig situations, the poll no longer watches them and water
                    // leaves their clothes alone, so the situation flow would leave the override's outfit on them in
                    // water until their cell reloads: the re-equip queue puts the regular outfit on, in water too.
                    if (!assignments.HasAnySituation(actorId) && !WigAssignments::GetSingleton().HasAnySituation(actorId)) {
                        CellHandler::QueueOutfitReEquip(person->GetHandle(), 0);
                    } else {
                        situations->ForceApplyForSituation(person);
                    }
                }
                return;
            }
            // Anyone else gets their own outfit back (marked above). Wig situations never change the outfit, so an
            // NPC with only those does too, and keeps the wig. The re-equip queue restores it, retrying while OBody
            // settles, now or when they next load.
            situations->ClearOutfitOverrides(actorId);
            CellHandler::QueueOutfitReEquip(person->GetHandle(), 0);
        });
        return true;
    }

    bool EvaluateOutfit(RE::Actor* actor)
    {
        if (!HasActor(actor)) return false;
        const auto actorId = actor->GetFormID();
        if (!actor->IsPlayerRef() && !ModOverrides::GetSingleton().Contains(actorId) &&
            !OutfitAssignments::GetSingleton().HasAssignment(actorId) && !WigAssignments::GetSingleton().HasAnySituation(actorId)) {
            logger::info("Tailor API: Tailor doesn't dress {:08X}", actorId);
            return false;
        }
        SKSE::GetTaskInterface()->AddTask(Dress(actor->GetHandle()));
        return true;
    }

    void PollEvents(const std::unordered_set<RE::FormID>& actorIds)
    {
        // The people Tailor dresses, and anyone with an override: the NPCs the poll watched, the player, NPCs
        // with an outfit assignment, and override actors. Anyone seen since the load stays watched, so a change
        // is still told when Tailor stops dressing someone or their override ends.
        auto watched = actorIds;
        if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) watched.insert(player->GetFormID());
        for (const auto& [id, row] : OutfitAssignments::GetSingleton().GetAll()) {
            if (row.HasOutfits()) watched.insert(id);
        }
        for (const auto id : ModOverrides::GetSingleton().Actors()) watched.insert(id);
        for (const auto& [id, seen] : sSeen) watched.insert(id);

        auto* situations = SituationHandler::GetSingleton();
        for (const auto id : watched) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            // One the game no longer holds is forgotten: a reference made at run time can come back under its
            // FormID as someone else.
            if (!actor) {
                sSeen.erase(id);
                continue;
            }
            if (!actor->Is3DLoaded() || actor->IsDead() || actor->IsChild()) continue;
            // Not known yet while Tailor is still putting their situation outfit back on, as after a load: the
            // 0 CurrentOutfitId gives then is not what they wear.
            const auto outfit = situations->OutfitPending(actor) ? std::nullopt : std::optional{situations->CurrentOutfitId(actor)};
            // As GetSituation names it, quietly.
            const int situation = static_cast<int>(situations->EvaluateSituation(actor, true, true));
            const auto change = Observe(sSeen[id], outfit, situation);
            // C++ hears a change through each listener, as Papyrus hears the mod event.
            if (change.outfit) {
                const auto name = OutfitName(*outfit);
                SendModEvent("Tailor_OnOutfitChanged", actor, name);
                TellListeners(sListeners, [&](TailorAPI::ITailorListener1* listener) { listener->OnOutfitChanged(actor, name.c_str()); });
            }
            if (change.situation) {
                const std::string name = SituationName(situation);
                SendModEvent("Tailor_OnSituationChanged", actor, name);
                TellListeners(sListeners, [&](TailorAPI::ITailorListener1* listener) { listener->OnSituationChanged(actor, name.c_str()); });
            }
        }
    }

    void ForgetEvents()
    {
        sSeen.clear();
    }

    void MarkLibraryLoaded()
    {
        sLibraryLoaded.store(true);
    }

    void AddListener(TailorAPI::ITailorListener1* listener)
    {
        if (listener && std::ranges::find(sListeners, listener) == sListeners.end()) sListeners.push_back(listener);
    }

    void RemoveListener(TailorAPI::ITailorListener1* listener)
    {
        std::erase(sListeners, listener);
    }
}
