#pragma once

#include <algorithm>
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
        Restrained = 1u << 0,
        MovementBlocked = 1u << 1,
        Camera = 1u << 2,
        SavingDisabled = 1u << 3,
        ExternalCamera = 1u << 4
    };

    struct FramingEnvelope
    {
        float headroom{0.0f};
        float centerFromFeet{0.0f};
        float halfHeight{0.0f};
    };

    [[nodiscard]] constexpr FramingEnvelope CalculateFramingEnvelope(
        float a_actorHeight) noexcept
    {
        const float headroom = std::clamp(a_actorHeight * 0.12f, 8.0f, 24.0f);
        const float halfHeight = (a_actorHeight + headroom) * 0.5f;
        return {headroom, halfHeight, halfHeight};
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
