#include "outfit/OutfitManager.h"
#include "api/ModOverrides.h"
#include "outfit/OutfitAssignments.h"
#include "outfit/NpcRecordReader.h"
#include "outfit/PreviewOutfitCleanup.h"
#include "outfit/PreviewEquipmentDiagnostics.h"
#include "outfit/ArmorEquipmentStatus.h"
#include "ui/TailorUI.h"
#include "events/CellHandler.h"
#include "events/SituationHandler.h"
#include "events/SituationHelmets.h"
#include "Settings.h"
#include "compat/OBodyCompat.h"
#include "wig/WigManager.h"
#include "preview/TailorPreviewSession.h"
#include "player/PlayerIds.h"
#include "player/PlayerWardrobe.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

OutfitManager& OutfitManager::GetSingleton()
{
    static OutfitManager singleton;
    return singleton;
}

void OutfitManager::NotifyOutfitChanged(RE::Actor* actor)
{
    if (!actor) return;
    Tailor::Preview::TailorPreviewSession::GetSingleton().NotifyAppearanceChanged(actor);
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
    logger::info("PreviewDiag: preview-diag-20260909-c enabled for Create/Edit Outfit sessions");
}

void OutfitManager::CaptureDefaultOutfitsAtDataLoad()
{
    std::lock_guard lock(_mutex);

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::error("OutfitManager: cannot capture data-load outfit defaults without TESDataHandler");
        return;
    }

    _dataLoadedDefaultOutfits.clear();
    for (auto* npc : dataHandler->GetFormArray<RE::TESNPC>()) {
        if (!npc) continue;
        _dataLoadedDefaultOutfits.insert_or_assign(
            npc->GetFormID(), DefaultOutfitState{ npc->defaultOutfit, npc->sleepOutfit });
    }

    auto& assignments = OutfitAssignments::GetSingleton();
    std::size_t repaired = 0;
    std::size_t corrected = 0;
    std::size_t unresolved = 0;
    for (const auto& [actorId, assignment] : assignments.GetAll()) {
        (void)assignment;

        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        DefaultOutfitState state;
        if (!GetDataLoadedDefaultOutfitState(actor, state)) {
            ++unresolved;
            continue;
        }

        // kDataLoaded precedes save loading, so these are the winning plugin
        // defaults and neither outfit change bit belongs to the save.
        if (assignments.CaptureMissingOriginalOutfitState(
                actorId,
                state.defaultOutfit,
                true,
                state.sleepOutfit,
                true,
                true,
                false,
                true,
                false)) {
            ++repaired;
        }
        // Older captures recorded whichever outfit the NPC wore when first dressed,
        // often another mod's. The NPC's own record is the original.
        if (StoreOriginRecordOutfitState(actor)) {
            ++corrected;
        }
    }

    if (repaired > 0 || corrected > 0) {
        assignments.Save();
    }

    logger::info(
        "OutfitManager: captured {} data-load NPC outfit defaults; repaired {} assignment(s), "
        "corrected {} to the NPC's own plugin record, {} unresolved",
        _dataLoadedDefaultOutfits.size(),
        repaired,
        corrected,
        unresolved);
}

// --- Target Management ---

void OutfitManager::UpdateTargetFromCrosshair()
{
    // A fresh open: the player becomes the target only when TailorUI asks.
    _playerTarget = false;
    _sessionNpc = RE::ActorHandle{};
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
                _sessionNpc = _currentTarget;
                return;
            }
        }
    }

    _currentTarget = RE::ActorHandle{};
}

