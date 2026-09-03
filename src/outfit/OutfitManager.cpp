#include "outfit/OutfitManager.h"
#include "outfit/OutfitAssignments.h"
#include "events/CellHandler.h"
#include "events/SituationHandler.h"
#include "Settings.h"
#include "compat/OBodyCompat.h"
#include "wig/WigManager.h"

#include <unordered_set>

OutfitManager& OutfitManager::GetSingleton()
{
    static OutfitManager singleton;
    return singleton;
}

void OutfitManager::NotifyOutfitChanged(RE::Actor* actor)
{
    if (!actor) return;
    WigManager::GetSingleton().ReEquipWigAfterOutfitChange(actor);

    // OBody owns its morph and ORefit state through armor equip events. Do not
    // ask it to regenerate the actor's preset or send the generic refresh that
    // made older OBody builds re-enter distribution during Tailor swaps.
    if (Settings::GetSingleton().GetRefreshMorphs()) {
        if (OBodyCompat::GetSingleton().TryRefreshMorphs(actor)) {
            return;
        }

        // Non-OBody fallback for body morph mods (AutoBody, CBPC, etc.).
        // SKEE hooks this event to trigger its morph pipeline.
        const SKSE::NiNodeUpdateEvent event{ actor };
        auto* source = SKSE::GetNiNodeUpdateEventSource();
        if (source) {
            source->SendEvent(std::addressof(event));
        }
    }
}

void OutfitManager::Initialize()
{
    logger::info("OutfitManager initialized");
}

// --- Target Management ---

void OutfitManager::UpdateTargetFromCrosshair()
{
    auto* crosshairRef = RE::CrosshairPickData::GetSingleton();
    if (!crosshairRef) {
        _currentTarget = RE::ActorHandle{};
        return;
    }

    // In unified SE+AE+VR builds, CrosshairPickData::target is an array
    // (one per VR device). Check all entries for the first valid actor.
    for (auto& handle : crosshairRef->target) {
        auto refPtr = handle.get();
        if (refPtr) {
            auto* actor = refPtr->As<RE::Actor>();
            if (actor && IsValidTarget(actor)) {
                _currentTarget = actor->GetHandle();
                return;
            }
        }
    }

    _currentTarget = RE::ActorHandle{};
}

RE::Actor* OutfitManager::GetTarget() const
{
    auto actor = _currentTarget.get();
    if (actor && !actor->IsPlayerRef()) {
        return actor.get();
    }
    return nullptr;
}

std::string OutfitManager::GetTargetName() const
{
    auto* target = GetTarget();
    if (!target) {
        return "";
    }
    return target->GetDisplayFullName();
}

std::string OutfitManager::GetTargetSex() const
{
    auto* target = GetTarget();
    if (!target) {
        return "";
    }
    auto* npc = target->GetActorBase();
    if (!npc) {
        return "";
    }
    return npc->GetSex() == RE::SEX::kFemale ? "female" : "male";
}

bool OutfitManager::IsValidTarget(RE::Actor* actor) const
{
    if (!actor || actor->IsDead() || actor->IsPlayerRef()) {
        return false;
    }

    auto* base = actor->GetActorBase();
    if (!base) {
        return false;
    }

    auto* race = base->GetRace();
    if (!race) {
        return false;
    }

    auto* creatureKW = RE::TESForm::LookupByID<RE::BGSKeyword>(0x13795);
    auto* dragonKW = RE::TESForm::LookupByID<RE::BGSKeyword>(0x35D59);

    if (creatureKW && (race->HasKeyword(creatureKW) || actor->HasKeyword(creatureKW))) {
        return false;
    }
    if (dragonKW && (race->HasKeyword(dragonKW) || actor->HasKeyword(dragonKW))) {
        return false;
    }

    return true;
}

// --- Dynamic Outfit Forms ---

