#include "wig/WigManager.h"
#include "events/SituationHandler.h"

namespace
{
    // Replace one hair-tint material on a geometry with a per-actor cloned copy.
    // Clone-not-mutate is mandatory: the engine de-dups hair materials by content,
    // so two actors wearing the same wig share ONE material object — mutating it in
    // place bleeds the color across both. Returns true if a retint happened.
    bool RetintHairGeometry(RE::BSGeometry* geom, const RE::NiColor& col)
    {
        if (!geom) {
            return false;
        }

        // Hair uses a BSLightingShaderProperty; cast helper lives in BSGeometry.h.
        auto* shader = geom->lightingShaderProp_cast();
        if (!shader || !shader->material) {
            return false;
        }

        if (shader->material->GetFeature() != RE::BSShaderMaterial::Feature::kHairTint) {
            return false;
        }

        auto* oldMat = static_cast<RE::BSLightingShaderMaterialHairTint*>(shader->material);
        if (oldMat->tintColor == col) {
            return false;  // already the right color — skip the churn
        }

        // Clone via the material's own virtual Create() (engine allocator), copy all
        // members so textures/params carry over, then set just the tint.
        auto* newMat = static_cast<RE::BSLightingShaderMaterialHairTint*>(oldMat->Create());
        if (!newMat) {
            return false;
        }
        newMat->CopyMembers(oldMat);   // copies oldMat's members INTO newMat
        newMat->tintColor = col;

        // false => engine content-hashes & de-dups into its own cache; it does NOT
        // adopt our pointer. Verified against kingeric1992/HairColourSyncNg.
        shader->SetMaterial(newMat, false);

        // The property now points at a cache-managed material, not newMat — free ours.
        // Guard against the theoretical "engine adopted it" branch to avoid UAF.
        if (shader->material != newMat) {
            delete newMat;
        }

        return true;
    }
}

WigManager& WigManager::GetSingleton()
{
    static WigManager singleton;
    return singleton;
}

void WigManager::Initialize()
{
    // Detect Nether's Follower Framework and cache faction forms.
    // Note: WigLibrary::Load() and WigAssignments::Load() are called
    // separately from main.cpp — not here.
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (dataHandler) {
        constexpr RE::FormID kNffStoredFacLocal = 0x0487D2;
        constexpr RE::FormID kNffOutfitFacLocal = 0x49BA78;
        constexpr auto       kNffPlugin         = "nwsFollowerFramework.esp"sv;

        auto* storedForm = dataHandler->LookupForm(kNffStoredFacLocal, kNffPlugin);
        auto* outfitForm = dataHandler->LookupForm(kNffOutfitFacLocal, kNffPlugin);

        if (storedForm && outfitForm) {
            _nffStoredFac = storedForm->As<RE::TESFaction>();
            _nffOutfitFac = outfitForm->As<RE::TESFaction>();
            _nffLoaded = (_nffStoredFac && _nffOutfitFac);
        }

        if (_nffLoaded) {
            logger::info("NFF detected — outfit conflict mitigation active");
        }
    }

    logger::info("WigManager initialized");
}

bool WigManager::IsNFFManaged(RE::Actor* actor) const
{
    if (!_nffLoaded || !actor) return false;
    return actor->IsInFaction(_nffStoredFac);
}

// --- Target Management (delegated from TailorUI) ---

void WigManager::SetTarget(RE::Actor* actor)
{
    _currentTarget = actor ? actor->GetHandle() : RE::ActorHandle{};
}

RE::Actor* WigManager::GetTarget() const
{
    auto actor = _currentTarget.get();
    if (actor && !actor->IsPlayerRef()) {
        return actor.get();
    }
    return nullptr;
}

// --- ArmorAddon Race Patching ---

