#include "preview/TailorPreviewSession.h"
#include "compat/SmoothCamCompat.h"
#include "ui/TailorUI.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace Tailor::Preview
{
    namespace
    {
        constexpr float kStageFOV = 55.0f;
        constexpr float kPi = 3.14159265358979323846f;
        bool IsFinite(const RE::NiPoint3& a_point)
        {
            return std::isfinite(a_point.x) && std::isfinite(a_point.y) &&
                   std::isfinite(a_point.z);
        }

        bool NearlyEqual(float a_lhs, float a_rhs, float a_epsilon = 0.001f)
        {
            return std::abs(a_lhs - a_rhs) <= a_epsilon;
        }

        bool NearlyEqual(const RE::NiPoint3& a_lhs, const RE::NiPoint3& a_rhs)
        {
            return NearlyEqual(a_lhs.x, a_rhs.x) && NearlyEqual(a_lhs.y, a_rhs.y) &&
                   NearlyEqual(a_lhs.z, a_rhs.z);
        }

        bool NearlyEqual(
            const RE::BSTPoint2<float>& a_lhs,
            const RE::BSTPoint2<float>& a_rhs)
        {
            return NearlyEqual(a_lhs.x, a_rhs.x) && NearlyEqual(a_lhs.y, a_rhs.y);
        }


        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        struct CursorMenuAdvanceMovieHook
        {
            static void thunk(RE::CursorMenu* menu, float interval, std::uint32_t time)
            {
                func(menu, interval, time);
                auto& session = TailorPreviewSession::GetSingleton();
                if (session.IsActive()) session.Tick(session.Generation());
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    bool ViewportRect::IsValid() const noexcept
    {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
            std::isfinite(height) && x >= 0 && y >= 0 && width >= 0.05f &&
            height >= 0.05f && x + width <= 1.001f && y + height <= 1.001f;
    }
    TailorPreviewSession& TailorPreviewSession::GetSingleton()
    {
        static TailorPreviewSession singleton;
        return singleton;
    }
    void TailorPreviewSession::InstallHooks()
    {
        static std::once_flag installed;
        std::call_once(installed, []() {
            if (REL::Module::IsVR()) return;
            REL::Relocation<std::uintptr_t> table{RE::VTABLE_CursorMenu[0]};
            CursorMenuAdvanceMovieHook::func = table.write_vfunc(0x5, CursorMenuAdvanceMovieHook::thunk);
            logger::info("Tailor live actor preview frame hook installed");
        });
    }
    void TailorPreviewSession::RegisterEvents()
    {
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) source->AddEventSink<RE::TESEquipEvent>(this);
        if (auto* source = SKSE::GetNiNodeUpdateEventSource()) source->AddEventSink(this);
    }
    void TailorPreviewSession::NotifyAppearanceChanged(RE::Actor* actor)
    {
        // Callbacks can arrive inside manager locks; only publish an invalidation.
        if (actor && actor->GetFormID() == _targetFormID.load()) ++_appearanceRevision;
    }
    RE::BSEventNotifyControl TailorPreviewSession::ProcessEvent(
        const RE::TESEquipEvent* event, RE::BSTEventSource<RE::TESEquipEvent>*)
    {
        if (event && event->actor) NotifyAppearanceChanged(event->actor->As<RE::Actor>());
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl TailorPreviewSession::ProcessEvent(
        const SKSE::NiNodeUpdateEvent* event, RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*)
    {
        if (event && event->reference) NotifyAppearanceChanged(event->reference->As<RE::Actor>());
        return RE::BSEventNotifyControl::kContinue;
    }

    bool TailorPreviewSession::Begin(RE::ActorHandle a_target)
    {
        std::scoped_lock lock(_mutex);
        if (_policy.IsActive()) return _target == a_target;
        auto target = a_target.get();
        auto* actor = target.get();
        if (!actor || actor->IsDead() || actor->IsPlayerRef() || !actor->Is3DLoaded() || REL::Module::IsVR()) {
            SetStatus(false, "Select a loaded NPC to preview.");
            return false;
        }
        if (!_policy.Begin()) return false;
        _target = a_target;
        _viewport = {};
        _lastRoot = actor->Get3D(false);
        _missing3DSince = 0;
        _observedRevision = _appearanceRevision.load();
        _targetFormID.store(actor->GetFormID());
        _refresh = {};
        _cameraApplied = false;
        _framingValid = false;
        _baseApproach = {};
        AcquireTargetHold(actor);
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto& flags = player->GetGameStatsData().byCharGenFlag;
            if (!flags.any(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving)) {
                flags.set(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving);
                _policy.Acquire(Ownership::SavingDisabled);
            }
        }
        SetStatus(false, "Preparing live NPC preview...");
        (void)_policy.Activate();
        logger::info("Tailor live actor preview: target={:08X}, AI was enabled={}",
            actor->GetFormID(), _aiHold.WasEnabled());
        return true;
    }
    void TailorPreviewSession::AcquireTargetHold(RE::Actor* actor)
    {
        _aiHold.Acquire(*actor);
        if (actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kAlive) {
            actor->SetLifeState(RE::ACTOR_LIFE_STATE::kRestrained);
            _policy.Acquire(Ownership::Restrained);
        }
        auto& flags = actor->GetActorRuntimeData().boolFlags;
        if (!flags.any(RE::Actor::BOOL_FLAGS::kMovementBlocked)) {
            flags.set(RE::Actor::BOOL_FLAGS::kMovementBlocked);
            _policy.Acquire(Ownership::MovementBlocked);
        }
    }
    void TailorPreviewSession::EnforceMovementHold(RE::Actor* actor)
    {
        actor->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        if (_policy.Owns(Ownership::Restrained) && actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kAlive) {
            actor->SetLifeState(RE::ACTOR_LIFE_STATE::kRestrained);
        }
        if (auto* controller = actor->GetCharController()) controller->SetLinearVelocityImpl(0.0f);
    }
    void TailorPreviewSession::ReleaseTargetHold(RE::Actor* actor) noexcept
    {
        if (actor) {
            if (_policy.Owns(Ownership::MovementBlocked)) {
                actor->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);
            }
            if (_policy.Owns(Ownership::Restrained) && actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained) {
                actor->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
            }
            _aiHold.Restore(*actor);
            logger::info("Tailor restored target {:08X}: AI enabled={}", actor->GetFormID(), actor->IsAIEnabled());
        }
        _aiHold.Reset();
    }
    void TailorPreviewSession::End(EndReason reason) noexcept
    {
        std::scoped_lock lock(_mutex);
        if (!_policy.BeginTeardown()) return;
        _targetFormID.store(0);
        _scene.End();
        ReleaseCamera();
        auto target = _target.get();
        ReleaseTargetHold(target.get());
        if (_policy.Owns(Ownership::SavingDisabled)) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                player->GetGameStatsData().byCharGenFlag.reset(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving);
            }
        }
        _target = {};
        _lastRoot = nullptr;
        _refresh = {};
        _viewport = {};
        SetStatus(false, "");
        _policy.FinishTeardown();
        logger::info("Tailor live actor preview ended (reason={})", static_cast<unsigned>(reason));
    }

    void TailorPreviewSession::SetViewport(ViewportRect viewport)
    {
        if (!viewport.IsValid()) return;
        std::scoped_lock lock(_mutex);
        if (!_policy.IsActive()) return;
        _viewport = viewport;
        _framingValid = false;
    }

    void TailorPreviewSession::Tick(std::uint64_t generation)
    {
        auto& ui = TailorUI::GetSingleton();
        if (!ui.IsOpen() || !ui.HasFocus()) {
            ui.CloseForLifecycle(ui.IsOpen() ? EndReason::FocusLost : EndReason::UserClose);
            return;
        }
        std::unique_lock lock(_mutex);
        if (!_policy.IsActive() || !_policy.Accepts(generation)) return;
        auto target = _target.get();
        auto* actor = target.get();
        const auto close = [&](EndReason reason) {
            lock.unlock();
            ui.CloseForLifecycle(reason);
        };
        if (!actor || actor->IsDead() || actor->IsDisabled()) {
            close(EndReason::TargetLost);
            return;
        }
        const auto now = NowMs();
        const auto revision = _appearanceRevision.load();
        const bool changed = revision != _observedRevision;
        if (changed) {
            _observedRevision = revision;
            _refresh.Request(now, true);
        }
        EnforceMovementHold(actor);
        auto* root = actor->Get3D(false);
        if (!root) {
            if (!_missing3DSince) _missing3DSince = now;
            if (now - _missing3DSince > 3000) {
                close(EndReason::TargetLost);
                return;
            }
            _aiHold.SetUpdating(*actor, true);
            return;
        }
        _missing3DSince = 0;
        const bool rebuilt = root != _lastRoot;
        if (rebuilt) {
            _lastRoot = root;
            _framingValid = false;
            _refresh.Request(now, true);
        }
        _refresh.AdvanceFrame();
        if (_refresh.IsSettled(now)) {
            _refresh.Complete();
            _framingValid = false;
        }
        // Native equip/morph work needs a brief actor update window. Movement
        // stays blocked throughout; no appearance is copied or reconstructed.
        _aiHold.SetUpdating(*actor, _refresh.AllowUpdates());
        if (!_viewport.IsValid()) return;
        if (!_policy.Owns(Ownership::Camera)) {
            if (!AcquireCamera(actor) || !_scene.Begin(actor)) {
                close(EndReason::SetupFailed);
                return;
            }
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || camera->currentState.get() != _freeCameraState ||
            (!_framingValid && !CalculateFraming(actor)) || !ApplyCamera() ||
            !_scene.Tick(actor, _stagedTranslation, _cameraApproach, _framingCenter,
                _framingDistance, kStageFOV, now, changed || rebuilt)) {
            close(EndReason::CameraChanged);
            return;
        }
        SetStatus(true, "");
        const bool sendStatus = std::exchange(_statusDirty, false);
        lock.unlock();
        if (sendStatus) ui.SendPreviewState();
    }

    void TailorPreviewSession::SetStatus(bool ready, std::string message)
    {
        if (_ready == ready && _statusMessage == message) return;
        _ready = ready;
        _statusMessage = std::move(message);
        _statusDirty = true;
    }
    bool TailorPreviewSession::IsActive() const noexcept
    {
        std::scoped_lock lock(_mutex);
        return _policy.IsActive();
    }
    std::uint64_t TailorPreviewSession::Generation() const noexcept
    {
        std::scoped_lock lock(_mutex);
        return _policy.Generation();
    }
    bool TailorPreviewSession::IsReady() const noexcept
    {
        std::scoped_lock lock(_mutex);
        return _ready;
    }
    std::string TailorPreviewSession::StatusMessage() const
    {
        std::scoped_lock lock(_mutex);
        return _statusMessage;
    }
    bool TailorPreviewSession::AcquireCamera(RE::Actor* a_actor)
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* worldCamera = RE::Main::WorldRootCamera();
        if (!camera || !camera->currentState || !worldCamera ||
            (!camera->IsInFirstPerson() && !camera->IsInThirdPerson()) ||
            !IsFinite(worldCamera->world.translate)) {
            logger::warn(
                "Tailor preview camera declined: current state is not ordinary first/third person");
            return false;
        }

        auto* freeState = static_cast<RE::FreeCameraState*>(
            camera->GetRuntimeData().cameraStates[RE::CameraState::kFree].get());
        if (!freeState) {
            logger::warn("Tailor preview camera declined: FreeCameraState unavailable");
            return false;
        }

        _savedCameraState = camera->currentState;
        _freeCameraState = freeState;
        _savedFreeTranslation = freeState->translation;
        _savedFreeRotation = freeState->rotation;
        _savedFreeZUpDown = freeState->zUpDown;
        _savedFreeVerticalDirection = freeState->verticalDirection;
        _savedFreeLockToZPlane = freeState->lockToZPlane;
        _savedFreeInputEnabled = freeState->IsInputEventHandlingEnabled();
        _savedFOV = camera->GetRuntimeData2().worldFOV;
        _initialCameraPosition = worldCamera->world.translate;

        auto& smoothCam = SmoothCamCompat::GetSingleton();
        if (!smoothCam.AcquireCameraControl()) {
            logger::warn("Tailor preview camera declined: SmoothCam did not grant control");
            return false;
        }
        if (smoothCam.OwnsCameraControl()) {
            _policy.Acquire(Ownership::ExternalCamera);
        }

        camera->ToggleFreeCameraMode(false);
        if (camera->currentState.get() != freeState) {
            logger::warn("Tailor preview camera declined: free-camera transition failed");
            freeState->translation = _savedFreeTranslation;
            freeState->rotation = _savedFreeRotation;
            freeState->zUpDown = _savedFreeZUpDown;
            freeState->verticalDirection = _savedFreeVerticalDirection;
            freeState->lockToZPlane = _savedFreeLockToZPlane;
            freeState->SetInputEventHandlingEnabled(_savedFreeInputEnabled);
            if (_savedCameraState && camera->currentState.get() != _savedCameraState.get()) {
                camera->SetState(_savedCameraState.get());
            }
            camera->GetRuntimeData2().worldFOV = _savedFOV;
            _freeCameraState = nullptr;
            _savedCameraState.reset();
            return false;
        }

        freeState->SetInputEventHandlingEnabled(false);
        freeState->zUpDown = {};
        freeState->verticalDirection = 0;
        freeState->lockToZPlane = false;
        _policy.Acquire(Ownership::Camera);

        return CalculateFraming(a_actor) && ApplyCamera();
    }

    void TailorPreviewSession::ReleaseCamera() noexcept
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* freeState = _freeCameraState;
        const bool cameraOwned = _policy.Owns(Ownership::Camera);
        const bool stillOwned = camera && cameraOwned && camera->currentState.get() == freeState;

        if (freeState && cameraOwned) {
            if (stillOwned || !_cameraApplied) {
                freeState->translation = _savedFreeTranslation;
                freeState->rotation = _savedFreeRotation;
                freeState->zUpDown = _savedFreeZUpDown;
                freeState->verticalDirection = _savedFreeVerticalDirection;
                freeState->lockToZPlane = _savedFreeLockToZPlane;
                freeState->SetInputEventHandlingEnabled(_savedFreeInputEnabled);
            } else {
                int restored = 0;
                int skipped = 0;
                if (NearlyEqual(freeState->translation, _stagedTranslation)) {
                    freeState->translation = _savedFreeTranslation;
                    ++restored;
                } else {
                    ++skipped;
                }
                if (NearlyEqual(freeState->rotation, _stagedRotation)) {
                    freeState->rotation = _savedFreeRotation;
                    ++restored;
                } else {
                    ++skipped;
                }
                const RE::BSTPoint2<float> zero{};
                if (NearlyEqual(freeState->zUpDown, zero)) {
                    freeState->zUpDown = _savedFreeZUpDown;
                    ++restored;
                } else {
                    ++skipped;
                }
                if (freeState->verticalDirection == 0) {
                    freeState->verticalDirection = _savedFreeVerticalDirection;
                    ++restored;
                } else {
                    ++skipped;
                }
                if (!freeState->lockToZPlane) {
                    freeState->lockToZPlane = _savedFreeLockToZPlane;
                    ++restored;
                } else {
                    ++skipped;
                }
                if (!freeState->IsInputEventHandlingEnabled()) {
                    freeState->SetInputEventHandlingEnabled(_savedFreeInputEnabled);
                    ++restored;
                } else {
                    ++skipped;
                }
                logger::warn(
                    "Tailor preview camera ownership was taken externally; restored {} dormant field(s), skipped {} changed field(s)",
                    restored, skipped);
            }
        }

        if (stillOwned) {
            camera->ToggleFreeCameraMode(false);
            if (_savedCameraState && camera->currentState.get() != _savedCameraState.get()) {
                camera->SetState(_savedCameraState.get());
            }
        }

        if (camera && _cameraApplied) {
            auto& fov = camera->GetRuntimeData2().worldFOV;
            if (NearlyEqual(fov, kStageFOV, 0.01f)) {
                fov = _savedFOV;
                camera->Update();
            } else {
                logger::warn("Tailor preview FOV changed externally; restore skipped");
            }
        }

        if (_policy.Owns(Ownership::ExternalCamera)) {
            SmoothCamCompat::GetSingleton().ReleaseCameraControl();
        }

        _savedCameraState.reset();
        _freeCameraState = nullptr;
        _cameraApplied = false;
        _framingValid = false;
    }

    bool TailorPreviewSession::CalculateFraming(RE::Actor* a_actor)
    {
        auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
        if (!a_actor || !root || !_viewport.IsValid() || !IsFinite(_initialCameraPosition)) {
            return false;
        }

        const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
        if (screen.width < 320 || screen.height < 240) {
            return false;
        }

        const RE::NiPoint3 feet = a_actor->GetPosition();
        RE::NiPoint3 head{};
        bool haveHead = false;
        if (auto* headNode = root->GetObjectByName(RE::BSFixedString("NPC Head [Head]"))) {
            head = headNode->world.translate;
            haveHead = IsFinite(head) && head.z > feet.z + 40.0f && head.z < feet.z + 280.0f;
        }

        const float height = haveHead ?
            std::clamp(head.z - feet.z, 80.0f, 260.0f) :
            std::clamp(root->worldBound.radius * 1.45f, 90.0f, 230.0f);
        const auto envelope = CalculateFramingEnvelope(height, _viewport.hairMode);
        RE::NiPoint3 center{
            haveHead ? (_viewport.hairMode ? head.x : (feet.x + head.x) * 0.5f) : root->worldBound.center.x,
            haveHead ? (_viewport.hairMode ? head.y : (feet.y + head.y) * 0.5f) : root->worldBound.center.y,
            feet.z + envelope.centerFromFeet
        };
        if (!IsFinite(center)) {
            return false;
        }

        if (std::abs(_baseApproach.x) < 0.0001f &&
            std::abs(_baseApproach.y) < 0.0001f) {
            const float heading = a_actor->GetAngleZ();
            const RE::NiPoint3 approach{std::sin(heading), std::cos(heading), 0.0f};
            const float approachLength = 1.0f;
            _baseApproach = approach / approachLength;
        }

        const float aspect = static_cast<float>(screen.width) / static_cast<float>(screen.height);
        const float tanHalfHorizontal = std::tan(kStageFOV * 0.5f * kPi / 180.0f);
        const float tanHalfVertical = tanHalfHorizontal / (std::max)(aspect, 0.1f);
        const auto fit = FitViewport(_viewport.x, _viewport.y, _viewport.width, _viewport.height,
            envelope.halfWidth, envelope.halfHeight, tanHalfHorizontal, tanHalfVertical);
        const float distance = fit.distance;
        const float lateral = fit.lateral;
        const float vertical = fit.vertical;
        const float cameraHeading = std::atan2(_baseApproach.x, _baseApproach.y) - fit.yawOffset;
        _cameraApproach = {std::sin(cameraHeading), std::cos(cameraHeading), 0.0f};
        const RE::NiPoint3 forward = _cameraApproach * -1.0f;
        const RE::NiPoint3 right{forward.y, -forward.x, 0.0f};

        _framingCenter = center;
        _framingDistance = distance;
        _stagedTranslation = center - forward * distance - right * lateral;
        _stagedTranslation.z -= vertical;
        _stagedRotation.x = 0.0f;
        _stagedRotation.y = std::atan2(forward.x, forward.y);
        _framingValid = IsFinite(_stagedTranslation) &&
            std::isfinite(_stagedRotation.y);
        return _framingValid;
    }

    bool TailorPreviewSession::ApplyCamera()
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* freeState = _freeCameraState;
        if (!camera || !freeState || camera->currentState.get() != freeState) {
            return false;
        }

        freeState->translation = _stagedTranslation;
        freeState->rotation = _stagedRotation;
        freeState->zUpDown = {};
        freeState->verticalDirection = 0;
        freeState->lockToZPlane = false;
        freeState->SetInputEventHandlingEnabled(false);
        camera->GetRuntimeData2().worldFOV = kStageFOV;
        _cameraApplied = true;
        camera->Update();
        return camera->currentState.get() == freeState;
    }

}
