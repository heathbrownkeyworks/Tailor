#include "preview/TailorPreviewSession.h"

#include "PapyrusBridge.h"
#include "compat/SmoothCamCompat.h"
#include "ui/TailorUI.h"

#include <algorithm>
#include <cmath>

namespace Tailor::Preview
{
    namespace
    {
        constexpr float kStageFOV = 55.0f;
        constexpr float kPi = 3.14159265358979323846f;

        struct CursorMenuAdvanceMovieHook
        {
            static void thunk(
                RE::CursorMenu* a_menu,
                float a_interval,
                std::uint32_t a_currentTime)
            {
                func(a_menu, a_interval, a_currentTime);

                auto& session = TailorPreviewSession::GetSingleton();
                if (session.IsActive()) {
                    session.Tick(session.Generation());
                }
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };

        const char* ReasonName(EndReason a_reason)
        {
            switch (a_reason) {
            case EndReason::UserClose: return "user-close";
            case EndReason::FocusLost: return "focus-lost";
            case EndReason::TargetLost: return "target-lost";
            case EndReason::CameraChanged: return "camera-changed";
            case EndReason::PreLoadGame: return "pre-load-game";
            case EndReason::NewGame: return "new-game";
            case EndReason::Shutdown: return "shutdown";
            case EndReason::SetupFailed: return "setup-failed";
            }
            return "unknown";
        }

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

        bool IsMovementBlocked(const RE::Actor* a_actor)
        {
            return a_actor->GetActorRuntimeData().boolFlags.any(
                RE::Actor::BOOL_FLAGS::kMovementBlocked);
        }

    }