void WigManager::EnsureArmorAddonRace(RE::TESObjectARMO* armor, RE::TESRace* race, RE::SEX sex)
{
    if (!armor || !race) return;

    if (armor->GetArmorAddon(race)) return;

    RE::TESObjectARMA* bestAA = nullptr;
    uint32_t bestRaceCount = 0;
    auto sexIdx = static_cast<std::uint32_t>(sex);

    for (auto* aa : armor->armorAddons) {
        if (!aa) continue;

        const char* model = aa->bipedModels[sexIdx].GetModel();
        if (!model || model[0] == '\0') continue;

        auto raceCount = static_cast<uint32_t>(aa->additionalRaces.size());
        if (!bestAA || raceCount > bestRaceCount) {
            bestAA = aa;
            bestRaceCount = raceCount;
        }
    }

    if (bestAA) {
        // Check if race is already in the list to avoid duplicates
        for (auto* existing : bestAA->additionalRaces) {
            if (existing == race) return;
        }
        bestAA->additionalRaces.push_back(race);
        logger::info("Patched AA 0x{:08X}: added race '{}' for wig '{}'",
            bestAA->GetFormID(),
            race->GetFormEditorID(),
            armor->GetFullName());
    } else {
        logger::warn("No suitable ArmorAddon found to patch for race '{}' on wig '{}'",
            race->GetFormEditorID(),
            armor->GetFullName());
    }
}

// --- Effective Inventory Count ---

int32_t WigManager::GetActorItemCount(RE::Actor* actor, RE::TESBoundObject* item)
{
    if (!actor || !item) return 0;

    auto* changes = actor->GetInventoryChanges();
    if (changes) {
        return changes->GetCount(item, [](const RE::InventoryEntryData*) { return true; });
    }

    auto* npc = actor->GetActorBase();
    return npc ? npc->GetObjectCount(item) : 0;
}

// --- Wig Operations ---

void WigManager::RemoveCurrentWig(RE::Actor* actor)
{
    auto& assignments = WigAssignments::GetSingleton();
    auto state = assignments.GetState(actor->GetFormID());
    if (!state || state->currentWig.formId == 0) {
        return;
    }

    auto* armor = state->currentWig.Resolve();
    if (!armor) {
        return;
    }

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (equipManager) {
        equipManager->UnequipObject(actor, armor);
    }

    // Take back the copy Tailor added; a copy the user gave the NPC stays put.
    if (state->itemAdded && GetActorItemCount(actor, armor) > 0) {
        actor->RemoveItem(armor, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    }
}

bool WigManager::EquipWig(RE::Actor* actor, const WigEntry& wig)
{
    if (!actor || !actor->Is3DLoaded()) {
        logger::warn("EquipWig: actor null or 3D not loaded");
        return false;
    }

    auto* armor = wig.Resolve();
    if (!armor) {
        logger::warn("Failed to resolve wig '{}' from plugin '{}'", wig.name, wig.plugin);
        return false;
    }

    auto* npc = actor->GetActorBase();
    if (!npc) {
        logger::warn("EquipWig: actor has no base form");
        return false;
    }

    auto& assignments = WigAssignments::GetSingleton();
    auto existingState = assignments.GetState(actor->GetFormID());
    auto* oldArmor = existingState ? existingState->currentWig.Resolve() : nullptr;
    auto* equipManager = RE::ActorEquipManager::GetSingleton();

    // Add to the actor REFERENCE's inventory, never the TESNPC base container.
    // Base-container edits get re-seeded into the real inventory by every engine
    // inventory re-init (outfit changes, actor resets), duplicating the item.
    bool addedNow = false;
    if (GetActorItemCount(actor, armor) <= 0) {
        actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
        addedNow = true;
    }

    auto* actorRace = npc->GetRace();
    auto actorSex = npc->GetSex();
    EnsureArmorAddonRace(armor, actorRace, actorSex);

    if (equipManager) {
        equipManager->EquipObject(actor, armor, nullptr, 1, nullptr, true, false, false, false);
    }

    // Retire the previous wig: unequip it and take back the copy Tailor added.
    if (oldArmor && oldArmor != armor) {
        if (equipManager) {
            equipManager->UnequipObject(actor, oldArmor);
        }
        if (existingState->itemAdded && GetActorItemCount(actor, oldArmor) > 0) {
            actor->RemoveItem(oldArmor, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }
    }

    bool itemAdded = addedNow || (oldArmor == armor && existingState && existingState->itemAdded);
    assignments.SetAssignment(actor->GetFormID(), wig, itemAdded);
    logger::info("Assigned wig '{}' to actor 0x{:08X}{}", wig.name, actor->GetFormID(),
        IsNFFManaged(actor) ? " (NFF managed — wig may be lost on NFF outfit change)" : "");

    return true;
}

bool WigManager::ResetWig(RE::Actor* actor)
{
    if (!actor) {
        return false;
    }

    RemoveCurrentWig(actor);
    WigAssignments::GetSingleton().ClearAssignment(actor->GetFormID());
    WigAssignments::GetSingleton().Save();

    logger::info("Reset wig for {}", actor->GetDisplayFullName());
    return true;
}

// --- Cycling ---

bool WigManager::StartCycling(RE::Actor* actor, WigCategory category)
{
    std::lock_guard lock(_mutex);

    auto& library = WigLibrary::GetSingleton();
    auto wigs = library.GetCategory(category);
    if (wigs.empty()) {
        return false;
    }

    CycleState state;
    state.category = category;
    state.index = 0;

    auto existing = WigAssignments::GetSingleton().GetAssignment(actor->GetFormID());
    if (existing) {
        state.originalWig = *existing;
        state.hadOriginal = true;

        for (int32_t i = 0; i < static_cast<int32_t>(wigs.size()); ++i) {
            if (wigs[i].formId == existing->formId && wigs[i].plugin == existing->plugin) {
                state.index = i;
                _cycleState = state;
                return true;
            }
        }
    }

    _cycleState = state;
    EquipWig(actor, wigs[0]);

    return true;
}

std::optional<WigEntry> WigManager::CycleNext()
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return std::nullopt;
    }

    auto& library = WigLibrary::GetSingleton();
    auto wigs = library.GetCategory(_cycleState->category);
    if (wigs.empty()) {
        return std::nullopt;
    }

    _cycleState->index = (_cycleState->index + 1) % static_cast<int32_t>(wigs.size());
    auto& wig = wigs[_cycleState->index];

    auto* target = GetTarget();
    if (target) {
        EquipWig(target, wig);
    }

    return wig;
}