void OutfitManager::UnequipWornArmorNotInOutfit(RE::Actor* actor,
                                                RE::BGSOutfit* incomingOutfit,
                                                RE::BGSOutfit* outgoingOutfit) const
{
    if (!actor) return;

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) return;

    // Items in the incoming outfit stay on; its slots are what we need to free.
    std::unordered_set<RE::TESBoundObject*> incomingArmor;
    std::uint32_t incomingSlots = 0;
    if (incomingOutfit) {
        for (auto* form : incomingOutfit->outfitItems) {
            if (auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr) {
                incomingArmor.insert(armor);
                incomingSlots |= armor->GetSlotMask().underlying();
            }
        }
    }

    // Pieces of the outfit we are replacing — ours to take off.
    std::unordered_set<RE::TESBoundObject*> outgoingArmor;
    if (outgoingOutfit) {
        for (auto* form : outgoingOutfit->outfitItems) {
            if (auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr) {
                outgoingArmor.insert(armor);
            }
        }
    }

    std::vector<RE::TESBoundObject*> wornArmor;
    for (auto& [object, data] : actor->GetInventory([](RE::TESBoundObject& item) {
             return item.IsArmor();
         })) {
        auto& [count, entry] = data;
        if (!object || count <= 0 || !entry || !entry->IsWorn()) {
            continue;
        }
        if (incomingArmor.contains(object)) {
            continue;  // staying on
        }

        // Anything else is only fair game if it is a piece of the outfit being
        // replaced, or if it occupies a slot the incoming outfit needs.
        //
        // Do NOT sweep every worn item: SMP/physics config objects (CBBE 3BBB's
        // "SMP ON" object and friends) are meshless armor worn in otherwise
        // unused slots, and unequipping them silently kills body physics while
        // leaving them sitting in the inventory looking fine.
        bool ours = outgoingArmor.contains(object);
        bool conflicts = false;
        if (auto* armor = object->As<RE::TESObjectARMO>()) {
            conflicts = (armor->GetSlotMask().underlying() & incomingSlots) != 0;
        }

        if (ours || conflicts) {
            wornArmor.push_back(object);
        }
    }

    for (auto* armor : wornArmor) {
        // Unequip synchronously and silently. Unlike Actor::SetDefaultOutfit,
        // this leaves the exact inventory instance (including extra data) intact.
        equipManager->UnequipObject(actor, armor, nullptr, 1, nullptr, false, false, false, true);
    }
}

bool OutfitManager::SetDefaultOutfitPreservingInventory(
    RE::Actor* actor, RE::BGSOutfit* outfit, bool update3D) const
{
    if (!actor) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;
    if (npc->defaultOutfit == outfit) return true;

    // CommonLib's Actor::SetDefaultOutfit calls RemoveOutfitItems on the
    // outgoing outfit. That can destroy an NPC's own copy of the same armor.
    // Unequip outgoing pieces instead, then update the base record directly.
    // npc->defaultOutfit is still the outgoing outfit here — it is replaced on
    // the next line.
    UnequipWornArmorNotInOutfit(actor, outfit, npc->defaultOutfit);
    npc->defaultOutfit = outfit;
    actor->InitInventoryIfRequired();

    if (outfit && !actor->IsDisabled()) {
        actor->AddWornOutfit(outfit, update3D);
    }
    return true;
}

bool OutfitManager::SetSleepOutfitPreservingInventory(RE::Actor* actor, RE::BGSOutfit* outfit) const
{
    if (!actor) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;
    if (npc->sleepOutfit == outfit) return true;

    // Tailor persists assignments itself. Do not mark a runtime FFxxxxxx outfit
    // as an NPC base-record change; that pointer is not stable across processes.
    npc->sleepOutfit = outfit;
    return true;
}

bool OutfitManager::GetOutfitChangeFlag(RE::TESNPC* npc, std::uint32_t flag)
{
    auto* saveLoad = RE::BGSSaveLoadGame::GetSingleton();
    return npc && saveLoad && saveLoad->GetChange(npc, flag);
}

void OutfitManager::SuppressOutfitChangeFlags(RE::TESNPC* npc)
{
    if (!npc) return;
    npc->RemoveChange(RE::TESNPC::ChangeFlags::kDefaultOutfit);
    npc->RemoveChange(RE::TESNPC::ChangeFlags::kSleepOutfit);
}

void OutfitManager::RestoreOutfitChangeFlags(
    RE::TESNPC* npc,
    bool defaultHadChange,
    bool sleepHadChange)
{
    if (!npc) return;

    if (defaultHadChange) {
        npc->AddChange(RE::TESNPC::ChangeFlags::kDefaultOutfit);
    } else {
        npc->RemoveChange(RE::TESNPC::ChangeFlags::kDefaultOutfit);
    }

    if (sleepHadChange) {
        npc->AddChange(RE::TESNPC::ChangeFlags::kSleepOutfit);
    } else {
        npc->RemoveChange(RE::TESNPC::ChangeFlags::kSleepOutfit);
    }
}