RE::Actor* OutfitManager::GetTarget() const
{
    auto actor = _currentTarget.get();
    // The player is a target only when chosen as one, never by accident.
    if (actor && (!actor->IsPlayerRef() || _playerTarget)) {
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

int OutfitManager::GetNpcSex(RE::Actor* actor)
{
    auto* npc = actor ? actor->GetActorBase() : nullptr;
    return npc ? static_cast<int>(npc->GetSex()) : -1;
}

OutfitLibrary::Wearable OutfitManager::WearableBy(RE::Actor* actor)
{
    const int sex = GetNpcSex(actor);
    return [sex](int id) { return OutfitStore::GetSingleton().Fits(id, sex); };
}

bool OutfitManager::IsValidTarget(RE::Actor* actor) const
{
    // Tailor never handles children.
    if (!actor || actor->IsDead() || actor->IsPlayerRef() || actor->IsChild()) {
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

bool OutfitManager::InitializeHiddenOutfitItems(
    RE::Actor* actor, RE::BGSOutfit* outfit, bool update3D) const
{
    if (!actor || !outfit) return false;

    auto* inventory = actor->GetInventoryChanges();
    if (!inventory) return false;

    inventory->InitOutfitItems(outfit, actor->GetLevel());

    const auto outfitFormId = outfit->GetFormID();
    std::size_t taggedItems = 0;
    std::size_t equippedItems = 0;
    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    const bool immediate = _createSessionActive && actor->GetHandle() == _createSessionActor;
    std::vector<std::pair<RE::TESBoundObject*, RE::ExtraDataList*>> instances;

    if (inventory->entryList) {
        for (auto* entryData : *inventory->entryList) {
            if (!entryData || !entryData->object || !entryData->extraLists) continue;

            for (auto* extraList : *entryData->extraLists) {
                auto* outfitItem = extraList ? extraList->GetByType<RE::ExtraOutfitItem>() : nullptr;
                if (!outfitItem || outfitItem->id != outfitFormId) continue;

                ++taggedItems;
                instances.emplace_back(entryData->object, extraList);
            }
        }
    }

    for (const auto& [object, extraList] : instances) {
        // Immediate preview equips may dispatch events which mutate inventory.
        if (!actor->IsDisabled() && equipManager &&
            Tailor::Outfits::IsOutfitInstance(actor, object, extraList, outfitFormId)) {
            equipManager->EquipObject(actor, object, extraList, 1, nullptr,
                immediate ? false : update3D, false, false, immediate);
            ++equippedItems;
        }
    }

    const auto expectedItems = static_cast<std::size_t>(outfit->outfitItems.size());
    // Restored vanilla outfits may contain leveled lists with variable output.
    const bool hasLeveledItems = std::ranges::any_of(outfit->outfitItems, [](auto* item) {
        return item && item->Is(RE::FormType::LeveledItem);
    });
    if (!hasLeveledItems && taggedItems != expectedItems) {
        logger::warn(
            "OutfitManager: initialized {}/{} hidden outfit items for {} (outfit {:08X})",
            taggedItems,
            expectedItems,
            actor->GetDisplayFullName(),
            outfitFormId);
        return false;
    }

    logger::info(
        "OutfitManager: initialized {}/{} hidden outfit items for {} ({} equip requests)",
        taggedItems,
        expectedItems,
        actor->GetDisplayFullName(),
        equippedItems);
    return true;
}

bool OutfitManager::SetActorDefaultOutfit(
    RE::Actor* actor, RE::BGSOutfit* outfit, bool update3D) const
{
    if (!actor || actor->IsPlayerRef()) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;

    std::vector<RE::TESForm*> incomingItems;
    if (outfit) incomingItems.assign(outfit->outfitItems.begin(), outfit->outfitItems.end());
    if (!WigManager::GetSingleton().PrepareForOutfitChange(actor, incomingItems)) return false;
    actor->InitInventoryIfRequired();

    const bool previewActor = _createSessionActive && actor->GetHandle() == _createSessionActor;
    const auto primary = _editOutfit.primary ? _editOutfit.primary->GetFormID() : 0;
    const auto alternate = _editOutfit.alternate ? _editOutfit.alternate->GetFormID() : 0;
    if (previewActor && !Tailor::Outfits::UnequipPreviewItems(actor, primary, alternate)) {
        logger::warn("OutfitManager: preview item still worn on {}; postponing outfit replacement",
            actor->GetDisplayFullName());
        return false;
    }

    // Remove every engine-marked outfit instance, including entries left by a
    // temporary outfit form from an earlier process. Ordinary inventory items
    // have no ExtraOutfitItem marker and are not affected.
    actor->RemoveOutfitItems(nullptr);
    if (previewActor && Tailor::Outfits::HasPreviewItems(actor, primary, alternate)) {
        logger::warn("OutfitManager: preview inventory remains on {}; postponing outfit replacement",
            actor->GetDisplayFullName());
        return false;
    }
    npc->SetDefaultOutfit(outfit);

    const bool hiddenInventoryReady =
        !outfit || InitializeHiddenOutfitItems(actor, outfit, update3D);

    // Runtime outfit forms use temporary FF FormIDs. Tailor persists their
    // source records itself, so the transient pointer must not enter the save.
    npc->RemoveChange(RE::TESNPC::ChangeFlags::kDefaultOutfit);
    QueueEquipmentAudit(actor, outfit);
    return hiddenInventoryReady && npc->defaultOutfit == outfit;
}

void OutfitManager::QueueEquipmentAudit(RE::Actor* actor, RE::BGSOutfit* outfit) const
{
    std::lock_guard lock(_mutex);
    const auto actorId = actor->GetFormID();
    const auto token = ++_nextEquipmentAudit;
    _equipmentAudits.erase(actorId);
    if (!outfit || outfit == _flushOutfit) return;
    _equipmentAudits[actorId] = token;
    const auto outfitId = outfit->GetFormID();
    std::vector<RE::FormID> expected;
    for (auto* item : outfit->outfitItems) if (item && item->IsArmor()) expected.push_back(item->GetFormID());
    std::thread([this, handle = actor->GetHandle(), actorId, outfitId, token, expected = std::move(expected)]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        SKSE::GetTaskInterface()->AddTask([this, handle, actorId, outfitId, token, expected]() {
            std::lock_guard auditLock(_mutex);
            const auto it = _equipmentAudits.find(actorId);
            if (it == _equipmentAudits.end() || it->second != token) return;
            _equipmentAudits.erase(it);
            const auto ref = handle.get();
            auto* target = ref.get();
            if (!target || !target->Is3DLoaded() || target->IsDead()) return;
            auto* npc = target->GetActorBase();
            if (!npc || !npc->defaultOutfit || npc->defaultOutfit->GetFormID() != outfitId) return;
            if (WigManager::GetSingleton().IsWigScreen(target)) return; // intentionally displaced equipment
            // Helmets Hide Helmets has off are off on purpose.
            const auto helmetsOff = SituationHelmets::GetSingleton().TakenPieces(target);
            std::size_t incomplete = 0;
            for (const auto id : expected) {
                auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
                if (armor && std::ranges::find(helmetsOff, armor) != helmetsOff.end()) continue;
                const auto status = Tailor::Outfits::InspectArmorEquipment(target, armor);
                if (status.worn && status.compatibleModel && status.attached && status.graphVisible) continue;
                ++incomplete;
                logger::warn("Outfit equipment: actor={:08X} outfit={:08X} armor={:08X} name='{}' worn={} compatibleModel={} attached={} graphVisible={}",
                    actorId, outfitId, id, armor ? armor->GetName() : "unresolved", status.worn,
                    status.compatibleModel, status.attached, status.graphVisible);
            }
            logger::info("Outfit equipment audit: {:08X}, {} requested armor records, {} incomplete after settling", actorId, expected.size(), incomplete);
            if (incomplete) {
                Tailor::Outfits::LogPreviewEquipment(target, "incomplete-outfit-after-settle", token);
                TailorUI::GetSingleton().ShowEquipmentWarning(target,
                    "Some outfit pieces could not be equipped or displayed. Check for conflicting items.");
            }
        });
    }).detach();
}

bool OutfitManager::SetActorSleepOutfit(RE::Actor* actor, RE::BGSOutfit* outfit) const
{
    if (!actor || actor->IsPlayerRef()) return false;

    auto* npc = actor->GetActorBase();
    if (!npc) return false;
    if (npc->sleepOutfit == outfit) return true;

    npc->SetSleepOutfit(outfit);
    npc->RemoveChange(RE::TESNPC::ChangeFlags::kSleepOutfit);
    return npc->sleepOutfit == outfit;
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

bool OutfitManager::GetDataLoadedDefaultOutfitState(
    RE::Actor* actor, DefaultOutfitState& state) const
{
    if (!actor) return false;

    std::lock_guard lock(_mutex);
    auto findState = [this, &state](RE::TESNPC* npc) {
        if (!npc) return false;
        const auto it = _dataLoadedDefaultOutfits.find(npc->GetFormID());
        if (it == _dataLoadedDefaultOutfits.end()) return false;
        state = it->second;
        return true;
    };

    if (findState(actor->GetActorBase())) return true;
    return findState(actor->GetTemplateBase());
}

bool OutfitManager::GetOriginRecordOutfitState(RE::Actor* actor, DefaultOutfitState& state) const
{
    auto* npc = actor ? actor->GetActorBase() : nullptr;
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    // A leveled actor's base is assembled at runtime; no plugin record describes it.
    if (!npc || !dataHandler || npc->IsDynamicForm()) return false;

    // The first file to define the form is where the NPC comes from; later files
    // in the list only override it.
    const auto* origin = npc->GetFile(0);
    if (!origin) return false;
    const auto id = npc->GetFormID();
    const auto* owner = (id >> 24) == 0xFE
        ? dataHandler->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((id >> 12) & 0xFFF))
        : dataHandler->LookupLoadedModByIndex(static_cast<std::uint8_t>(id >> 24));
    if (!owner) owner = origin;

    const auto record = Tailor::Records::ReadNpcOutfits(
        std::filesystem::path("Data") / origin->GetFilename(),
        owner->GetFilename(),
        owner->IsLight() ? id & 0xFFF : id & 0xFFFFFF);
    // With an inventory template the outfit worn is the template's, so this
    // record's own DOFT is not the NPC's default.
    if (!record || record->usesTemplateInventory) return false;

    const auto resolve = [dataHandler](const std::optional<Tailor::Records::FormRef>& link, RE::BGSOutfit*& outfit) {
        outfit = link ? dataHandler->LookupForm<RE::BGSOutfit>(link->localId, link->plugin) : nullptr;
        return !link || outfit;
    };
    DefaultOutfitState resolved;
    if (!resolve(record->defaultOutfit, resolved.defaultOutfit) || !resolve(record->sleepOutfit, resolved.sleepOutfit)) {
        logger::warn("OutfitManager: {} names an outfit for {} that is not loaded", origin->GetFilename(), actor->GetDisplayFullName());
        return false;
    }
    state = resolved;
    return true;
}

void OutfitManager::GetOriginRecordChangeState(
    RE::Actor* actor, const DefaultOutfitState& origin, bool& defaultHadChange, bool& sleepHadChange) const
{
    DefaultOutfitState winning;
    const bool known = GetDataLoadedDefaultOutfitState(actor, winning);
    defaultHadChange = !known || winning.defaultOutfit != origin.defaultOutfit;
    sleepHadChange = !known || winning.sleepOutfit != origin.sleepOutfit;
}

bool OutfitManager::StoreOriginRecordOutfitState(RE::Actor* actor)
{
    DefaultOutfitState origin;
    if (!GetOriginRecordOutfitState(actor, origin)) return false;
    bool defaultHadChange = false, sleepHadChange = false;
    GetOriginRecordChangeState(actor, origin, defaultHadChange, sleepHadChange);
    return OutfitAssignments::GetSingleton().SetOriginalOutfitState(
        actor->GetFormID(), origin.defaultOutfit, origin.sleepOutfit, defaultHadChange, sleepHadChange);
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
    std::lock_guard lock(_mutex);
    if (!actor) return;
    if (!WigManager::GetSingleton().SetWigScreen(false)) return;

    if (_createSessionActive) {
        EndCreateOutfit();
        if (_createSessionActive) {
            logger::warn("BeginCreateOutfit: previous preview restoration is still pending");
            return;
        }
    }

    if (actor->IsPlayerRef()) {
        BeginPlayerCreateOutfit(actor);
        return;
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
    _createSessionEnding = false;
    ++_createSessionGeneration;
    SuppressOutfitChangeFlags(npc);

    if (InitOutfitPair(_editOutfit)) {
        _editOutfit.desiredItems.clear();
    }

    logger::info("BeginCreateOutfit: ready for {} (saved original outfit: {})",
        actor->GetDisplayFullName(), _preCreateOutfit ? "yes" : "none");
    Tailor::Outfits::LogPreviewEquipment(actor, "begin-create", _createSessionGeneration);
}

void OutfitManager::EndCreateOutfit()
{
    std::lock_guard lock(_mutex);
    if (!_createSessionActive) return;
    _createSessionEnding = true;
    // A fresh close/cancel can retry a failed restoration. Older queued work
    // must not race it or a subsequent preview session.
    ++_createSessionGeneration;
    TryEndCreateOutfit(_createSessionGeneration, 0);
}

void OutfitManager::TryEndCreateOutfit(std::uint64_t generation, int attempt)
{
    std::lock_guard lock(_mutex);
    if (generation != _createSessionGeneration || !_createSessionActive || !_createSessionEnding) return;

    auto actorPtr = _createSessionActor.get();
    // The wardrobe puts the player back at once; no retries are needed.
    if (actorPtr && actorPtr->IsPlayerRef()) {
        EndPlayerCreateOutfit(actorPtr.get());
        return;
    }
    bool restored = false;
    if (actorPtr) {
        auto* sessionActor = actorPtr.get();
        if (attempt == 0) {
            Tailor::Outfits::LogPreviewEquipment(sessionActor, "before-cleanup", generation);
        }
        const bool defaultRestored = SetActorDefaultOutfit(sessionActor, _preCreateOutfit, true);

        // Restore the exact captured SOFT, including a real null.
        const bool sleepRestored = SetActorSleepOutfit(sessionActor, _preCreateSleepOutfit);
        restored = defaultRestored && sleepRestored;
        if (restored) {
            RestoreOutfitChangeFlags(
                sessionActor->GetActorBase(),
                _preCreateDefaultHadChange,
                _preCreateSleepHadChange);
            Tailor::Outfits::LogPreviewEquipment(sessionActor, "after-captured-restore", generation);
        }
    }

    if (!restored) {
        if (attempt < 20) {
            // AddTask alone drains in one frame. Delay in real time, then touch
            // the actor only on the game thread after validating the generation.
            std::thread([this, generation, attempt]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                SKSE::GetTaskInterface()->AddTask([this, generation, attempt]() {
                    TryEndCreateOutfit(generation, attempt + 1);
                });
            }).detach();
        } else {
            logger::error("EndCreateOutfit: restoration failed after {} attempts; retained preview state for actor handle {:08X}",
                attempt + 1, _createSessionActor.native_handle());
            Tailor::Outfits::LogPreviewEquipment(actorPtr.get(), "cleanup-failed", generation);
        }
        return;
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
    _createSessionEnding = false;
    logger::info("EndCreateOutfit: preview outfit markers cleared and captured outfit state restored on {} (attempt {}; mesh detachment not verified)",
        actorPtr->GetDisplayFullName(), attempt + 1);
    RestoreAssignedOutfit(actorPtr.get());
    Tailor::Outfits::LogPreviewEquipment(actorPtr.get(), "after-assignment-restore", generation);
    QueuePreviewDiagnostics(actorPtr->GetHandle(), generation);
}

void OutfitManager::QueuePreviewDiagnostics(RE::ActorHandle actor, std::uint64_t generation)
{
    for (const int delayMs : {200, 1000}) {
        std::thread([this, actor, generation, delayMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            SKSE::GetTaskInterface()->AddTask([this, actor, generation, delayMs]() {
                std::lock_guard lock(_mutex);
                if (generation != _createSessionGeneration) return;
                if (auto actorPtr = actor.get()) {
                    Tailor::Outfits::LogPreviewEquipment(actorPtr.get(),
                        std::format("after-restore-{}ms", delayMs), generation);
                }
            });
        }).detach();
    }
}

void OutfitManager::RestoreAssignedOutfit(RE::Actor* actor)
{
    auto& assignments = OutfitAssignments::GetSingleton();
    const auto actorId = actor->GetFormID();
    // Her last outfit was deleted, and the redress queue leaves an NPC in the editor
    // alone. Now that the editor is done, the queue returns her to the Default Outfit.
    if (assignments.IsRestoreDefaultPending(actorId)) {
        CellHandler::QueueOutfitReEquip(actor->GetHandle());
    }
    // Likewise it gives an NPC a deleted override outfit let go their own outfit back.
    if (IsReturningToOwnOutfit(actorId)) CellHandler::QueueOutfitReEquip(actor->GetHandle());
    // Situations, or a mod's override, dress her through the situation flow.
    if (assignments.HasAnySituation(actorId) || ModOverrides::GetSingleton().Contains(actorId)) {
        SituationHandler::GetSingleton()->ForceApplyForSituation(actor);
        return;
    }
    const auto outfitId = assignments.GetOutfitId(actorId);
    if (outfitId > 0) {
        if (auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId)) {
            // Still assigned, but tagged for the other sex: the Default Outfit until it fits again.
            if (!OutfitFits(outfit->sex, GetNpcSex(actor))) {
                if (RestoreOriginalOutfit(actor)) NotifyOutfitChanged(actor);
                else logger::warn("RestoreAssignedOutfit: outfit {} does not fit {} and the Default Outfit could not be restored",
                    outfitId, actor->GetDisplayFullName());
                return;
            }
            if (!ApplyCustomOutfit(actor, *outfit)) {
                logger::warn("EndCreateOutfit: could not refresh assigned outfit {} on {}",
                    outfitId, actor->GetDisplayFullName());
            }
            return;
        }
    }
    NotifyOutfitChanged(actor);
}

void OutfitManager::AddItemToCreateOutfit(RE::Actor* actor, const ArmorItem& item)
{
    if (!actor || !_createSessionActive || _createSessionEnding || actor->GetHandle() != _createSessionActor) return;

    auto* form = item.Resolve();
    if (!form) {
        logger::warn("AddItemToCreateOutfit: failed to resolve '{}' from '{}'", item.name, item.plugin);
        return;
    }

    if (!InitOutfitPair(_editOutfit)) return;

    Tailor::Outfits::LogPreviewEquipment(actor, "before-preview-add", _createSessionGeneration);
    _editOutfit.desiredItems.push_back(form);
    if (!FlushAndApplyOutfit(actor, _editOutfit)) return;

    logger::info("AddItemToCreateOutfit: added '{}' — outfit now has {} items",
        item.name, _editOutfit.desiredItems.size());
    Tailor::Outfits::LogPreviewEquipment(actor, "after-preview-add", _createSessionGeneration);
}

void OutfitManager::LoadCreateOutfitItems(RE::Actor* actor, const std::vector<ArmorItem>& items)
{
    if (!actor || !_createSessionActive || _createSessionEnding || actor->GetHandle() != _createSessionActor) return;

    if (!InitOutfitPair(_editOutfit)) return;

    Tailor::Outfits::LogPreviewEquipment(actor, "before-preview-load", _createSessionGeneration);
    _editOutfit.desiredItems.clear();

    for (auto& item : items) {
        auto* form = item.Resolve();
        if (form) {
            _editOutfit.desiredItems.push_back(form);
        } else {
            logger::warn("LoadCreateOutfitItems: failed to resolve '{}' from '{}'", item.name, item.plugin);
        }
    }

    // The player is dressed from the inventory: an outfit with nothing loaded would strip them bare.
    if (actor->IsPlayerRef() && !items.empty() && _editOutfit.desiredItems.empty()) {
        KeepPlayerLookForUnloadedOutfit(actor);
        return;
    }

    if (!FlushAndApplyOutfit(actor, _editOutfit)) return;

    logger::info("LoadCreateOutfitItems: loaded {} items onto {}",
        _editOutfit.desiredItems.size(), actor->GetDisplayFullName());
    Tailor::Outfits::LogPreviewEquipment(actor, "after-preview-load", _createSessionGeneration);
}

void OutfitManager::RemoveItemFromCreateOutfit(RE::Actor* actor, const ArmorItem& item)
{
    if (!actor || !_createSessionActive || _createSessionEnding || actor->GetHandle() != _createSessionActor) return;

    auto* form = item.Resolve();
    if (!form || !_editOutfit.primary) return;

    Tailor::Outfits::LogPreviewEquipment(actor, "before-preview-remove", _createSessionGeneration);
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
    Tailor::Outfits::LogPreviewEquipment(actor, "after-preview-remove", _createSessionGeneration);
}

// --- Flush + Apply ---

bool OutfitManager::FlushAndApplyOutfit(RE::Actor* actor, OutfitPair& pair)
{
    // The player's Create/Edit pieces are worn from the inventory, never through an outfit record.
    if (actor && actor->IsPlayerRef()) return PreviewPlayerItems(actor, pair.desiredItems);
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
            SetActorDefaultOutfit(actor, _flushOutfit, true);
        }
    }

    if (!SetActorDefaultOutfit(actor, incoming, true)) {
        return false;
    }

    // Also override the NPC's sleep outfit (SOFT) to prevent vanilla sleep
    // outfits (e.g. Belted Tunic) from overriding equipped body-slot items.
    SetActorSleepOutfit(actor, incoming);
    std::swap(pair.primary, pair.alternate);
    if (_createSessionActive && actor->GetHandle() == _createSessionActor) NotifyOutfitChanged(actor);
    return true;
}

// --- Custom Outfit Application ---

bool OutfitManager::ApplyCustomOutfit(
    RE::Actor* actor,
    const CustomOutfit& outfit,
    bool automatic)
{
    // The player is dressed through the inventory; `automatic` only gates OBody's NPC work.
    if (actor && actor->IsPlayerRef()) return ApplyPlayerOutfit(actor, outfit);
    if (!actor || actor->IsPlayerRef()) return false;

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

std::optional<OutfitSnapshot> OutfitManager::CaptureOutfitSnapshot(RE::Actor* actor) const
{
    auto* npc = actor ? actor->GetActorBase() : nullptr;
    if (!npc || actor->IsPlayerRef()) return std::nullopt;
    OutfitSnapshot snapshot;
    snapshot.defaultOutfit = npc->defaultOutfit;
    snapshot.sleepOutfit = npc->sleepOutfit;
    snapshot.defaultHadChange = GetOutfitChangeFlag(npc, RE::TESNPC::ChangeFlags::kDefaultOutfit);
    snapshot.sleepHadChange = GetOutfitChangeFlag(npc, RE::TESNPC::ChangeFlags::kSleepOutfit);
    if (const auto it = _actorOutfits.find(actor->GetFormID()); it != _actorOutfits.end()) {
        const auto& pair = it->second;
        snapshot.defaultWasActorPair = snapshot.defaultOutfit &&
            (snapshot.defaultOutfit == pair.primary || snapshot.defaultOutfit == pair.alternate);
        snapshot.sleepWasActorPair = snapshot.sleepOutfit &&
            (snapshot.sleepOutfit == pair.primary || snapshot.sleepOutfit == pair.alternate);
        if (snapshot.defaultWasActorPair && snapshot.defaultOutfit) {
            snapshot.items.assign(snapshot.defaultOutfit->outfitItems.begin(), snapshot.defaultOutfit->outfitItems.end());
        }
    }
    return snapshot;
}

bool OutfitManager::RestoreOutfitSnapshot(RE::Actor* actor, const OutfitSnapshot& snapshot)
{
    if (!actor || actor->IsPlayerRef() || !actor->GetActorBase() ||
        !OBodyCompat::GetSingleton().PrepareActorForAutomaticOutfitChange(actor)) return false;

    auto* restoredDefault = snapshot.defaultOutfit;
    if (snapshot.defaultWasActorPair) {
        auto* pair = GetOrCreateActorOutfit(actor->GetFormID());
        if (!pair) return false;
        pair->desiredItems = snapshot.items;
        if (!FlushAndApplyOutfit(actor, *pair)) return false;
        restoredDefault = pair->primary;
    } else if (!SetActorDefaultOutfit(actor, snapshot.defaultOutfit, true)) {
        return false;
    }
    if (!SetActorSleepOutfit(actor, snapshot.sleepWasActorPair ? restoredDefault : snapshot.sleepOutfit)) return false;
    RestoreOutfitChangeFlags(actor->GetActorBase(), snapshot.defaultHadChange, snapshot.sleepHadChange);
    NotifyOutfitChanged(actor);
    return true;
}

bool OutfitManager::StartCycle(int categoryId, OutfitSituation situation)
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

    const auto type = situation == OutfitSituation::Adventuring
        ? OutfitAssignments::GetSingleton().GetAdventuringArmorType(target->GetFormID()) : OutfitArmorType::Any;
    // Only what the target can wear: a Male outfit never enters a female NPC's rack.
    auto eligibleIds = OutfitLibrary::GetSingleton().FilterAdventuringEligible(category->outfitIds, type, WearableBy(target));
    if (eligibleIds.empty()) {
        logger::warn("StartCycle: category '{}' has no matching outfits", category->name);
        return false;
    }

    // Capture the NPC's current outfits so CancelCycle can restore them
    if (!WigManager::GetSingleton().SetWigScreen(false)) return false;
    // The wardrobe records what the player wears instead; the player's base record
    // is never read or written here.
    const bool player = target->IsPlayerRef();
    if (player && !Tailor::Player::PlayerWardrobe::GetSingleton().BeginPreview(target)) return false;
    auto* npc = player ? nullptr : target->GetActorBase();
    _preCycleOutfit = npc ? npc->defaultOutfit : nullptr;
    _preCycleSleepOutfit = npc ? npc->sleepOutfit : nullptr;
    _preCycleDefaultHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kDefaultOutfit);
    _preCycleSleepHadChange = GetOutfitChangeFlag(
        npc, RE::TESNPC::ChangeFlags::kSleepOutfit);
    _preCycleStateCaptured = !player;
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
    state.outfitIds = std::move(eligibleIds);
    state.situation = situation;

    // Sort only this preview session. Saved category membership and outfit IDs
    // stay unchanged; arrows and dropdown indices share this same order.
    auto& store = OutfitStore::GetSingleton();
    std::stable_sort(state.outfitIds.begin(), state.outfitIds.end(), [&store](int a, int b) {
        const auto* left = store.GetOutfitById(a);
        const auto* right = store.GetOutfitById(b);
        if (!left || !right) return left && !right;
        return _stricmp(left->name.c_str(), right->name.c_str()) < 0;
    });

    auto* firstOutfit = store.GetOutfitById(state.outfitIds[0]);
    if (!firstOutfit || !ApplyCustomOutfit(target, *firstOutfit)) {
        logger::warn("StartCycle: failed to apply first outfit");
        AbandonCycleStart(target, player);
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
    if (situation != cs.situation) return false;
    int outfitId = cs.outfitIds[cs.index];
    const auto wearable = WearableBy(target);
    if (!wearable(outfitId) || (situation == OutfitSituation::Adventuring &&
        !OutfitLibrary::GetSingleton().IsAdventuringEligible(outfitId,
            OutfitAssignments::GetSingleton().GetAdventuringArmorType(target->GetFormID()), wearable))) {
        CancelCycle();
        return false;
    }
    auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
    if (outfit) {
        logger::info("ConfirmCycle: confirmed outfit '{}' on {}", outfit->name, target->GetDisplayFullName());
    }
    if (target->IsPlayerRef()) return ConfirmPlayerCycle(target, situation, outfitId);

    // A newly enabled water/combat override must capture the outfit
    // from before dressing, never the temporary preview being confirmed.
    const bool usesSituations = static_cast<int>(situation) > 0 ||
        OutfitAssignments::GetSingleton().HasAnySituation(target->GetFormID());
    if ((situation == OutfitSituation::Swimming ||
            (usesSituations && target->IsInCombat())) &&
        !RestoreCycleSnapshot(target)) return false;

    // Persist the assignment
    auto& assignments = OutfitAssignments::GetSingleton();
    if (static_cast<int>(situation) > 0) {
        assignments.AssignSituation(target->GetFormID(), situation, outfitId);
    } else {
        assignments.Assign(target->GetFormID(), outfitId);
    }
    // An outfit confirmed for them in Tailor ends a mod's override.
    ModOverrides::GetSingleton().Clear(target->GetFormID());

    assignments.CaptureOriginalOutfitState(
        target->GetFormID(),
        _preCycleOutfit,
        _preCycleSleepOutfit,
        _preCycleDefaultHadChange,
        _preCycleSleepHadChange);
    // What she wore before dressing may be another mod's outfit. Her own plugin
    // record is the original whenever it can be read.
    StoreOriginRecordOutfitState(target);
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
    if (actor && actor->IsPlayerRef()) return CancelPlayerPreview(actor);
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
        defaultRestored = SetActorDefaultOutfit(actor, _preCycleOutfit, true);
    }

    bool sleepRestored = false;
    if (_preCycleSleepWasActorPair && restoredDefault) {
        sleepRestored = SetActorSleepOutfit(actor, restoredDefault);
    } else {
        sleepRestored = SetActorSleepOutfit(actor, _preCycleSleepOutfit);
    }

    RestoreCycleChangeFlags(actor->GetActorBase());
    return defaultRestored && sleepRestored;
}