std::optional<WigEntry> WigManager::CyclePrev()
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return std::nullopt;
    }

    auto& library = WigLibrary::GetSingleton();
    auto wigs = library.GetCategory(_cycleState->category);
    if (wigs.empty()) {
        return std::nullopt;
    }

    auto count = static_cast<int32_t>(wigs.size());
    _cycleState->index = (_cycleState->index - 1 + count) % count;
    auto& wig = wigs[_cycleState->index];

    auto* target = GetTarget();
    if (target) {
        EquipWig(target, wig);
    }

    return wig;
}

std::optional<WigEntry> WigManager::CycleToIndex(int32_t index)
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return std::nullopt;
    }

    auto& library = WigLibrary::GetSingleton();
    auto wigs = library.GetCategory(_cycleState->category);
    if (wigs.empty() || index < 0 || index >= static_cast<int32_t>(wigs.size())) {
        return std::nullopt;
    }

    _cycleState->index = index;
    auto& wig = wigs[_cycleState->index];

    auto* target = GetTarget();
    if (target) {
        EquipWig(target, wig);
    }

    return wig;
}

std::optional<WigEntry> WigManager::GetCurrentCycleWig() const
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return std::nullopt;
    }

    auto& library = WigLibrary::GetSingleton();
    auto wigs = library.GetCategory(_cycleState->category);
    auto count = static_cast<int32_t>(wigs.size());

    if (wigs.empty() || _cycleState->index < 0 || _cycleState->index >= count) {
        return std::nullopt;
    }

    return wigs[_cycleState->index];
}

int32_t WigManager::GetCycleIndex() const
{
    std::lock_guard lock(_mutex);
    return _cycleState ? _cycleState->index : -1;
}

int32_t WigManager::GetCycleCount() const
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return 0;
    }
    return static_cast<int32_t>(WigLibrary::GetSingleton().GetCategoryCount(_cycleState->category));
}

std::vector<WigEntry> WigManager::GetCycleWigs() const
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return {};
    }
    return WigLibrary::GetSingleton().GetCategory(_cycleState->category);
}

