#pragma once

#include "preview/PreviewSessionPolicy.h"
#include "preview/PreviewScene.h"
#include <atomic>
#include <mutex>
#include <string>

namespace Tailor::Preview
{
    enum class EndReason : std::uint8_t
    {
        UserClose, FocusLost, TargetLost, CameraChanged,
        PreLoadGame, NewGame, Shutdown, SetupFailed
    };

    struct ViewportRect
    {
        float x{0}, y{0}, width{0}, height{0};
        bool hairMode{false};
        [[nodiscard]] bool IsValid() const noexcept;
    };

    class TailorPreviewSession : public RE::BSTEventSink<RE::TESEquipEvent>,
                                public RE::BSTEventSink<SKSE::NiNodeUpdateEvent>
    {
    public:
        static TailorPreviewSession& GetSingleton();
        static void InstallHooks();
        void RegisterEvents();
        bool Begin(RE::ActorHandle a_target);
        void End(EndReason a_reason) noexcept;
        void SetViewport(ViewportRect a_viewport);
        void Tick(std::uint64_t a_generation);
        void NotifyAppearanceChanged(RE::Actor* a_actor);
        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        [[nodiscard]] bool IsReady() const noexcept;
        [[nodiscard]] std::string StatusMessage() const;

        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(const SKSE::NiNodeUpdateEvent* a_event,
            RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*) override;
    private:
        TailorPreviewSession() = default;
        void AcquireTargetHold(RE::Actor* a_actor);
        void EnforceMovementHold(RE::Actor* a_actor);
        void ReleaseTargetHold(RE::Actor* a_actor) noexcept;
        void SetStatus(bool a_ready, std::string a_message);
        bool AcquireCamera(RE::Actor* a_actor);
        void ReleaseCamera() noexcept;
        bool CalculateFraming(RE::Actor* a_actor);
        bool ApplyCamera();

        mutable std::mutex _mutex;
        PreviewSessionPolicy _policy;
        ActorAIHold _aiHold;
        AppearanceUpdateWindow _refresh;
        RE::ActorHandle _target;
        ViewportRect _viewport;
        const RE::NiAVObject* _lastRoot{nullptr}; // identity only; never dereferenced
        std::atomic<RE::FormID> _targetFormID{0};
        std::atomic<std::uint64_t> _appearanceRevision{0};
        std::uint64_t _observedRevision{0};
        std::int64_t _missing3DSince{0};
        bool _ready{false}, _statusDirty{false};
        std::string _statusMessage;
        PreviewScene _scene;
        RE::NiPoint3 _framingCenter{};
        float _framingDistance{0};
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
        RE::NiPoint3 _cameraApproach{};
        RE::NiPoint3 _stagedTranslation{};
        RE::BSTPoint2<float> _stagedRotation{};
        bool _framingValid{false};
    };
}
