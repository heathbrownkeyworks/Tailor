#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace Tailor::Preview
{
    enum class SessionState : std::uint8_t
    {
        Inactive,
        SettingUp,
        Active,
        TearingDown
    };

    enum class Ownership : std::uint32_t
    {
        None = 0,
        Camera = 1u << 2,
        SavingDisabled = 1u << 3,
        ExternalCamera = 1u << 4
    };

    template<class Camera> bool ExitFlyCameraForOpen(Camera* camera)
    {
        if (!camera || !camera->IsInFreeCameraMode()) return true;
        // This API toggles modes; false means unpaused, not "disable".
        // Guard it so ordinary gameplay can never accidentally enter fly mode.
        camera->ToggleFreeCameraMode(false);
        return !camera->IsInFreeCameraMode();
    }

    template<class Camera, class State>
    bool EnterPreviewCamera(Camera* camera, State* freeState, State* thirdState, bool playerTarget)
    {
        if (!camera || !freeState) return false;
        if (playerTarget && camera->IsInFirstPerson()) {
            if (!thirdState) return false;
            // ForceThirdPerson is a request, not a completed transition.
            // Enter the actual state before freecam captures which
            // player skeleton should render (the SPIM/Menu Studio transition).
            camera->SetState(thirdState);
            if (camera->currentState.get() != thirdState) return false;
        }
        camera->ToggleFreeCameraMode(false);
        return camera->currentState.get() == freeState;
    }

    // Hold locomotion only. Actor processing, life state and animation graphs
    // remain owned by Skyrim and other mods throughout the preview.
    class ActorMovementHold
    {
    public:
        template<class Actor> void Acquire(Actor& actor)
        {
            if (_active) return;
            _wasBlocked = actor.GetActorRuntimeData().boolFlags.any(Actor::BOOL_FLAGS::kMovementBlocked);
            _active = true;
            Enforce(actor);
        }
        template<class Actor> void Enforce(Actor& actor)
        {
            if (!_active) return;
            actor.GetActorRuntimeData().boolFlags.set(Actor::BOOL_FLAGS::kMovementBlocked);
            if (auto* controller = actor.GetCharController()) controller->SetLinearVelocityImpl(0.0f);
        }
        template<class Actor> void Restore(Actor& actor)
        {
            if (!_active) return;
            if (!_wasBlocked) actor.GetActorRuntimeData().boolFlags.reset(Actor::BOOL_FLAGS::kMovementBlocked);
            Reset();
        }
        void Reset() noexcept { _active = false; _wasBlocked = false; }
    private:
        bool _active{false};
        bool _wasBlocked{false};
    };

    // AddTask can drain several callbacks in one frame: require time AND frames.
    class AppearanceUpdateWindow
    {
    public:
        void Request(std::int64_t nowMs)
        {
            if (!_pending) {
                _deadlineMs = nowMs + 1500;
                _frames = 0;
            }
            _pending = true;
            _dueMs = nowMs + 250;
            _stableFrames = 0;
        }
        void AdvanceFrame() noexcept
        {
            if (!_pending) return;
            if (_frames < 3) ++_frames;
            if (_stableFrames < 3) ++_stableFrames;
        }
        bool IsSettled(std::int64_t nowMs) const noexcept
        {
            return _pending && _frames >= 3 &&
                ((nowMs >= _dueMs && _stableFrames >= 3) || nowMs >= _deadlineMs);
        }
        void Complete() noexcept { _pending = false; }
    private:
        bool _pending{false};
        std::int64_t _dueMs{0}, _deadlineMs{0};
        unsigned _frames{0}, _stableFrames{0};
    };

    struct FramingEnvelope
    {
        float headroom{0.0f};
        float centerFromFeet{0.0f};
        float halfHeight{0.0f};
        float halfWidth{0.0f};
    };

    [[nodiscard]] constexpr FramingEnvelope CalculateFramingEnvelope(
        float a_actorHeight, bool a_hairMode = false) noexcept
    {
        const float headroom = std::clamp(a_actorHeight * 0.12f, 8.0f, 24.0f);
        // Wig mode includes the head, shoulders and room for longer hair.
        const float bottom = a_hairMode ? a_actorHeight * 0.58f : 0.0f;
        const float halfHeight = (a_actorHeight + headroom - bottom) * 0.5f;
        return {headroom, bottom + halfHeight, halfHeight,
            a_actorHeight * (a_hairMode ? 0.20f : 0.22f)};
    }

    struct CameraFraming
    {
        float distance, lateral, vertical, yawOffset;
    };

    class PreviewOrbit
    {
    public:
        bool Set(float yaw, std::uint64_t sequence) noexcept
        {
            if (!std::isfinite(yaw) || !sequence || sequence <= _sequence) return false;
            _yaw = std::remainder(yaw, 6.28318530718f);
            _sequence = sequence;
            return true;
        }
        float Yaw() const noexcept { return _yaw; }
        void Reset() noexcept { _yaw = 0.0f; _sequence = 0; }
    private:
        float _yaw{0.0f};
        std::uint64_t _sequence{0};
    };

    struct OrbitPlacement
    {
        float x, y, z, heading;
    };

    [[nodiscard]] inline OrbitPlacement PlaceOrbitCamera(
        float actorHeading, float orbitYaw, const CameraFraming& fit)
    {
        const float heading = actorHeading + orbitYaw - fit.yawOffset;
        const float x = std::sin(heading), y = std::cos(heading);
        return {x * fit.distance + y * fit.lateral,
            y * fit.distance - x * fit.lateral, -fit.vertical, heading};
    }

    // Inputs are normalized HTML bounds and the world camera frustum tangents.
    // Keeping both in full-screen coordinates avoids double-applying viewport
    // scaling (the tiny actor at the bottom of the former snapshot preview).
    [[nodiscard]] inline CameraFraming FitViewport(float x, float y, float width, float height,
        float halfActorWidth, float halfActorHeight, float tanHorizontal, float tanVertical)
    {
        const float distance = (std::max)(halfActorWidth / (width * tanHorizontal * 0.86f),
            halfActorHeight / (height * tanVertical * 0.86f));
        const float lateral = (2.0f * (x + width * 0.5f) - 1.0f) * distance * tanHorizontal;
        // Counter-rotate the camera basis before applying the lateral offset.
        // Its position then stays in front of the NPC on either side of the UI.
        return {distance, lateral,
            (1.0f - 2.0f * (y + height * 0.5f)) * distance * tanVertical,
            std::atan2(lateral, distance)};
    }

    class PreviewSessionPolicy
    {
    public:
        [[nodiscard]] bool Begin() noexcept
        {
            if (_state != SessionState::Inactive) {
                return false;
            }

            AdvanceGeneration();
            _state = SessionState::SettingUp;
            _ownership = 0;
            return true;
        }

        [[nodiscard]] bool Activate() noexcept
        {
            if (_state != SessionState::SettingUp) {
                return false;
            }
            _state = SessionState::Active;
            return true;
        }

        [[nodiscard]] bool BeginTeardown() noexcept
        {
            if (_state == SessionState::Inactive || _state == SessionState::TearingDown) {
                return false;
            }
            AdvanceGeneration();
            _state = SessionState::TearingDown;
            return true;
        }

        void FinishTeardown() noexcept
        {
            _ownership = 0;
            _state = SessionState::Inactive;
        }

        [[nodiscard]] SessionState State() const noexcept { return _state; }
        [[nodiscard]] bool IsActive() const noexcept { return _state == SessionState::Active; }
        [[nodiscard]] std::uint64_t Generation() const noexcept { return _generation; }

        [[nodiscard]] bool Accepts(std::uint64_t a_generation) const noexcept
        {
            return a_generation == _generation &&
                   (_state == SessionState::SettingUp || _state == SessionState::Active);
        }

        void Acquire(Ownership a_resource) noexcept
        {
            _ownership |= ToBits(a_resource);
        }

        void Release(Ownership a_resource) noexcept
        {
            _ownership &= ~ToBits(a_resource);
        }

        [[nodiscard]] bool Owns(Ownership a_resource) const noexcept
        {
            const auto resource = ToBits(a_resource);
            return resource != 0 && (_ownership & resource) == resource;
        }

    private:
        static constexpr std::uint32_t ToBits(Ownership a_resource) noexcept
        {
            return static_cast<std::underlying_type_t<Ownership>>(a_resource);
        }

        void AdvanceGeneration() noexcept
        {
            ++_generation;
            if (_generation == 0) {
                ++_generation;
            }
        }

        SessionState _state{SessionState::Inactive};
        std::uint64_t _generation{0};
        std::uint32_t _ownership{0};
    };
}