void WigManager::ConfirmCycle()
{
    std::lock_guard lock(_mutex);
    _cycleState.reset();
    logger::info("Wig cycling confirmed");
    WigAssignments::GetSingleton().Save();

    auto* target = GetTarget();
    if (target) {
        auto state = WigAssignments::GetSingleton().GetState(target->GetFormID());
        if (state && state->HasHairColor()) {
            auto handle = target->GetHandle();
            auto cr = static_cast<uint8_t>(state->hairColorR);
            auto cg = static_cast<uint8_t>(state->hairColorG);
            auto cb = static_cast<uint8_t>(state->hairColorB);
            ApplyHairColor(target, cr, cg, cb);
            DeferHairColor(handle, cr, cg, cb, 1);
        }
        ScheduleActorHairRetint(target->GetHandle(), {1, 5, 12});
    }
}

void WigManager::ConfirmCycle(OutfitSituation situation)
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) return;

    auto* target = GetTarget();
    if (!target) return;

    auto& library = WigLibrary::GetSingleton();
    auto wigs = library.GetCategory(_cycleState->category);
    if (_cycleState->index < 0 || _cycleState->index >= static_cast<int32_t>(wigs.size())) return;

    auto& wig = wigs[_cycleState->index];

    // Save to situational assignment (clears legacy)
    auto& assignments = WigAssignments::GetSingleton();
    assignments.AssignSituation(target->GetFormID(), situation, wig);
    assignments.SaveSituations();

    _cycleState.reset();
    logger::info("Wig cycling confirmed for situation {} — '{}'", static_cast<int>(situation), wig.name);

    // Apply correct wig for current situation (not necessarily the one just confirmed)
    SituationHandler::GetSingleton()->ForceApplyForSituation(target);

    // Re-apply hair color
    auto state = assignments.GetState(target->GetFormID());
    if (state && state->HasHairColor()) {
        auto handle = target->GetHandle();
        auto cr = static_cast<uint8_t>(state->hairColorR);
        auto cg = static_cast<uint8_t>(state->hairColorG);
        auto cb = static_cast<uint8_t>(state->hairColorB);
        ApplyHairColor(target, cr, cg, cb);
        DeferHairColor(handle, cr, cg, cb, 1);
    }

    ScheduleActorHairRetint(target->GetHandle(), {1, 5, 12});
}

void WigManager::CancelCycle()
{
    std::lock_guard lock(_mutex);
    if (!_cycleState) {
        return;
    }

    auto* target = GetTarget();
    if (target) {
        if (_cycleState->hadOriginal) {
            EquipWig(target, _cycleState->originalWig);
        } else {
            ResetWig(target);
        }
        auto state = WigAssignments::GetSingleton().GetState(target->GetFormID());
        if (state && state->HasHairColor()) {
            auto handle = target->GetHandle();
            auto cr = static_cast<uint8_t>(state->hairColorR);
            auto cg = static_cast<uint8_t>(state->hairColorG);
            auto cb = static_cast<uint8_t>(state->hairColorB);
            ApplyHairColor(target, cr, cg, cb);
            DeferHairColor(handle, cr, cg, cb, 1);
        }
        ScheduleActorHairRetint(target->GetHandle(), {1, 5, 12});
    }

    _cycleState.reset();
    logger::info("Wig cycling cancelled");
}

bool WigManager::IsCycling() const
{
    std::lock_guard lock(_mutex);
    return _cycleState.has_value();
}

// --- Preview ---

void WigManager::StartPreview()
{
    std::lock_guard lock(_mutex);
    if (_previewState) return;

    auto* target = GetTarget();
    if (!target) return;

    PreviewState state;
    auto existing = WigAssignments::GetSingleton().GetAssignment(target->GetFormID());
    if (existing) {
        state.originalWig = *existing;
        state.hadOriginal = true;
    }
    _previewState = state;
    logger::info("Preview started for {}", target->GetDisplayFullName());
}

bool WigManager::PreviewWig(RE::Actor* actor, const WigEntry& wig)
{
    std::lock_guard lock(_mutex);
    if (!_previewState) {
        PreviewState state;
        auto existing = WigAssignments::GetSingleton().GetAssignment(actor->GetFormID());
        if (existing) {
            state.originalWig = *existing;
            state.hadOriginal = true;
        }
        _previewState = state;
    }
    return EquipWig(actor, wig);
}

