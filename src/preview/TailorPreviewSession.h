#pragma once

#include "preview/PreviewSessionPolicy.h"

#include <mutex>

namespace Tailor::Preview
{
    enum class EndReason : std::uint8_t
    {
        UserClose,
        FocusLost,
        TargetLost,
        CameraChanged,
        PreLoadGame,
        NewGame,
        Shutdown,
        SetupFailed
    };

    struct ViewportRect
    {
        float x{0.0f};
        float y{0.0f};
        float width{0.0f};
        float height{0.0f};

        [[nodiscard]] bool IsValid() const noexcept;
    };

    class TailorPreviewSession
    {
    public:
        static TailorPreviewSession& GetSingleton();
        static void InstallHooks();

        bool Begin(RE::ActorHandle a_target, ViewportRect a_viewport);
        void End(EndReason a_reason) noexcept;

        void SetViewport(ViewportRect a_viewport);
        void Tick(std::uint64_t a_generation);

        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] std::uint64_t Generation() const noexcept;

    private:
        TailorPreviewSession() = default;

        bool AcquireTargetHold(RE::Actor* a_actor);
        void ReleaseTargetHold(RE::Actor* a_actor) noexcept;
        bool CaptureTargetRoot(RE::Actor* a_actor);
        bool CaptureReplacementRoot(RE::Actor* a_actor);
        bool AcquireCamera(RE::Actor* a_actor);
        void ReleaseCamera() noexcept;
        bool CalculateFraming(RE::Actor* a_actor);
        bool ApplyCamera();

        mutable std::mutex _mutex;
        PreviewSessionPolicy _policy;
        RE::ActorHandle _target;
        ViewportRect _viewport;

        RE::NiPointer<RE::NiAVObject> _targetRoot;
        RE::NiPointer<RE::NiNode> _sceneParent;
        bool _targetRootRebuiltLogged{false};

        RE::BSTSmartPointer<RE::TESCameraState> _savedCameraState;
        RE::FreeCameraState* _freeCameraState{nullptr};
        RE::NiPoint3 _savedFreeTranslation{};
        RE::BSTPoint2<float> _savedFreeRotation{};
        RE::BSTPoint2<float> _savedFreeZUpDown{};
        std::int16_t _savedFreeVerticalDirection{0};
        bool _savedFreeLockToZPlane{false};
        bool _savedFreeInputEnabled{true};
        float _savedFOV{0.0f};
        bool _cameraApplied{false};

        RE::NiPoint3 _initialCameraPosition{};
        RE::NiPoint3 _baseApproach{};
        RE::NiPoint3 _stagedTranslation{};
        RE::BSTPoint2<float> _stagedRotation{};
        bool _framingValid{false};
    };
}