    bool ViewportRect::IsValid() const noexcept
    {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
               std::isfinite(height) && x >= 0.0f && y >= 0.0f && width >= 0.05f &&
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
            if (REL::Module::IsVR()) {
                logger::info("Tailor preview frame hook skipped on VR");
                return;
            }

            REL::Relocation<std::uintptr_t> cursorMenuVtbl{RE::VTABLE_CursorMenu[0]};
            CursorMenuAdvanceMovieHook::func =
                cursorMenuVtbl.write_vfunc(0x5, CursorMenuAdvanceMovieHook::thunk);
            logger::info(
                "Tailor preview frame hook installed on CursorMenu::AdvanceMovie");
        });
    }

    bool TailorPreviewSession::Begin(RE::ActorHandle a_target, ViewportRect a_viewport)
    {
        std::unique_lock lock(_mutex);
        if (_policy.IsActive()) {
            if (_target == a_target && a_viewport.IsValid()) {
                _viewport = a_viewport;
                _framingValid = false;
                return true;
            }
            return false;
        }
        if (!_policy.Begin()) {
            return false;
        }

        _target = a_target;
        _viewport = a_viewport;
        auto targetPtr = _target.get();
        auto* actor = targetPtr ? targetPtr.get() : nullptr;
        if (!actor || actor->IsDead() || actor->IsPlayerRef() || !actor->Is3DLoaded() ||
            !_viewport.IsValid() || REL::Module::IsVR()) {
            logger::warn("Tailor preview declined: target, viewport, or runtime is ineligible");
            lock.unlock();
            End(EndReason::SetupFailed);
            return false;
        }

        _targetRootRebuiltLogged = false;
        _cameraApplied = false;
        _framingValid = false;
        _baseApproach = {};

        if (!CaptureTargetRoot(actor)) {
            lock.unlock();
            End(EndReason::SetupFailed);
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            auto& flags = player->GetGameStatsData().byCharGenFlag;
            if (!flags.any(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving)) {
                flags.set(RE::PlayerCharacter::ByCharGenFlag::kDisableSaving);
                _policy.Acquire(Ownership::SavingDisabled);
            }
        }

        if (!AcquireTargetHold(actor) || !AcquireCamera(actor) || !_policy.Activate()) {
            lock.unlock();
            End(EndReason::SetupFailed);
            return false;
        }

        logger::info("Tailor preview active for {} (generation={})",
            actor->GetDisplayFullName(), _policy.Generation());
        return true;
    }

    void TailorPreviewSession::End(EndReason a_reason) noexcept
    {
        std::unique_lock lock(_mutex);
        if (!_policy.BeginTeardown()) {
            return;
        }

        auto targetPtr = _target.get();
        auto* actor = targetPtr ? targetPtr.get() : nullptr;

        ReleaseCamera();

        if (_policy.Owns(Ownership::SavingDisabled)) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                player->GetGameStatsData().byCharGenFlag.reset(
                    RE::PlayerCharacter::ByCharGenFlag::kDisableSaving);
            }
        }

        ReleaseTargetHold(actor);
        _targetRoot.reset();
        _sceneParent.reset();
        _target = RE::ActorHandle{};
        _viewport = {};
        _framingValid = false;
        _policy.FinishTeardown();
        logger::info("Tailor preview ended ({})", ReasonName(a_reason));
    }

    void TailorPreviewSession::SetViewport(ViewportRect a_viewport)
    {
        if (!a_viewport.IsValid()) {
            return;
        }
        std::scoped_lock lock(_mutex);
        _viewport = a_viewport;
        _framingValid = false;
    }

    void TailorPreviewSession::Tick(std::uint64_t a_generation)
    {
        auto& ui = TailorUI::GetSingleton();
        if (!ui.IsOpen()) {
            End(EndReason::UserClose);
            return;
        }
        if (!ui.HasFocus()) {
            ui.CloseForLifecycle(EndReason::FocusLost);
            return;
        }

        std::unique_lock lock(_mutex);
        if (!_policy.IsActive() || !_policy.Accepts(a_generation)) {
            return;
        }

        auto targetPtr = _target.get();
        auto* actor = targetPtr ? targetPtr.get() : nullptr;
        if (!actor || actor->IsDead() || !actor->Is3DLoaded() ||
            !CaptureReplacementRoot(actor)) {
            lock.unlock();
            End(EndReason::TargetLost);
            return;
        }

        auto* camera = RE::PlayerCamera::GetSingleton();
        if (_policy.Owns(Ownership::Camera) &&
            (!camera || camera->currentState.get() != _freeCameraState)) {
            lock.unlock();
            End(EndReason::CameraChanged);
            return;
        }

        if ((!_framingValid && !CalculateFraming(actor)) || !ApplyCamera()) {
            lock.unlock();
            End(EndReason::CameraChanged);
            return;
        }
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

    bool TailorPreviewSession::AcquireTargetHold(RE::Actor* a_actor)
    {
        const bool wasRestrained = a_actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained;
        const bool movementWasBlocked = IsMovementBlocked(a_actor);

        const bool restrainedHeld = wasRestrained || PapyrusBridge::SetActorRestrained(a_actor, true);
        if (!wasRestrained && restrainedHeld) {
            _policy.Acquire(Ownership::Restrained);
        }
        const bool movementHeld = movementWasBlocked || PapyrusBridge::SetActorDontMove(a_actor, true);
        if (!movementWasBlocked && movementHeld) {
            _policy.Acquire(Ownership::MovementBlocked);
        }

        logger::info("Tailor preview soft hold: {} (restrainedOwned={}, movementOwned={})",
            a_actor->GetDisplayFullName(), _policy.Owns(Ownership::Restrained),
            _policy.Owns(Ownership::MovementBlocked));
        return restrainedHeld && movementHeld;
    }

    void TailorPreviewSession::ReleaseTargetHold(RE::Actor* a_actor) noexcept
    {
        if (!a_actor) {
            return;
        }
        // Papyrus method calls are queued. Always enqueue the inverse for a
        // hold we own so rapid open/close leaves the VM queue in restore order.
        if (_policy.Owns(Ownership::MovementBlocked)) {
            PapyrusBridge::SetActorDontMove(a_actor, false);
        }
        if (_policy.Owns(Ownership::Restrained)) {
            PapyrusBridge::SetActorRestrained(a_actor, false);
        }
    }

    bool TailorPreviewSession::CaptureTargetRoot(RE::Actor* a_actor)
    {
        auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
        auto* parent = root ? root->parent : nullptr;
        if (!root || !parent) {
            logger::warn("Tailor preview declined: actor scene parent unavailable");
            return false;
        }

        _targetRoot = RE::NiPointer<RE::NiAVObject>{root};
        _sceneParent = RE::NiPointer<RE::NiNode>{parent};
        return true;
    }

    bool TailorPreviewSession::CaptureReplacementRoot(RE::Actor* a_actor)
    {
        auto* replacement = a_actor ? a_actor->Get3D(false) : nullptr;
        if (!replacement || replacement->parent != _sceneParent.get()) {
            return false;
        }
        if (replacement == _targetRoot.get()) {
            return true;
        }

        auto* previous = _targetRoot.get();
        _targetRoot = RE::NiPointer<RE::NiAVObject>{replacement};
        _framingValid = false;
        if (!_targetRootRebuiltLogged) {
            logger::info(
                "Tailor preview captured rebuilt target 3D root {:p} -> {:p} (generation={})",
                static_cast<const void*>(previous), static_cast<const void*>(replacement),
                _policy.Generation());
            _targetRootRebuiltLogged = true;
        }
        return true;
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
        auto* root = _targetRoot.get();
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
        const auto envelope = CalculateFramingEnvelope(height);
        RE::NiPoint3 center{
            haveHead ? (feet.x + head.x) * 0.5f : root->worldBound.center.x,
            haveHead ? (feet.y + head.y) * 0.5f : root->worldBound.center.y,
            feet.z + envelope.centerFromFeet
        };
        if (!IsFinite(center)) {
            return false;
        }

        if (std::abs(_baseApproach.x) < 0.0001f &&
            std::abs(_baseApproach.y) < 0.0001f) {
            RE::NiPoint3 approach = _initialCameraPosition - center;
            approach.z = 0.0f;
            float approachLength =
                std::sqrt(approach.x * approach.x + approach.y * approach.y);
            if (approachLength < 10.0f || !std::isfinite(approachLength)) {
                const float heading = a_actor->GetAngleZ();
                approach = {std::sin(heading), std::cos(heading), 0.0f};
                approachLength = 1.0f;
            }
            _baseApproach = approach / approachLength;
        }

        const RE::NiPoint3 approach = _baseApproach;
        const RE::NiPoint3 forward{-approach.x, -approach.y, 0.0f};
        const RE::NiPoint3 right{forward.y, -forward.x, 0.0f};
        const float aspect = static_cast<float>(screen.width) / static_cast<float>(screen.height);
        const float tanHalfHorizontal = std::tan(kStageFOV * 0.5f * kPi / 180.0f);
        const float tanHalfVertical = tanHalfHorizontal / (std::max)(aspect, 0.1f);
        const float horizontalHalfWidth = height * 0.22f;
        const float verticalHalfHeight = envelope.halfHeight;
        const float horizontalDistance = horizontalHalfWidth /
            (std::max)(_viewport.width * tanHalfHorizontal * 0.86f, 0.01f);
        const float verticalDistance = verticalHalfHeight /
            (std::max)(_viewport.height * tanHalfVertical * 0.86f, 0.01f);
        const float baseDistance = std::clamp(
            (std::max)(horizontalDistance, verticalDistance), 150.0f, 900.0f);
        const float distance = baseDistance;

        const float centerX = _viewport.x + _viewport.width * 0.5f;
        const float centerY = _viewport.y + _viewport.height * 0.5f;
        const float ndcX = centerX * 2.0f - 1.0f;
        const float ndcY = 1.0f - centerY * 2.0f;
        const float lateral = ndcX * distance * tanHalfHorizontal;
        const float vertical = ndcY * distance * tanHalfVertical;

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