void OutfitManager::CapturePersistedOutfitStateIfNeeded(RE::Actor* actor)
{
    if (!actor) return;

    auto& assignments = OutfitAssignments::GetSingleton();
    const auto actorId = actor->GetFormID();
    if (!assignments.HasAssignment(actorId) && !assignments.HasAnySituation(actorId)) {
        return;
    }

    auto* npc = actor->GetActorBase();
    if (!npc) return;

    RE::BGSOutfit* savedDefault = nullptr;
    RE::BGSOutfit* savedSleep = nullptr;
    bool ignoredChange = false;
    const bool defaultKnown = assignments.GetOriginalDefaultOutfit(actorId, savedDefault);
    const bool sleepKnown = assignments.GetOriginalSleepOutfit(actorId, savedSleep);
    const bool defaultChangeKnown = assignments.GetOriginalDefaultChangeState(actorId, ignoredChange);
    const bool sleepChangeKnown = assignments.GetOriginalSleepChangeState(actorId, ignoredChange);

    auto isPluginBacked = [](RE::BGSOutfit* outfit) {
        return outfit && (outfit->GetFormID() & 0xFF000000) != 0xFF000000;
    };

    // For released v1 assignments, a plugin-backed saved original DOFT is
    // authoritative. SOFT was not stored; only adopt a live plugin-backed value
    // and never fabricate SOFT=DOFT or treat an invalidated FF pointer as null.
    auto* originalDefault = npc->defaultOutfit;
    const bool canCaptureDefault = !defaultKnown && isPluginBacked(originalDefault);
    auto* originalSleep = npc->sleepOutfit;
    const bool canCaptureSleep = !sleepKnown && isPluginBacked(originalSleep);
    const bool canCaptureDefaultChange =
        !defaultChangeKnown && isPluginBacked(originalDefault) &&
        (canCaptureDefault || (defaultKnown && originalDefault == savedDefault));
    const bool canCaptureSleepChange =
        !sleepChangeKnown && isPluginBacked(originalSleep) &&
        (canCaptureSleep || (sleepKnown && originalSleep == savedSleep));

    // Legacy assignments did not record whether these change bits predated
    // Tailor. Preserve any loaded bit when restoring the original outfit, but
    // suppress it while Tailor owns a volatile runtime outfit.
    const bool defaultHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kDefaultOutfit);
    const bool sleepHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kSleepOutfit);
    if (assignments.CaptureMissingOriginalOutfitState(
            actorId,
            originalDefault,
            canCaptureDefault,
            originalSleep,
            canCaptureSleep,
            canCaptureDefaultChange,
            defaultHadChange,
            canCaptureSleepChange,
            sleepHadChange)) {
        assignments.Save();
    }

    // Tailor now owns the temporary runtime pointers. The original ownership
    // state was captured above; suppressing these two bits prevents FFxxxxxx
    // outfits from being serialized into the save.
    SuppressOutfitChangeFlags(npc);
}

bool OutfitManager::InitOutfitPair(OutfitPair& pair)
{
    if (!pair.primary) {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSOutfit>();
        if (factory) {
            pair.primary = factory->Create();
            logger::info("Created dynamic outfit (FormID: {:08X})",
                pair.primary ? pair.primary->GetFormID() : 0);
        }
    }
    if (!pair.alternate) {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSOutfit>();
        if (factory) {
            pair.alternate = factory->Create();
            logger::info("Created dynamic alternate outfit (FormID: {:08X})",
                pair.alternate ? pair.alternate->GetFormID() : 0);
        }
    }
    return pair.primary != nullptr && pair.alternate != nullptr;
}

OutfitManager::OutfitPair* OutfitManager::GetOrCreateActorOutfit(RE::FormID actorId)
{
    auto& pair = _actorOutfits[actorId];
    return InitOutfitPair(pair) ? &pair : nullptr;
}

void OutfitManager::InitFlushOutfit()
{
    if (!_flushOutfit) {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSOutfit>();
        if (factory) {
            _flushOutfit = factory->Create();
            logger::info("Created dynamic flush outfit (FormID: {:08X})",
                _flushOutfit ? _flushOutfit->GetFormID() : 0);
        }
    }
}

// --- Create Outfit ---

void OutfitManager::BeginCreateOutfit(RE::Actor* actor)
{
    if (!actor) return;

    if (_createSessionActive) {
        EndCreateOutfit(nullptr);
    }

    auto* npc = actor->GetActorBase();
    if (!npc) return;
    _preCreateOutfit = npc ? npc->defaultOutfit : nullptr;
    _preCreateSleepOutfit = npc ? npc->sleepOutfit : nullptr;
    _preCreateDefaultHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kDefaultOutfit);
    _preCreateSleepHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kSleepOutfit);
    _createSessionActor = actor->GetHandle();
    _createSessionActive = true;
    SuppressOutfitChangeFlags(npc);

    if (InitOutfitPair(_editOutfit)) {
        _editOutfit.desiredItems.clear();
    }

    logger::info("BeginCreateOutfit: ready for {} (saved original outfit: {})",
        actor->GetDisplayFullName(), _preCreateOutfit ? "yes" : "none");
}

