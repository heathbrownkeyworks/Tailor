#include "compat/OBodyCompat.h"
#include "outfit/OutfitAssignments.h"

#include <Windows.h>

#include <chrono>

namespace
{
    class OBodyReadinessListener final : public OBody::API::IOBodyReadinessEventListener
    {
    public:
        void OBodyIsReady() override
        {
            OBodyCompat::GetSingleton().OnOBodyReady();
        }

        void OBodyIsNoLongerReady() override
        {
            OBodyCompat::GetSingleton().OnOBodyNoLongerReady();
        }
    };

    class OBodyActorChangeListener final : public OBody::API::IActorChangeEventListener
    {
    public:
        OnActorGenerated::Response OnActorGenerated(
            RE::Actor* actor,
            OnActorGenerated::Flags,
            OnActorGenerated::Payload& payload) override
        {
            OBodyCompat::GetSingleton().OnActorPresetEvent(
                actor,
                payload.presetName,
                payload.responsiblePluginInterface,
                false);
            return OnActorGenerated::Response::None;
        }

        OnActorPresetChangedWithoutGeneration::Response OnActorPresetChangedWithoutGeneration(
            RE::Actor* actor,
            OnActorPresetChangedWithoutGeneration::Flags flags,
            OnActorPresetChangedWithoutGeneration::Payload& payload) override
        {
            const bool unassigned =
                (static_cast<std::uint64_t>(flags) &
                    static_cast<std::uint64_t>(
                        OnActorPresetChangedWithoutGeneration::PresetWasUnassigned)) != 0;
            OBodyCompat::GetSingleton().OnActorPresetEvent(
                actor,
                payload.presetName,
                payload.responsiblePluginInterface,
                unassigned);
            return OnActorPresetChangedWithoutGeneration::Response::None;
        }

        OnActorMorphsCleared::Response OnActorMorphsCleared(
            RE::Actor* actor,
            OnActorMorphsCleared::Flags,
            OnActorMorphsCleared::Payload& payload) override
        {
            OBodyCompat::GetSingleton().OnActorPresetEvent(
                actor,
                {},
                payload.responsiblePluginInterface,
                true);
            return OnActorMorphsCleared::Response::None;
        }
    };

    OBodyReadinessListener g_readinessListener;
    OBodyActorChangeListener g_actorChangeListener;
}

OBodyCompat& OBodyCompat::GetSingleton()
{
    static OBodyCompat singleton;
    return singleton;
}

void OBodyCompat::DetectInstalled(const SKSE::LoadInterface* loadInterface)
{
    const auto* pluginInfo = loadInterface ? loadInterface->GetPluginInfo("OBody") : nullptr;
    _installed.store(pluginInfo != nullptr);

    if (pluginInfo) {
        logger::info(
            "OBodyCompat: detected SKSE plugin '{}' version 0x{:08X}",
            pluginInfo->name ? pluginInfo->name : "OBody",
            pluginInfo->version);
    } else {
        logger::info("OBodyCompat: OBody SKSE plugin not detected");
    }
}

void OBodyCompat::RequestAPI()
{
    if (!_installed.load()) {
        if (GetModuleHandleA("OBody.dll")) {
            _installed.store(true);
            logger::info("OBodyCompat: detected loaded OBody.dll at kPostPostLoad");
        } else {
            return;
        }
    }

    if (_apiRequested.exchange(true)) {
        return;
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        logger::warn("OBodyCompat: messaging interface unavailable; suppressing generic morph refresh");
        return;
    }

    OBody::API::SKSEMessages::RequestPluginInterface request{};
    request.version = OBody::API::PluginAPIVersion::Latest;
    request.pluginInterface = &_api;
    request.readinessEventListener = std::addressof(g_readinessListener);

    const bool dispatched = messaging->Dispatch(
        OBody::API::SKSEMessages::RequestPluginInterface::type,
        std::addressof(request),
        sizeof(request),
        "OBody");

    if (!dispatched || !_api) {
        logger::warn(
            "OBodyCompat: OBody is installed, but its plugin API did not respond. "
            "Tailor will suppress the generic NiNode morph refresh for OBody compatibility.");
        return;
    }

    logger::info("OBodyCompat: OBody plugin API acquired; waiting for readiness");
}