void WigManager::EndPreview()
{
    std::lock_guard lock(_mutex);
    if (!_previewState) return;

    auto* target = GetTarget();
    if (target) {
        if (_previewState->hadOriginal) {
            EquipWig(target, _previewState->originalWig);
        } else {
            ResetWig(target);
        }
        auto state = WigAssignments::GetSingleton().GetState(target->GetFormID());
        if (state && state->HasHairColor()) {
            auto handle = target->GetHandle();
            auto cr = static_cast<uint8_t>(state->hairColorR);
            auto cg = static_cast<uint8_t>(state->hairColorG);
            auto cb = static_cast<uint8_t>(state->hairColorB);
            ApplyHairColor(target, cr, cg, cb);
            DeferHairColor(handle, cr, cg, cb, 1);
        }
        ScheduleActorHairRetint(target->GetHandle(), {1, 5, 12});
    }

    _previewState.reset();
    logger::info("Preview ended");
}

bool WigManager::IsPreviewing() const
{
    std::lock_guard lock(_mutex);
    return _previewState.has_value();
}

// --- Mod Scanning ---

std::vector<ModWigList> WigManager::ScanAllModWigs() const
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::warn("ScanAllModWigs: no data handler");
        return {};
    }

    static const std::set<std::string_view> kSkipPlugins = {
        "Skyrim.esm", "Update.esm", "Dawnguard.esm",
        "HearthFires.esm", "Dragonborn.esm"
    };

    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

    std::map<std::string, std::vector<InventoryWig>> modMap;

    auto& armors = dataHandler->GetFormArray<RE::TESObjectARMO>();
    int scanned = 0;
    int matched = 0;

    for (auto* armor : armors) {
        if (!armor) continue;
        scanned++;

        if (!armor->HasPartOf(Slot::kHair)) {
            continue;
        }

        if (armor->HasPartOf(Slot::kHead) || armor->HasPartOf(Slot::kBody)) {
            continue;
        }

        const char* fullName = armor->GetFullName();
        if (!fullName || fullName[0] == '\0') continue;

        auto* file = armor->GetFile(0);
        if (!file) continue;

        std::string pluginName(file->GetFilename());
        if (kSkipPlugins.contains(pluginName)) continue;

        matched++;

        InventoryWig wig;
        wig.formId = armor->GetLocalFormID();
        wig.plugin = std::move(pluginName);
        wig.name = SanitizeUtf8(fullName);

        modMap[wig.plugin].push_back(std::move(wig));
    }

    std::vector<ModWigList> result;
    result.reserve(modMap.size());

    for (auto& [modName, wigs] : modMap) {
        std::sort(wigs.begin(), wigs.end(), [](const InventoryWig& a, const InventoryWig& b) {
            return a.name < b.name;
        });
        result.push_back({modName, std::move(wigs)});
    }

    std::sort(result.begin(), result.end(), [](const ModWigList& a, const ModWigList& b) {
        return a.modName < b.modName;
    });

    logger::info("ScanAllModWigs: scanned {} armors, {} hair-slot matches across {} mods",
        scanned, matched, result.size());

    return result;
}

// --- Batch Wig Cleanup ---

void WigManager::ProcessDeferredRemoval([[maybe_unused]] RE::Actor* actor)
{
    // Reserved for future use — cell-detach cleanup hook.
}

void WigManager::DeferHairColor(RE::ActorHandle handle, uint8_t r, uint8_t g, uint8_t b, int32_t delaySecs)
{
    // Single-delay convenience overload — bumps generation so it invalidates prior batches
    ScheduleHairColorDefers(handle, r, g, b, {delaySecs});
}

void WigManager::ScheduleHairColorDefers(RE::ActorHandle handle, uint8_t r, uint8_t g, uint8_t b,
                                          std::initializer_list<int32_t> delays)
{
    auto actorPtr = handle.get();
    RE::FormID actorId = actorPtr ? actorPtr->GetFormID() : 0;
    uint32_t gen = 0;
    if (actorId) {
        std::lock_guard lock(_mutex);
        gen = ++_hairColorGeneration[actorId];
    }

    for (auto delaySecs : delays) {
        std::thread([this, handle, r, g, b, delaySecs, actorId, gen]() {
            if (delaySecs > 0) {
                std::this_thread::sleep_for(std::chrono::seconds(delaySecs));
            }

            SKSE::GetTaskInterface()->AddTask([this, handle, r, g, b, delaySecs, actorId, gen]() {
                if (actorId) {
                    std::lock_guard lock(_mutex);
                    if (_hairColorGeneration[actorId] != gen) {
                        return;
                    }
                }

                auto actorRef = handle.get();
                if (!actorRef) {
                    return;
                }
                auto* actor = actorRef.get();
                if (!actor || !actor->Is3DLoaded()) {
                    return;
                }

                if (ApplyHairColor(actor, r, g, b)) {
                    logger::info("DeferHairColor: applied color ({},{},{}) to {} (after {}s delay)",
                        r, g, b, actor->GetDisplayFullName(), delaySecs);
                }
            });
        }).detach();
    }
}