void OutfitManager::EndCreateOutfit(RE::Actor* actor)
{
    (void)actor;
    if (!_createSessionActive) return;

    auto actorPtr = _createSessionActor.get();
    if (actorPtr) {
        auto* sessionActor = actorPtr.get();
        SetDefaultOutfitPreservingInventory(sessionActor, _preCreateOutfit, true);

        // Restore the exact captured SOFT, including a real null.
        SetSleepOutfitPreservingInventory(sessionActor, _preCreateSleepOutfit);
        RestoreOutfitChangeFlags(
            sessionActor->GetActorBase(),
            _preCreateDefaultHadChange,
            _preCreateSleepHadChange);
        logger::info(
            "EndCreateOutfit: restored captured outfit state on {}",
            sessionActor->GetDisplayFullName());
    } else {
        logger::warn("EndCreateOutfit: captured actor is no longer available");
    }

    // Reuse the registered runtime forms. Discarding the pointers here leaked
    // two FF forms per Create/Edit session.
    _editOutfit.desiredItems.clear();
    if (_editOutfit.primary) _editOutfit.primary->outfitItems.clear();
    if (_editOutfit.alternate) _editOutfit.alternate->outfitItems.clear();
    _preCreateOutfit = nullptr;
    _preCreateSleepOutfit = nullptr;
    _preCreateDefaultHadChange = false;
    _preCreateSleepHadChange = false;
    _createSessionActor = RE::ActorHandle{};
    _createSessionActive = false;
}

void OutfitManager::AddItemToCreateOutfit(RE::Actor* actor, const ArmorItem& item)
{
    if (!actor) return;

    auto* form = item.Resolve();
    if (!form) {
        logger::warn("AddItemToCreateOutfit: failed to resolve '{}' from '{}'", item.name, item.plugin);
        return;
    }

    if (!InitOutfitPair(_editOutfit)) return;

    _editOutfit.desiredItems.push_back(form);
    if (!FlushAndApplyOutfit(actor, _editOutfit)) return;

    logger::info("AddItemToCreateOutfit: added '{}' — outfit now has {} items",
        item.name, _editOutfit.desiredItems.size());
}

void OutfitManager::LoadCreateOutfitItems(RE::Actor* actor, const std::vector<ArmorItem>& items)
{
    if (!actor) return;

    if (!InitOutfitPair(_editOutfit)) return;

    _editOutfit.desiredItems.clear();

    for (auto& item : items) {
        auto* form = item.Resolve();
        if (form) {
            _editOutfit.desiredItems.push_back(form);
        } else {
            logger::warn("LoadCreateOutfitItems: failed to resolve '{}' from '{}'", item.name, item.plugin);
        }
    }

    if (!FlushAndApplyOutfit(actor, _editOutfit)) return;

    logger::info("LoadCreateOutfitItems: loaded {} items onto {}",
        _editOutfit.desiredItems.size(), actor->GetDisplayFullName());
}

void OutfitManager::RemoveItemFromCreateOutfit(RE::Actor* actor, const ArmorItem& item)
{
    if (!actor) return;

    auto* form = item.Resolve();
    if (!form || !_editOutfit.primary) return;

    // Rebuild the outfit items without the removed form
    std::vector<RE::TESForm*> keep;
    for (auto entry : _editOutfit.desiredItems) {
        if (entry != form) {
            keep.push_back(entry);
        }
    }
    _editOutfit.desiredItems = std::move(keep);

    if (!FlushAndApplyOutfit(actor, _editOutfit)) return;

    logger::info("RemoveItemFromCreateOutfit: removed '{}' — outfit now has {} items",
        item.name, _editOutfit.desiredItems.size());
}

// --- Flush + Apply ---

bool OutfitManager::FlushAndApplyOutfit(RE::Actor* actor, OutfitPair& pair)
{
    if (!actor || !InitOutfitPair(pair)) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;

    if (npc->defaultOutfit == pair.alternate) {
        std::swap(pair.primary, pair.alternate);
    }

    pair.alternate->outfitItems.clear();
    for (auto* item : pair.desiredItems) {
        pair.alternate->outfitItems.push_back(item);
    }

    auto* incoming = pair.alternate;

    if (!OBodyCompat::GetSingleton().IsInstalled()) {
        InitFlushOutfit();
        if (_flushOutfit && _flushOutfit != incoming) {
            SetDefaultOutfitPreservingInventory(actor, _flushOutfit, true);
        }
    }

    if (!SetDefaultOutfitPreservingInventory(actor, incoming, true)) {
        return false;
    }

    // Also override the NPC's sleep outfit (SOFT) to prevent vanilla sleep
    // outfits (e.g. Belted Tunic) from overriding equipped body-slot items.
    SetSleepOutfitPreservingInventory(actor, incoming);
    std::swap(pair.primary, pair.alternate);
    return true;
}

// --- Custom Outfit Application ---