void OBodyCompat::OnOBodyReady()
{
    if (!_api) {
        return;
    }

    std::lock_guard lock(_stateMutex);
    _capturedPresets.clear();
    _legacyWaitStarted.clear();
    _preparedActors.clear();
    _intentionallyUnassignedActors.clear();
    _waitingLogged.clear();
    _readyAt = std::chrono::steady_clock::now();

    _api->SetOwner("Tailor");
    if (!_api->HasRegisteredEventListener(g_actorChangeListener)) {
        if (!_api->RegisterEventListener(g_actorChangeListener)) {
            logger::warn("OBodyCompat: could not register OBody actor-change listener");
        }
    }

    // OBody has just loaded its cosave registry. Copy the exact assignments now,
    // before actor initialization can replace a missing live RaceMenu marker via
    // normal (possibly random) distribution.
    const auto assignments = OutfitAssignments::GetSingleton().GetAll();
    for (const auto& [actorId, assignment] : assignments) {
        (void)assignment;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
        if (!actor) {
            continue;
        }

        OBody::API::PresetAssignmentInformation preset{};
        _api->GetPresetAssignedToActor(actor, preset);
        if (!preset.presetName.empty()) {
            _capturedPresets.emplace(actorId, std::string(preset.presetName));
        }
    }

    _apiReady.store(true);
    logger::info(
        "OBodyCompat: OBody API is ready; captured {} Tailor-managed preset assignment(s)",
        _capturedPresets.size());
}

void OBodyCompat::OnOBodyNoLongerReady()
{
    _apiReady.store(false);
    std::lock_guard lock(_stateMutex);
    _capturedPresets.clear();
    _legacyWaitStarted.clear();
    _preparedActors.clear();
    _intentionallyUnassignedActors.clear();
    _waitingLogged.clear();
    _readyAt = {};
    logger::info("OBodyCompat: OBody API is no longer ready");
}

void OBodyCompat::OnPreLoadGame()
{
    _apiReady.store(false);
    std::lock_guard lock(_stateMutex);
    _capturedPresets.clear();
    _legacyWaitStarted.clear();
    _preparedActors.clear();
    _intentionallyUnassignedActors.clear();
    _waitingLogged.clear();
    _readyAt = {};
}

void OBodyCompat::OnNewGame()
{
    // A new game can begin while OBody remains in its ready state, in which
    // case it does not emit another readiness transition. Rebuild Tailor's
    // per-game snapshot explicitly instead of carrying actor state forward.
    if (_api && _apiReady.load()) {
        OnOBodyReady();
        return;
    }

    std::lock_guard lock(_stateMutex);
    _capturedPresets.clear();
    _legacyWaitStarted.clear();
    _preparedActors.clear();
    _intentionallyUnassignedActors.clear();
    _waitingLogged.clear();
    _readyAt = {};
}

void OBodyCompat::OnActorPresetEvent(
    RE::Actor* actor,
    std::string_view presetName,
    OBody::API::IPluginInterfaceVersionIndependent* responsibleInterface,
    bool unassigned)
{
    if (!actor) return;
    if (responsibleInterface == _api) return;

    const auto actorId = actor->GetFormID();
    auto& assignments = OutfitAssignments::GetSingleton();
    if (!assignments.HasAssignment(actorId) && !assignments.HasAnySituation(actorId)) {
        return;
    }

    std::lock_guard lock(_stateMutex);

    // Events caused by Tailor's own exact restoration must not replace the
    // saved baseline. A null responsible interface is OBody's automatic
    // distribution; preserve an existing cosave snapshot across that too.
    if (!responsibleInterface) {
        if (!unassigned && !presetName.empty()) {
            _intentionallyUnassignedActors.erase(actorId);
            if (!_capturedPresets.contains(actorId)) {
                _capturedPresets.emplace(actorId, std::string(presetName));
            }
        }
        _preparedActors.erase(actorId);
        return;
    }

    if (unassigned || presetName.empty()) {
        _capturedPresets.erase(actorId);
        _intentionallyUnassignedActors.insert(actorId);
    } else {
        _capturedPresets[actorId] = std::string(presetName);
        _intentionallyUnassignedActors.erase(actorId);
    }
    _preparedActors.erase(actorId);
    logger::info(
        "OBodyCompat: recorded intentional OBody preset change for {} from '{}'",
        actor->GetDisplayFullName(),
        responsibleInterface->owner ? responsibleInterface->owner : "unknown");
}