void WigManager::ScheduleActorHairRetint(RE::ActorHandle handle, std::initializer_list<int32_t> delays)
{
    auto actorPtr = handle.get();
    RE::FormID actorId = actorPtr ? actorPtr->GetFormID() : 0;
    uint32_t gen = 0;
    if (actorId) {
        std::lock_guard lock(_mutex);
        gen = ++_hairRetintGeneration[actorId];
    }

    for (auto delaySecs : delays) {
        std::thread([this, handle, delaySecs, actorId, gen]() {
            if (delaySecs > 0) {
                std::this_thread::sleep_for(std::chrono::seconds(delaySecs));
            }

            SKSE::GetTaskInterface()->AddTask([this, handle, actorId, gen]() {
                if (actorId) {
                    std::lock_guard lock(_mutex);
                    if (_hairRetintGeneration[actorId] != gen) {
                        return;  // superseded by a newer schedule
                    }
                }

                auto actorRef = handle.get();
                if (!actorRef) {
                    return;
                }
                auto* actor = actorRef.get();
                if (!actor || !actor->Is3DLoaded()) {
                    return;
                }

                RetintActorHair(actor);
            });
        }).detach();
    }
}

void WigManager::ReEquipAllAssignments()
{
    auto& assignments = WigAssignments::GetSingleton();

    for (auto& [actorFormId, state] : assignments.GetAll()) {
        auto* form = RE::TESForm::LookupByID(actorFormId);
        if (!form) {
            continue;
        }

        auto* actor = form->As<RE::Actor>();
        if (!actor) {
            continue;
        }

        if (state.currentWig.formId != 0 && !state.currentWig.plugin.empty()) {
            auto* armor = state.currentWig.Resolve();
            if (armor) {
                auto* npc = actor->GetActorBase();
                if (GetActorItemCount(actor, armor) <= 0) {
                    actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
                    assignments.MarkItemAdded(actor->GetFormID());
                }

                if (npc) {
                    EnsureArmorAddonRace(armor, npc->GetRace(), npc->GetSex());
                }

                if (actor->Is3DLoaded()) {
                    auto* equipManager = RE::ActorEquipManager::GetSingleton();
                    if (equipManager) {
                        equipManager->EquipObject(actor, armor, nullptr, 1, nullptr, true, false, false, false);
                    }
                }
            }
        }

        if (state.HasHairColor()) {
            auto handle = actor->GetHandle();
            auto r = static_cast<uint8_t>(state.hairColorR);
            auto g = static_cast<uint8_t>(state.hairColorG);
            auto b = static_cast<uint8_t>(state.hairColorB);
            ScheduleHairColorDefers(handle, r, g, b, {1, 5, 12});
        }

        if (state.currentWig.formId != 0 || state.HasHairColor()) {
            ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
        }
    }

    // Situational wig re-equip
    auto allSituational = WigAssignments::GetSingleton().GetAllSituational();
    for (auto& [actorFormId, wsa] : allSituational) {
        if (!wsa.HasAnySituation()) continue;

        auto* form = RE::TESForm::LookupByID(actorFormId);
        if (!form) continue;
        auto* actor = form->As<RE::Actor>();
        if (!actor) continue;

        SituationHandler::GetSingleton()->ClearCachedSituation(actorFormId);
        if (actor->Is3DLoaded()) {
            SituationHandler::GetSingleton()->ApplyForSituation(actor);
        }

        // Hair color (per-actor, not per-situation)
        auto state = WigAssignments::GetSingleton().GetState(actorFormId);
        if (state && state->HasHairColor()) {
            auto handle = actor->GetHandle();
            auto r = static_cast<uint8_t>(state->hairColorR);
            auto g = static_cast<uint8_t>(state->hairColorG);
            auto b = static_cast<uint8_t>(state->hairColorB);
            ScheduleHairColorDefers(handle, r, g, b, {1, 5, 12});
        }

        ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
    }

    logger::info("Re-equipped all wig assignments after game load");
}