bool OutfitManager::ApplyCustomOutfit(
    RE::Actor* actor,
    const CustomOutfit& outfit,
    bool automatic)
{
    if (!actor) return false;

    if (automatic &&
        !OBodyCompat::GetSingleton().PrepareActorForAutomaticOutfitChange(actor)) {
        return false;
    }

    CapturePersistedOutfitStateIfNeeded(actor);

    // Each actor gets their own outfit object — see OutfitPair in the header.
    auto* pair = GetOrCreateActorOutfit(actor->GetFormID());
    if (!pair) return false;

    pair->desiredItems.clear();
    for (auto& item : outfit.items) {
        auto* form = item.Resolve();
        if (form) {
            pair->desiredItems.push_back(form);
        }
    }

    // Log template flags for debugging NPC outfit issues
    if (auto* npc = actor->GetActorBase()) {
        using TF = RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG;
        auto flags = npc->actorData.templateUseFlags;
        if (flags.underlying() != 0) {
            std::string flagStr;
            if (flags.all(TF::kTraits))     flagStr += " Traits";
            if (flags.all(TF::kInventory))  flagStr += " INVENTORY";
            if (flags.all(TF::kSpells))     flagStr += " Spells";
            if (flags.all(TF::kAIPackages)) flagStr += " AIPackages";
            if (flags.all(TF::kStats))      flagStr += " Stats";
            logger::info("ApplyCustomOutfit: {} template flags: 0x{:X}{}",
                actor->GetDisplayFullName(), flags.underlying(), flagStr);
        }
        logger::info("ApplyCustomOutfit: {} defaultOutfit={}, faceNPC={}",
            actor->GetDisplayFullName(),
            npc->defaultOutfit ? npc->defaultOutfit->GetFormEditorID() : "null",
            npc->faceNPC ? npc->faceNPC->GetFormEditorID() : "null");
    }

    if (!FlushAndApplyOutfit(actor, *pair)) {
        return false;
    }
    NotifyOutfitChanged(actor);

    logger::info("Applied custom outfit '{}' ({} items) to {}",
        outfit.name, outfit.items.size(), actor->GetDisplayFullName());
    return true;
}

// --- Cycling ---

bool OutfitManager::StartCycle(int categoryId)
{
    std::lock_guard lock(_mutex);

    auto* target = GetTarget();
    if (!target) {
        logger::warn("StartCycle: no target");
        return false;
    }

    auto* category = OutfitLibrary::GetSingleton().GetCategoryById(categoryId);
    if (!category) {
        logger::warn("StartCycle: category id {} not found", categoryId);
        return false;
    }

    if (category->outfitIds.empty()) {
        logger::warn("StartCycle: category '{}' has no outfits", category->name);
        return false;
    }

    // Capture the NPC's current outfits so CancelCycle can restore them
    auto* npc = target->GetActorBase();
    _preCycleOutfit = npc ? npc->defaultOutfit : nullptr;
    _preCycleSleepOutfit = npc ? npc->sleepOutfit : nullptr;
    _preCycleDefaultHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kDefaultOutfit);
    _preCycleSleepHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kSleepOutfit);
    _preCycleStateCaptured = true;
    _preCycleDefaultWasActorPair = false;
    _preCycleSleepWasActorPair = false;
    _preCycleOutfitItems.clear();

    if (const auto pairIt = _actorOutfits.find(target->GetFormID());
        pairIt != _actorOutfits.end()) {
        const auto& pair = pairIt->second;
        _preCycleDefaultWasActorPair =
            _preCycleOutfit == pair.primary || _preCycleOutfit == pair.alternate;
        _preCycleSleepWasActorPair =
            _preCycleSleepOutfit == pair.primary || _preCycleSleepOutfit == pair.alternate;
        if (_preCycleDefaultWasActorPair && _preCycleOutfit) {
            for (auto* item : _preCycleOutfit->outfitItems) {
                _preCycleOutfitItems.push_back(item);
            }
        }
    }
    SuppressOutfitChangeFlags(npc);

    CycleState state;
    state.categoryId = categoryId;
    state.index = 0;
    state.outfitIds = category->outfitIds;

    auto* firstOutfit = OutfitStore::GetSingleton().GetOutfitById(state.outfitIds[0]);
    if (!firstOutfit || !ApplyCustomOutfit(target, *firstOutfit)) {
        logger::warn("StartCycle: failed to apply first outfit");
        RestoreOutfitChangeFlags(
            npc, _preCycleDefaultHadChange, _preCycleSleepHadChange);
        _preCycleOutfit = nullptr;
        _preCycleSleepOutfit = nullptr;
        _preCycleDefaultHadChange = false;
        _preCycleSleepHadChange = false;
        _preCycleStateCaptured = false;
        _preCycleDefaultWasActorPair = false;
        _preCycleSleepWasActorPair = false;
        _preCycleOutfitItems.clear();
        return false;
    }

    _cycleState = std::move(state);
    logger::info("StartCycle: cycling category '{}' ({} outfits) on {}",
        category->name, category->outfitIds.size(), target->GetDisplayFullName());
    return true;
}

bool OutfitManager::CycleNext()
{
    std::lock_guard lock(_mutex);

    if (!_cycleState) return false;

    auto* target = GetTarget();
    if (!target) return false;

    auto& cs = *_cycleState;
    cs.index = (cs.index + 1) % static_cast<int>(cs.outfitIds.size());

    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(cs.outfitIds[cs.index]);
    if (!outfit || !ApplyCustomOutfit(target, *outfit)) {
        logger::warn("CycleNext: failed to apply outfit at index {}", cs.index);
        return false;
    }

    logger::info("CycleNext: index {} — '{}'", cs.index, outfit->name);
    return true;
}