bool OutfitManager::IsRuntimeOutfit(const RE::BGSOutfit* outfit) const
{
    if (!outfit) return false;
    if (outfit == _flushOutfit || outfit == _editOutfit.primary || outfit == _editOutfit.alternate) return true;
    return std::ranges::any_of(_actorOutfits, [outfit](const auto& entry) {
        return outfit == entry.second.primary || outfit == entry.second.alternate;
    });
}

void OutfitManager::RestoreCycleChangeFlags(RE::TESNPC* npc) const
{
    // A field the cancel or the failed start left on a runtime form gets no flag, whatever was captured: that FF
    // pointer must never reach the save. A field holding a real outfit gets back what it had.
    RestoreOutfitChangeFlags(npc,
        _preCycleDefaultHadChange && npc && !IsRuntimeOutfit(npc->defaultOutfit),
        _preCycleSleepHadChange && npc && !IsRuntimeOutfit(npc->sleepOutfit));
}

void OutfitManager::AbandonCycleStart(RE::Actor* target, bool player)
{
    if (player) Tailor::Player::PlayerWardrobe::GetSingleton().EndPreview(target, false);
    auto* npc = player ? nullptr : target->GetActorBase();
    // A refusal before anything was written left the captured outfits on the record: only the flags go back. A flush
    // that went on first (without OBody), or a first outfit written and then failed, moved it and would leave the NPC
    // on a runtime form: the capture is put back once, now, as part of the player's own action, as Cancel does.
    if (npc && (npc->defaultOutfit != _preCycleOutfit || npc->sleepOutfit != _preCycleSleepOutfit)) {
        if (!RestoreCycleSnapshot(target))
            logger::warn("StartCycle: could not put back {}'s outfit after the first outfit failed", target->GetDisplayFullName());
        // As Cancel does, whether or not the restore worked: the failed outfit may have taken the wig off, and an
        // outfit change re-applies the wig, the hair colour (1/5/12 s) and, without OBody, the body morphs.
        NotifyOutfitChanged(target);
    } else {
        RestoreCycleChangeFlags(npc);
    }
    _preCycleOutfit = nullptr;
    _preCycleSleepOutfit = nullptr;
    _preCycleDefaultHadChange = false;
    _preCycleSleepHadChange = false;
    _preCycleStateCaptured = false;
    _preCycleDefaultWasActorPair = false;
    _preCycleSleepWasActorPair = false;
    _preCycleOutfitItems.clear();
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
    if (actor->IsPlayerRef()) return ResetPlayerOutfit(actor);

    auto& assignments = OutfitAssignments::GetSingleton();
    const auto actorId = actor->GetFormID();
    const bool hadAssignment =
        assignments.HasAssignment(actorId) || assignments.HasAnySituation(actorId);
    bool restored = false;

    // An unassigned preview has no persisted baseline yet, so its exact
    // transient snapshot is the default state. An already assigned actor must
    // reset to the real original outfit, not the previous Tailor outfit.
    DefaultOutfitState origin;
    if (_preCycleStateCaptured && !hadAssignment) {
        // Default Outfit means her own plugin record, even when another mod had
        // dressed her before this preview. The snapshot is only the fallback.
        restored = GetOriginRecordOutfitState(actor, origin) ? RestoreOriginalOutfit(actor) : RestoreCycleSnapshot(actor);
    } else {
        restored = RestoreOriginalOutfit(actor);
    }

    if (!restored) {
        logger::warn(
            "ResetOutfit: failed to restore captured outfit state for {}; assignment retained",
            actor->GetDisplayFullName());
        return false;
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

    logger::info("Reset outfit for {} to captured original DOFT/SOFT state", actor->GetDisplayFullName());
    SituationHandler::GetSingleton()->ClearOutfitOverrides(actorId);
    // Resetting them in Tailor ends a mod's override too.
    ModOverrides::GetSingleton().Clear(actorId);
    NotifyOutfitChanged(actor);

    // Remove persisted assignment only after exact restoration succeeds.
    if (assignments.HasAssignment(actorId) || assignments.HasAnySituation(actorId)) {
        assignments.Unassign(actorId);
        assignments.Save();
    }

    return true;
}

bool OutfitManager::IsCreateSessionActor(RE::Actor* actor) const
{
    std::lock_guard lock(_mutex);
    return actor && _createSessionActive && actor->GetHandle() == _createSessionActor;
}

bool OutfitManager::IsReturningToOwnOutfit(RE::FormID actorId) const
{
    std::lock_guard lock(_mutex);
    return _returnToOwnOutfit.contains(actorId);
}

void OutfitManager::MarkReturnToOwnOutfit(RE::FormID actorId)
{
    std::lock_guard lock(_mutex);
    _returnToOwnOutfit.insert(actorId);
}

void OutfitManager::ForgetReturnToOwnOutfit(RE::FormID actorId)
{
    std::lock_guard lock(_mutex);
    _returnToOwnOutfit.erase(actorId);
}

void OutfitManager::RefitOutfits(const std::vector<int>& outfitIds)
{
    std::lock_guard lock(_mutex);
    auto* situations = SituationHandler::GetSingleton();
    auto& assignments = OutfitAssignments::GetSingleton();
    std::vector<RE::FormID> actorIds;
    const auto add = [&](RE::FormID actorId) {
        if (std::ranges::find(actorIds, actorId) == actorIds.end()) actorIds.push_back(actorId);
    };
    for (const int id : outfitIds) {
        // Wearing one now as a random pick, or due to return to one when a water or combat
        // override ends: ForgetOutfit drops that runtime state, so each is resolved afresh.
        for (const auto actorId : situations->ForgetOutfit(id)) add(actorId);
        // Assigned one in any slot.
        for (const auto actorId : assignments.GetActorsUsingOutfit(id)) add(actorId);
        // A mod's override in one, worn or waiting until it fits them again.
        for (const auto actorId : ModOverrides::GetSingleton().Actors()) {
            if (ModOverrides::GetSingleton().Get(actorId) == id) add(actorId);
        }
        // The player wearing one right now.
        if (Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId() == id) add(Tailor::Player::kPlayerRef);
    }
    // Staggered like a cell load: re-equipping many NPCs in one frame can exceed
    // SKEE's body morph memory and crash. The queue also waits for OBody and retries.
    constexpr int32_t kStaggerMs = 50;
    int32_t queued = 0;
    bool player = false;
    for (const auto actorId : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        // An NPC in an open editor is redressed by the editor when it closes.
        if (!actor || !actor->Is3DLoaded() || IsCreateSessionActor(actor)) continue;
        // The player, a single actor, is reconciled at once below; the queue is for NPCs.
        if (actor->IsPlayerRef()) { player = true; continue; }
        CellHandler::QueueOutfitReEquip(actor->GetHandle(), queued++ * kStaggerMs);
    }
    if (player) ReconcilePlayer();
    logger::info("RefitOutfits: {} outfit(s) changed sex; queued {} loaded NPC(s) {}ms apart",
        outfitIds.size(), queued, kStaggerMs);
}

void OutfitManager::ReleaseDeletedOutfit(int outfitId)
{
    std::lock_guard lock(_mutex);
    auto& assignments = OutfitAssignments::GetSingleton();
    auto* situations = SituationHandler::GetSingleton();

    // Assigned it in any slot, wearing it now as a situation's random pick, or due
    // to return to it when a water or combat override ends.
    auto actorIds = assignments.GetActorsUsingOutfit(outfitId);
    for (const auto actorId : situations->ForgetOutfit(outfitId)) {
        if (std::ranges::find(actorIds, actorId) == actorIds.end()) actorIds.push_back(actorId);
    }
    // A mod's override in it goes too, and whoever it dressed is redressed like the rest. An NPC with no outfit of
    // their own in Tailor, and no Default Outfit return already waiting, has no row to mark: the queue below, or
    // their next load, gives them their own outfit back from _returnToOwnOutfit.
    const auto released = ModOverrides::GetSingleton().ForgetOutfit(outfitId);
    for (const auto actorId : released) {
        if (std::ranges::find(actorIds, actorId) == actorIds.end()) actorIds.push_back(actorId);
        if (actorId != Tailor::Player::kPlayerRef && !assignments.HasAssignment(actorId) &&
            !assignments.IsRestoreDefaultPending(actorId)) {
            _returnToOwnOutfit.insert(actorId);
        }
    }
    // The player wearing it right now.
    if (Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId() == outfitId &&
        std::ranges::find(actorIds, Tailor::Player::kPlayerRef) == actorIds.end()) {
        actorIds.push_back(Tailor::Player::kPlayerRef);
    }

    std::vector<RE::FormID> restoreLater;
    for (const auto actorId : actorIds) {
        situations->ClearOutfitOverrides(actorId);
        // The player is redressed at once below and keeps no Default Outfit metadata.
        if (actorId == Tailor::Player::kPlayerRef) continue;
        const auto* saved = assignments.GetAssignment(actorId);
        auto remaining = saved ? *saved : SituationalAssignment{};
        remaining.RemoveOutfit(outfitId);
        // Nothing of Tailor's is left for her, so she returns to her Default Outfit.
        // That needs the original-outfit state her assignment holds, so the assignment
        // stays, marked, until the queue below or her next load has restored her.
        if (!remaining.HasOutfits()) restoreLater.push_back(actorId);
    }
    assignments.RemoveOutfitFromAllAssignments(outfitId, restoreLater);

    // Staggered like a cell load: re-equipping many NPCs in one frame can exceed
    // SKEE's body morph memory and crash. The queue dresses each from what remains,
    // or a marked one in her Default Outfit, and waits for OBody and retries.
    // One who is not loaded is dressed when she loads.
    constexpr int32_t kStaggerMs = 50;
    int32_t queued = 0;
    bool player = false;
    for (const auto actorId : actorIds) {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        // An NPC in an open editor is redressed by the editor when it closes.
        if (!actor || !actor->Is3DLoaded() || IsCreateSessionActor(actor)) continue;
        // The player, a single actor, is reconciled at once below; the queue is for NPCs.
        if (actor->IsPlayerRef()) { player = true; continue; }
        CellHandler::QueueOutfitReEquip(actor->GetHandle(), queued++ * kStaggerMs);
    }
    // If the deleted outfit was their last situation, their state ends now, not at the first poll after Tailor closes.
    if (player && !situations->PlayerHasSituations()) situations->ForgetPlayerSituationState();
    if (player) ReconcilePlayer();
    logger::info(
        "ReleaseDeletedOutfit: outfit {} removed from {} NPC(s), {} of them to return to the Default Outfit; queued {} loaded NPC(s) {}ms apart",
        outfitId, actorIds.size(), restoreLater.size(), queued, kStaggerMs);
}

bool OutfitManager::RestoreOriginalOutfit(RE::Actor* actor, bool automatic)
{
    // The player's Default Outfit is Own Gear.
    if (actor && actor->IsPlayerRef()) return RestorePlayerOwnGear(actor);
    if (!actor || actor->IsPlayerRef() || !actor->GetActorBase()) return false;
    if (automatic && !OBodyCompat::GetSingleton().PrepareActorForAutomaticOutfitChange(actor)) return false;
    CapturePersistedOutfitStateIfNeeded(actor);
    auto& assignments = OutfitAssignments::GetSingleton();
    const auto actorId = actor->GetFormID();
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

    bool resolvedDefault = defaultKnown;
    bool resolvedSleep = sleepKnown;
    bool resolvedDefaultChange = defaultChangeKnown;
    bool resolvedSleepChange = sleepChangeKnown;

    // The NPC's own plugin record outranks anything captured from the live NPC:
    // a captured outfit may be one another mod assigned before Tailor did.
    DefaultOutfitState originState;
    if (GetOriginRecordOutfitState(actor, originState)) {
        originalOutfit = originState.defaultOutfit;
        originalSleepOutfit = originState.sleepOutfit;
        GetOriginRecordChangeState(actor, originState, defaultHadChange, sleepHadChange);
        resolvedDefault = resolvedSleep = resolvedDefaultChange = resolvedSleepChange = true;
        logger::info(
            "ResetOutfit: using the outfit from {}'s own plugin record ({:08X})",
            actor->GetDisplayFullName(),
            originalOutfit ? originalOutfit->GetFormID() : 0);
    }

    DefaultOutfitState dataLoadedState;
    if ((!resolvedDefault || !resolvedSleep ||
            !resolvedDefaultChange || !resolvedSleepChange) &&
        GetDataLoadedDefaultOutfitState(actor, dataLoadedState)) {
        if (!resolvedDefault) {
            originalOutfit = dataLoadedState.defaultOutfit;
            resolvedDefault = true;
        }
        if (!resolvedSleep) {
            originalSleepOutfit = dataLoadedState.sleepOutfit;
            resolvedSleep = true;
        }
        if (!resolvedDefaultChange) {
            defaultHadChange = false;
            resolvedDefaultChange = true;
        }
        if (!resolvedSleepChange) {
            sleepHadChange = false;
            resolvedSleepChange = true;
        }
        logger::info(
            "ResetOutfit: using data-load default fallback for {}",
            actor->GetDisplayFullName());
    }

    if (!resolvedDefault) {
        logger::warn(
            "ResetOutfit: no original or data-load default outfit is available for {}",
            actor->GetDisplayFullName());
        return false;
    }

    // Missing SOFT metadata must never prevent the primary DOFT reset. If
    // no plugin baseline exists, clearing SOFT removes Tailor's override.
    if (!resolvedSleep) {
        originalSleepOutfit = nullptr;
    }
    if (!resolvedDefaultChange) {
        defaultHadChange = false;
    }
    if (!resolvedSleepChange) {
        sleepHadChange = false;
    }

    const bool restored =
        SetActorDefaultOutfit(actor, originalOutfit, true) &&
        SetActorSleepOutfit(actor, originalSleepOutfit);
    if (restored) {
        RestoreOutfitChangeFlags(
            actor->GetActorBase(), defaultHadChange, sleepHadChange);
    }
    return restored;
}

// --- Re-apply on Game Load ---

void OutfitManager::ReApplyAllAssignments()
{
    auto& assignments = OutfitAssignments::GetSingleton();
    auto all = assignments.GetAll();
    // This save's mod overrides dress their people too, managed or not.
    const auto overridden = ModOverrides::GetSingleton().Actors();

    if (all.empty() && overridden.empty()) {
        logger::info("ReApplyAllAssignments: no assignments to restore");
        return;
    }

    std::vector<RE::ActorHandle> batch;
    int deferred = 0;

    for (auto& [actorFormId, sa] : all) {
        (void)sa;
        auto* form = RE::TESForm::LookupByID(actorFormId);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor || actor->IsPlayerRef()) continue;

        if (actor->Is3DLoaded()) {
            batch.push_back(actor->GetHandle());
        } else {
            deferred++;
        }
    }
    for (const auto actorFormId : overridden) {
        if (all.contains(actorFormId)) continue;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
        if (!actor || actor->IsPlayerRef() || actor->IsDead()) continue;

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
    _equipmentAudits.clear();
    ++_createSessionGeneration;
    _createSessionEnding = false;

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
            // A player session wrote no outfit record: the player's base record stays untouched.
            if (auto* npc = actorPtr->IsPlayerRef() ? nullptr : actorPtr->GetActorBase()) {
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

        // The shared flush outfit is Tailor's too: a record left on it (a flush that went on before an outfit failed,
        // without OBody) is detached the same way. A restore can copy it into the sleep field as well.
        const bool defaultIsTailor =
            npc->defaultOutfit == pair.primary || npc->defaultOutfit == pair.alternate ||
            (_flushOutfit && npc->defaultOutfit == _flushOutfit);
        const bool sleepIsTailor =
            npc->sleepOutfit == pair.primary || npc->sleepOutfit == pair.alternate ||
            (_flushOutfit && npc->sleepOutfit == _flushOutfit);
        if (!defaultIsTailor && !sleepIsTailor) continue;

        // Without an assignment-held original (an NPC only a mod's override dressed), their own outfit goes back,
        // as RestoreOriginalOutfit resolves it: their own plugin record, else the data-load default. Read once, if needed.
        DefaultOutfitState own;
        std::optional<bool> ownKnown;
        const auto resolveOwn = [&] {
            if (!ownKnown) ownKnown = GetOriginRecordOutfitState(actor, own) || GetDataLoadedDefaultOutfitState(actor, own);
            return *ownKnown;
        };

        if (defaultIsTailor) {
            RE::BGSOutfit* original = nullptr;
            if (assignments.GetOriginalDefaultOutfit(actorId, original)) {
                npc->defaultOutfit = original;
            } else if (resolveOwn()) {
                npc->defaultOutfit = own.defaultOutfit;
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
            } else if (resolveOwn()) {
                npc->sleepOutfit = own.sleepOutfit;
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
    // The load replaces the player's inventory: forget a wardrobe preview without equipment work.
    Tailor::Player::PlayerWardrobe::GetSingleton().DropPreview();
    _playerTarget = false;
    _sessionNpc = RE::ActorHandle{};
    // Anyone still waiting to return to their own outfit got it back above, with Tailor's outfits taken off.
    _returnToOwnOutfit.clear();

    logger::info("OutfitManager: cleared runtime outfit forms for game load");
}