void WigManager::ReApplyAllHairColors()
{
    auto& assignments = WigAssignments::GetSingleton();

    for (auto& [actorFormId, state] : assignments.GetAll()) {
        const bool hasWig = state.currentWig.formId != 0 && !state.currentWig.plugin.empty();
        if (!state.HasHairColor() && !hasWig) {
            continue;
        }

        auto* form = RE::TESForm::LookupByID(actorFormId);
        if (!form) continue;
        auto* actor = form->As<RE::Actor>();
        if (!actor) continue;

        auto handle = actor->GetHandle();
        if (state.HasHairColor()) {
            auto r = static_cast<uint8_t>(state.hairColorR);
            auto g = static_cast<uint8_t>(state.hairColorG);
            auto b = static_cast<uint8_t>(state.hairColorB);
            ScheduleHairColorDefers(handle, r, g, b, {1, 5});
        }
        ScheduleActorHairRetint(handle, {1, 5});
    }
}

// --- Outfit Change Integration ---

void WigManager::ReEquipWigAfterOutfitChange(RE::Actor* actor)
{
    if (!actor) return;

    auto state = WigAssignments::GetSingleton().GetState(actor->GetFormID());
    if (!state) return;

    if (state->currentWig.formId != 0 && !state->currentWig.plugin.empty()) {
        EquipWig(actor, state->currentWig);
        logger::info("Re-equipped wig '{}' on {} after outfit change",
            state->currentWig.name, actor->GetDisplayFullName());
    }

    if (state->HasHairColor()) {
        auto handle = actor->GetHandle();
        auto r = static_cast<uint8_t>(state->hairColorR);
        auto g = static_cast<uint8_t>(state->hairColorG);
        auto b = static_cast<uint8_t>(state->hairColorB);
        ApplyHairColor(actor, r, g, b);
        ScheduleHairColorDefers(handle, r, g, b, {1, 5, 12});
    }

    ScheduleActorHairRetint(actor->GetHandle(), {1, 5, 12});
}

// --- Per-Actor Hair Retint ---

bool WigManager::ResolveHairTint(RE::Actor* actor, RE::NiColor& out) const
{
    if (!actor) {
        return false;
    }

    // 1) Tailor custom color, if set.
    auto state = WigAssignments::GetSingleton().GetState(actor->GetFormID());
    if (state && state->HasHairColor()) {
        out.red   = static_cast<float>(state->hairColorR) / 128.0f;
        out.green = static_cast<float>(state->hairColorG) / 128.0f;
        out.blue  = static_cast<float>(state->hairColorB) / 128.0f;
        return true;
    }

    // 2) Otherwise the NPC's natural hair color from the base record.
    auto* npc = actor->GetActorBase();
    if (npc && npc->headRelatedData && npc->headRelatedData->hairColor) {
        const auto& c = npc->headRelatedData->hairColor->color;
        out.red   = static_cast<float>(c.red)   / 128.0f;
        out.green = static_cast<float>(c.green) / 128.0f;
        out.blue  = static_cast<float>(c.blue)  / 128.0f;
        return true;
    }

    return false;
}