bool OutfitManager::CyclePrev()
{
    std::lock_guard lock(_mutex);

    if (!_cycleState) return false;

    auto* target = GetTarget();
    if (!target) return false;

    auto& cs = *_cycleState;
    cs.index = (cs.index - 1 + static_cast<int>(cs.outfitIds.size())) % static_cast<int>(cs.outfitIds.size());

    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(cs.outfitIds[cs.index]);
    if (!outfit || !ApplyCustomOutfit(target, *outfit)) {
        logger::warn("CyclePrev: failed to apply outfit at index {}", cs.index);
        return false;
    }

    logger::info("CyclePrev: index {} — '{}'", cs.index, outfit->name);
    return true;
}

bool OutfitManager::CycleToIndex(int index)
{
    std::lock_guard lock(_mutex);

    if (!_cycleState) return false;

    auto* target = GetTarget();
    if (!target) return false;

    auto& cs = *_cycleState;
    int count = static_cast<int>(cs.outfitIds.size());
    if (count == 0 || index < 0 || index >= count) return false;

    cs.index = index;

    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(cs.outfitIds[cs.index]);
    if (!outfit || !ApplyCustomOutfit(target, *outfit)) {
        logger::warn("CycleToIndex: failed to apply outfit at index {}", cs.index);
        return false;
    }

    logger::info("CycleToIndex: index {} — '{}'", cs.index, outfit->name);
    return true;
}

bool OutfitManager::ConfirmCycle(OutfitSituation situation)
{
    std::lock_guard lock(_mutex);

    if (!_cycleState) return false;

    auto* target = GetTarget();
    if (!target) return false;

    // Preview already applied the selected outfit. Confirm only persists it;
    // another flip here would emit redundant equip events into OBody.
    auto& cs = *_cycleState;
    int outfitId = cs.outfitIds[cs.index];
    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
    if (outfit) {
        logger::info("ConfirmCycle: confirmed outfit '{}' on {}", outfit->name, target->GetDisplayFullName());
    }

    // Persist the assignment
    auto& assignments = OutfitAssignments::GetSingleton();
    if (static_cast<int>(situation) > 0) {
        assignments.AssignSituation(target->GetFormID(), situation, outfitId);
    } else {
        assignments.Assign(target->GetFormID(), outfitId);
    }

    assignments.CaptureOriginalOutfitState(
        target->GetFormID(),
        _preCycleOutfit,
        _preCycleSleepOutfit,
        _preCycleDefaultHadChange,
        _preCycleSleepHadChange);
    assignments.Save();

    _preCycleOutfit = nullptr;
    _preCycleSleepOutfit = nullptr;
    _preCycleDefaultHadChange = false;
    _preCycleSleepHadChange = false;
    _preCycleStateCaptured = false;
    _preCycleDefaultWasActorPair = false;
    _preCycleSleepWasActorPair = false;
    _preCycleOutfitItems.clear();
    _cycleState.reset();

    // After saving, evaluate the current situation and apply the correct outfit.
    // This ensures the NPC immediately wears the right outfit for their current
    // situation rather than staying in whatever was just previewed.
    if (assignments.HasAnySituation(target->GetFormID())) {
        SituationHandler::GetSingleton()->ForceApplyForSituation(target);
    }

    return true;
}

bool OutfitManager::RestoreCycleSnapshot(RE::Actor* actor)
{
    if (!actor || !_preCycleStateCaptured) return false;

    bool defaultRestored = false;
    RE::BGSOutfit* restoredDefault = _preCycleOutfit;

    if (_preCycleDefaultWasActorPair) {
        auto pairIt = _actorOutfits.find(actor->GetFormID());
        if (pairIt != _actorOutfits.end()) {
            pairIt->second.desiredItems = _preCycleOutfitItems;
            defaultRestored = FlushAndApplyOutfit(actor, pairIt->second);
            restoredDefault = defaultRestored ? pairIt->second.primary : nullptr;
        }
    } else {
        // A captured null is a real state, not a missing sentinel.
        defaultRestored = SetDefaultOutfitPreservingInventory(actor, _preCycleOutfit, true);
    }

    bool sleepRestored = false;
    if (_preCycleSleepWasActorPair && restoredDefault) {
        sleepRestored = SetSleepOutfitPreservingInventory(actor, restoredDefault);
    } else {
        sleepRestored = SetSleepOutfitPreservingInventory(actor, _preCycleSleepOutfit);
    }

    RestoreOutfitChangeFlags(
        actor->GetActorBase(),
        _preCycleDefaultHadChange,
        _preCycleSleepHadChange);
    return defaultRestored && sleepRestored;
}

