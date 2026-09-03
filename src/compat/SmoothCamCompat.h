#pragma once

#include "compat/SmoothCamAPI.h"

#include <atomic>

namespace SKSE
{
    class LoadInterface;
}

class SmoothCamCompat
{
public:
    static SmoothCamCompat& GetSingleton();

    void DetectInstalled(const SKSE::LoadInterface* a_loadInterface);
    void RegisterInterfaceListener();
    void RequestAPI();
    void HandleMessage(SKSE::MessagingInterface::Message* a_message);

    [[nodiscard]] bool AcquireCameraControl();
    void ReleaseCameraControl() noexcept;
    [[nodiscard]] bool OwnsCameraControl() const noexcept { return _ownsCamera.load(); }

private:
    SmoothCamCompat() = default;

    std::atomic_bool _installed{false};
    std::atomic_bool _listenerRegistered{false};
    std::atomic_bool _apiRequested{false};
    std::atomic_bool _ownsCamera{false};
    std::atomic<SmoothCamAPI::IVSmoothCam1*> _api{nullptr};
};