void WigManager::RetintActorHair(RE::Actor* actor)
{
    if (!actor || !actor->Is3DLoaded()) {
        return;
    }

    auto* root = actor->Get3D();
    if (!root) {
        return;
    }

    RE::NiColor col;
    if (!ResolveHairTint(actor, col)) {
        return;  // no custom and no natural color to apply
    }

    int retinted = 0;
    RE::BSVisit::TraverseScenegraphGeometries(root,
        [&col, &retinted](RE::BSGeometry* geom) -> RE::BSVisit::BSVisitControl {
            if (RetintHairGeometry(geom, col)) {
                ++retinted;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

    if (retinted > 0) {
        auto state = WigAssignments::GetSingleton().GetState(actor->GetFormID());
        const bool custom = state && state->HasHairColor();
        logger::info("RetintActorHair: {} — {} hair material(s) retinted ({})",
            actor->GetDisplayFullName(), retinted, custom ? "custom" : "natural");
    }
}

void WigManager::RetintNearbyActors(RE::Actor* except)
{
    auto* processLists = RE::ProcessLists::GetSingleton();
    if (!processLists) {
        return;
    }

    for (auto& handle : processLists->highActorHandles) {
        auto ptr = handle.get();
        if (!ptr) {
            continue;
        }
        auto* actor = ptr.get();
        if (!actor || actor == except || !actor->Is3DLoaded()) {
            continue;
        }
        RetintActorHair(actor);
    }
}

// --- Hair Color ---

bool WigManager::ApplyHairColor(RE::Actor* actor, uint8_t r, uint8_t g, uint8_t b)
{
    if (!actor || !actor->Is3DLoaded()) {
        logger::warn("ApplyHairColor: actor null or 3D not loaded");
        return false;
    }

    auto* npc = actor->GetActorBase();
    if (!npc) {
        logger::warn("ApplyHairColor: actor has no base form");
        return false;
    }

    auto actorId = actor->GetFormID();
    {
        std::lock_guard lock(_mutex);
        if (!_originalHairColors.contains(actorId)) {
            if (npc->headRelatedData && npc->headRelatedData->hairColor) {
                _originalHairColors[actorId] = npc->headRelatedData->hairColor;
            }
        }
    }

    // Reuse cached color form per actor to avoid leaking engine forms
    RE::BGSColorForm* colorForm = nullptr;
    {
        std::lock_guard lock(_mutex);
        auto it = _cachedColorForms.find(actorId);
        if (it != _cachedColorForms.end()) {
            colorForm = it->second;
        }
    }

    if (!colorForm) {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>();
        if (!factory) {
            logger::error("ApplyHairColor: failed to get BGSColorForm factory");
            return false;
        }

        colorForm = factory->Create();
        if (!colorForm) {
            logger::error("ApplyHairColor: failed to create BGSColorForm");
            return false;
        }

        std::lock_guard lock(_mutex);
        _cachedColorForms[actorId] = colorForm;
    }

    colorForm->color = RE::Color(r, g, b, 0);
    npc->SetHairColor(colorForm);

    // Deliberately NOT actor->UpdateHairColor(): vanilla writes tintColor into the
    // hair-tint material in place, and the engine de-dups those materials by content
    // hash — so it bleeds the color onto every other actor wearing the same wig NIF.
    // RetintActorHair clones per actor instead. The base record is written above first,
    // so ResolveHairTint sees this color even before the assignment is confirmed
    // (live color-picker preview).
    RetintActorHair(actor);

    logger::info("Applied hair color ({}, {}, {}) to {}", r, g, b, actor->GetDisplayFullName());
    return true;
}

bool WigManager::ResetHairColor(RE::Actor* actor)
{
    if (!actor) {
        return false;
    }

    auto* npc = actor->GetActorBase();
    if (!npc) {
        return false;
    }

    auto actorId = actor->GetFormID();

    RE::BGSColorForm* original = nullptr;
    {
        std::lock_guard lock(_mutex);
        auto it = _originalHairColors.find(actorId);
        if (it != _originalHairColors.end()) {
            original = it->second;
            _originalHairColors.erase(it);
        }
    }

    if (original) {
        npc->SetHairColor(original);
        if (actor->Is3DLoaded()) {
            // Same reason as ApplyHairColor — never UpdateHairColor(). Callers must clear
            // the actor's WigAssignments hair color BEFORE this, or ResolveHairTint will
            // resolve the custom color we're trying to undo instead of the natural one.
            RetintActorHair(actor);
        }
        logger::info("Restored original hair color for {}", actor->GetDisplayFullName());
    }

    return true;
}

void WigManager::ReApplyHairColor(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    auto state = WigAssignments::GetSingleton().GetState(actor->GetFormID());
    if (!state || !state->HasHairColor()) {
        return;
    }

    ApplyHairColor(actor,
        static_cast<uint8_t>(state->hairColorR),
        static_cast<uint8_t>(state->hairColorG),
        static_cast<uint8_t>(state->hairColorB));
}