bool OBodyCompat::PrepareActorForAutomaticOutfitChange(RE::Actor* actor)
{
    if (!_installed.load()) {
        return true;
    }
    if (!actor) {
        return false;
    }

    // Match OBody's own TESInit/equip-event eligibility filter. These actors
    // never receive an OBody processed marker, so waiting for one would block
    // their Tailor assignments forever.
    if (!actor->HasKeywordString("ActorTypeNPC") || actor->IsChild()) {
        return true;
    }

    constexpr auto kAPISettleTime = std::chrono::seconds(8);
    constexpr auto kLegacySettleTime = std::chrono::seconds(8);

    const auto actorId = actor->GetFormID();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(_stateMutex);

    // OBody 4.3.7 and earlier have no native API. Their performance path delays
    // setting the processed morph marker by 3-7 seconds, so wait beyond that
    // window. This is necessarily best-effort because those versions expose no
    // deterministic preset-restoration call.
    if (!_api) {
        if (_preparedActors.contains(actorId)) {
            return true;
        }

        auto [it, inserted] = _legacyWaitStarted.try_emplace(actorId, now);
        if (inserted) {
            logger::warn(
                "OBodyCompat: legacy OBody detected for {}; delaying automatic outfit changes for 8 seconds. "
                "Deterministic preset preservation requires OBody 4.4+",
                actor->GetDisplayFullName());
        }
        if (now - it->second < kLegacySettleTime) {
            return false;
        }

        _preparedActors.insert(actorId);
        _legacyWaitStarted.erase(it);
        return true;
    }

    if (!_apiReady.load() || _readyAt.time_since_epoch().count() == 0 ||
        now - _readyAt < kAPISettleTime) {
        if (_waitingLogged.insert(actorId).second) {
            logger::info(
                "OBodyCompat: holding automatic outfit change for {} until OBody and its Papyrus settings settle",
                actor->GetDisplayFullName());
        }
        return false;
    }

    OBody::API::PresetAssignmentInformation current{};
    _api->GetPresetAssignedToActor(actor, current);
    const std::string currentPreset(current.presetName);
    const bool processed = _api->ActorIsProcessed(actor);
    const bool blacklisted = _api->ActorIsBlacklisted(actor);

    // Respect an explicit API/Papyrus unassignment. OBody intentionally has no
    // processed marker in this state, but its armor event path is safe to skip.
    if (_intentionallyUnassignedActors.contains(actorId)) {
        _preparedActors.insert(actorId);
        _waitingLogged.erase(actorId);
        return true;
    }

    const bool wasPrepared = _preparedActors.contains(actorId);
    if (wasPrepared && processed) {
        const auto captured = _capturedPresets.find(actorId);
        if (captured == _capturedPresets.end()) {
            if (!currentPreset.empty()) {
                _capturedPresets.emplace(actorId, currentPreset);
            }
            _waitingLogged.erase(actorId);
            return true;
        }
        if (captured->second == currentPreset) {
            _waitingLogged.erase(actorId);
            return true;
        }
    }

    if (wasPrepared) {
        // Distribution-key resets and marker loss do not emit another API-ready
        // event. Preserve the last known preset and wait for OBody to rebuild
        // the marker before Tailor emits armor events.
        _preparedActors.erase(actorId);
    }

    OBody::API::PresetCounts counts{};
    _api->GetPresetCounts(counts);
    const auto* npc = actor->GetActorBase();
    const bool female = npc && npc->GetSex() == RE::SEX::kFemale;
    const bool hasPresetForSex = female ? counts.female > 0 : counts.male > 0;
    if (!hasPresetForSex) {
        _waitingLogged.erase(actorId);
        logger::info(
            "OBodyCompat: no OBody presets are available for {}; Tailor outfit change may proceed",
            actor->GetDisplayFullName());
        return true;
    }

    if (!currentPreset.empty() && !_capturedPresets.contains(actorId)) {
        _capturedPresets.emplace(actorId, currentPreset);
    }

    std::string presetToRestore;
    if (const auto captured = _capturedPresets.find(actorId); captured != _capturedPresets.end()) {
        presetToRestore = captured->second;
    } else if (!currentPreset.empty()) {
        presetToRestore = currentPreset;
    }

    // OBody ignores armor changes for blacklisted actors regardless of their
    // marker. Preserve a manually assigned captured preset when one exists,
    // but never wait on a marker that OBody itself does not require here.
    if (blacklisted &&
        (presetToRestore.empty() || (processed && currentPreset == presetToRestore))) {
        _preparedActors.insert(actorId);
        _waitingLogged.erase(actorId);
        return true;
    }

    if (!processed && !blacklisted && presetToRestore.empty()) {
        if (_waitingLogged.insert(actorId).second) {
            logger::info(
                "OBodyCompat: holding automatic outfit change for {} until OBody's processed marker is present",
                actor->GetDisplayFullName());
        }
        return false;
    }

    if (presetToRestore.empty()) {
        _preparedActors.insert(actorId);
        _waitingLogged.erase(actorId);
        logger::info(
            "OBodyCompat: {} is processed with no saved preset name; automatic outfit change may proceed",
            actor->GetDisplayFullName());
        return true;
    }

    if (currentPreset != presetToRestore || !processed) {
        OBody::API::AssignPresetPayload payload{};
        payload.flags = OBody::API::AssignPresetPayload::ForceImmediateApplicationOfMorphs;
        payload.presetName = presetToRestore;
        if (!_api->AssignPresetToActor(actor, payload)) {
            // The v1 API documents false as "preset name not found." If OBody
            // already has a valid processed preset, preserve that valid state
            // rather than blocking Tailor forever for a removed preset file.
            if (!currentPreset.empty() && _api->ActorIsProcessed(actor)) {
                _capturedPresets[actorId] = currentPreset;
                _preparedActors.insert(actorId);
                _waitingLogged.erase(actorId);
                logger::warn(
                    "OBodyCompat: saved preset '{}' is unavailable for {}; accepting OBody's current preset '{}'",
                    presetToRestore,
                    actor->GetDisplayFullName(),
                    currentPreset);
                return true;
            }

            logger::warn(
                "OBodyCompat: could not restore exact preset '{}' for {}; retaining automatic outfit retry",
                presetToRestore,
                actor->GetDisplayFullName());
            return false;
        }

        // OBody's return value only means that the preset name was found.
        // Verify the registry name and current-key marker after the exact
        // assignment before allowing Tailor's armor events to proceed.
        OBody::API::PresetAssignmentInformation restored{};
        _api->GetPresetAssignedToActor(actor, restored);
        if ((!blacklisted && !_api->ActorIsProcessed(actor)) ||
            restored.presetName != presetToRestore) {
            logger::info(
                "OBodyCompat: exact preset '{}' was accepted for {}, but OBody is not processed yet; retaining retry",
                presetToRestore,
                actor->GetDisplayFullName());
            return false;
        }

        logger::info(
            "OBodyCompat: restored exact preset '{}' for {} before Tailor outfit change",
            presetToRestore,
            actor->GetDisplayFullName());
    }

    _preparedActors.insert(actorId);
    _waitingLogged.erase(actorId);
    return true;
}

bool OBodyCompat::TryRefreshMorphs(RE::Actor*)
{
    if (!_installed.load()) {
        return false;
    }

    if (!_passiveModeLogged.exchange(true)) {
        logger::info(
            "OBodyCompat: leaving morph and ORefit updates to OBody's armor equip events; "
            "Tailor will not request a full preset reapplication");
    }

    return true;
}