void OutfitManager::CancelCycle()
{
    std::lock_guard lock(_mutex);

    if (!_cycleState) return;

    auto* target = GetTarget();
    if (target) {
        if (RestoreCycleSnapshot(target)) {
            logger::info("CancelCycle: restored exact captured outfit state on {}", target->GetDisplayFullName());
        } else {
            logger::warn("CancelCycle: could not restore captured outfit state on {}", target->GetDisplayFullName());
        }
        NotifyOutfitChanged(target);
    }

    _preCycleOutfit = nullptr;
    _preCycleSleepOutfit = nullptr;
    _preCycleDefaultHadChange = false;
    _preCycleSleepHadChange = false;
    _preCycleStateCaptured = false;
    _preCycleDefaultWasActorPair = false;
    _preCycleSleepWasActorPair = false;
    _preCycleOutfitItems.clear();
    _cycleState.reset();
}

bool OutfitManager::IsCycling() const
{
    std::lock_guard lock(_mutex);
    return _cycleState.has_value();
}

const OutfitManager::CycleState* OutfitManager::GetCycleState() const
{
    std::lock_guard lock(_mutex);
    if (_cycleState) {
        return &(*_cycleState);
    }
    return nullptr;
}

std::string OutfitManager::GetCycleOutfitName() const
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) return "";

    auto& cs = *_cycleState;
    if (cs.outfitIds.empty() || cs.index < 0 ||
        cs.index >= static_cast<int>(cs.outfitIds.size())) {
        return "";
    }

    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(cs.outfitIds[cs.index]);
    return outfit ? outfit->name : "";
}

// --- Reset ---

bool OutfitManager::ResetOutfit(RE::Actor* actor)
{
    if (!actor) return false;

    auto& assignments = OutfitAssignments::GetSingleton();
    const auto actorId = actor->GetFormID();
    bool restored = false;

    // A reset during cycling uses the exact transient snapshot, including a
    // known-null original or a copied Tailor runtime outfit.
    if (_preCycleStateCaptured) {
        restored = RestoreCycleSnapshot(actor);
        _preCycleOutfit = nullptr;
        _preCycleSleepOutfit = nullptr;
        _preCycleDefaultHadChange = false;
        _preCycleSleepHadChange = false;
        _preCycleStateCaptured = false;
        _preCycleDefaultWasActorPair = false;
        _preCycleSleepWasActorPair = false;
        _preCycleOutfitItems.clear();
        _cycleState.reset();
    } else {
        RE::BGSOutfit* originalOutfit = nullptr;
        RE::BGSOutfit* originalSleepOutfit = nullptr;
        bool defaultHadChange = false;
        bool sleepHadChange = false;

        const bool defaultKnown = assignments.GetOriginalDefaultOutfit(actorId, originalOutfit);
        const bool sleepKnown = assignments.GetOriginalSleepOutfit(actorId, originalSleepOutfit);
        const bool defaultChangeKnown =
            assignments.GetOriginalDefaultChangeState(actorId, defaultHadChange);
        const bool sleepChangeKnown =
            assignments.GetOriginalSleepChangeState(actorId, sleepHadChange);

        if (!defaultKnown || !sleepKnown || !defaultChangeKnown || !sleepChangeKnown) {
            logger::warn(
                "ResetOutfit: original DOFT/SOFT ownership is incomplete for {}; "
                "leaving the assignment intact instead of guessing",
                actor->GetDisplayFullName());
            return false;
        }

        restored =
            SetDefaultOutfitPreservingInventory(actor, originalOutfit, true) &&
            SetSleepOutfitPreservingInventory(actor, originalSleepOutfit);
        if (restored) {
            RestoreOutfitChangeFlags(
                actor->GetActorBase(), defaultHadChange, sleepHadChange);
        }
    }

    if (!restored) {
        logger::warn(
            "ResetOutfit: failed to restore captured outfit state for {}; assignment retained",
            actor->GetDisplayFullName());
        return false;
    }

    logger::info("Reset outfit for {} to captured original DOFT/SOFT state", actor->GetDisplayFullName());
    NotifyOutfitChanged(actor);

    // Remove persisted assignment only after exact restoration succeeds.
    if (assignments.HasAssignment(actorId) || assignments.HasAnySituation(actorId)) {
        assignments.Unassign(actorId);
        assignments.Save();
    }

    return true;
}

// --- Re-apply on Game Load ---

void OutfitManager::ReApplyAllAssignments()
{
    auto& assignments = OutfitAssignments::GetSingleton();
    auto all = assignments.GetAll();

    if (all.empty()) {
        logger::info("ReApplyAllAssignments: no assignments to restore");
        return;
    }

    std::vector<RE::ActorHandle> batch;
    int deferred = 0;

    for (auto& [actorFormId, sa] : all) {
        (void)sa;
        auto* form = RE::TESForm::LookupByID(actorFormId);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) continue;

        if (actor->Is3DLoaded()) {
            batch.push_back(actor->GetHandle());
        } else {
            deferred++;
        }
    }

    if (batch.empty()) {
        logger::info("ReApplyAllAssignments: 0 applied, {} deferred to cell load", deferred);
        return;
    }

    // Use the same generation-checked retry path as cell attach. This prevents
    // duplicate detached workers from crossing a save-load boundary.
    constexpr int32_t kStaggerMs = 50;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CellHandler::QueueOutfitReEquip(
            batch[i], static_cast<int32_t>(i) * kStaggerMs);
    }

    logger::info(
        "ReApplyAllAssignments: queued {} outfits ({}ms apart), {} deferred to cell load",
        batch.size(), kStaggerMs, deferred);
}

void OutfitManager::PrepareForGameLoad()
{
    std::lock_guard lock(_mutex);

    // Restore transient sessions without scheduling visual work: the game is
    // about to revert the world, but no TESNPC may be left pointing at one of
    // Tailor's runtime forms when those forms are invalidated.
    if (_preCycleStateCaptured) {
        if (auto* target = GetTarget()) {
            if (auto* npc = target->GetActorBase()) {
                if (!_preCycleDefaultWasActorPair) npc->defaultOutfit = _preCycleOutfit;
                if (!_preCycleSleepWasActorPair) npc->sleepOutfit = _preCycleSleepOutfit;
                RestoreOutfitChangeFlags(
                    npc, _preCycleDefaultHadChange, _preCycleSleepHadChange);
            }
        }
    }

    if (_createSessionActive) {
        auto actorPtr = _createSessionActor.get();
        if (actorPtr) {
            if (auto* npc = actorPtr->GetActorBase()) {
                npc->defaultOutfit = _preCreateOutfit;
                npc->sleepOutfit = _preCreateSleepOutfit;
                RestoreOutfitChangeFlags(
                    npc, _preCreateDefaultHadChange, _preCreateSleepHadChange);
            }
        }
    }

    auto& assignments = OutfitAssignments::GetSingleton();
    for (auto& [actorId, pair] : _actorOutfits) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        auto* npc = actor ? actor->GetActorBase() : nullptr;
        if (!npc) continue;

        const bool defaultIsTailor =
            npc->defaultOutfit == pair.primary || npc->defaultOutfit == pair.alternate;
        const bool sleepIsTailor =
            npc->sleepOutfit == pair.primary || npc->sleepOutfit == pair.alternate;
        if (!defaultIsTailor && !sleepIsTailor) continue;

        if (defaultIsTailor) {
            RE::BGSOutfit* original = nullptr;
            if (assignments.GetOriginalDefaultOutfit(actorId, original)) {
                npc->defaultOutfit = original;
            } else {
                npc->defaultOutfit = nullptr;
                logger::warn(
                    "OutfitManager: original DOFT unknown for 0x{:08X}; detached Tailor runtime form as null before load",
                    actorId);
            }

            bool hadChange = false;
            if (assignments.GetOriginalDefaultChangeState(actorId, hadChange) && hadChange) {
                npc->AddChange(RE::TESNPC::ChangeFlags::kDefaultOutfit);
            } else {
                npc->RemoveChange(RE::TESNPC::ChangeFlags::kDefaultOutfit);
            }
        }

        if (sleepIsTailor) {
            RE::BGSOutfit* original = nullptr;
            if (assignments.GetOriginalSleepOutfit(actorId, original)) {
                npc->sleepOutfit = original;
            } else {
                npc->sleepOutfit = nullptr;
                logger::warn(
                    "OutfitManager: original SOFT unknown for 0x{:08X}; detached Tailor runtime form as null before load",
                    actorId);
            }

            bool hadChange = false;
            if (assignments.GetOriginalSleepChangeState(actorId, hadChange) && hadChange) {
                npc->AddChange(RE::TESNPC::ChangeFlags::kSleepOutfit);
            } else {
                npc->RemoveChange(RE::TESNPC::ChangeFlags::kSleepOutfit);
            }
        }
    }

    // Runtime-created FFxxxxxx forms do not have a stable lifetime across a
    // save revert. Drop every raw pointer without dereferencing it; assignments
    // JSON will create fresh staging pairs after the new save is ready.
    _actorOutfits.clear();
    _editOutfit = {};
    _flushOutfit = nullptr;
    _preCreateOutfit = nullptr;
    _preCreateSleepOutfit = nullptr;
    _createSessionActor = RE::ActorHandle{};
    _createSessionActive = false;
    _preCycleOutfit = nullptr;
    _preCycleSleepOutfit = nullptr;
    _preCycleOutfitItems.clear();
    _preCycleStateCaptured = false;
    _preCycleDefaultWasActorPair = false;
    _preCycleSleepWasActorPair = false;
    _preCycleDefaultHadChange = false;
    _preCycleSleepHadChange = false;
    _preCreateDefaultHadChange = false;
    _preCreateSleepHadChange = false;
    _cycleState.reset();
    _currentTarget = RE::ActorHandle{};

    logger::info("OutfitManager: cleared runtime outfit forms for game load");
}
