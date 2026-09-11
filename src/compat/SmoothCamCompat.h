#pragma once

#include "compat/SmoothCamAPI.h"

#include <atomic>
#include <mutex>

namespace SKSE
{
    class LoadInterface;
}

class SmoothCamCompat
{
public:
    enum class CameraControlResult { Pending, Granted, Denied };

    static SmoothCamCompat& GetSingleton();

    void DetectInstalled(const SKSE::LoadInterface* a_loadInterface);
    void RegisterInterfaceListener();
    void RequestAPI();
    void HandleMessage(SKSE::MessagingInterface::Message* a_message);

    [[nodiscard]] CameraControlResult AcquireCameraControl();
    void ReleaseCameraControl() noexcept;
    void ProcessCameraRequests();
    [[nodiscard]] bool OwnsCameraControl() const noexcept;

private:
    SmoothCamCompat() = default;
    void ProcessCameraRequestsLocked();

    enum class CameraState { Idle, Pending, Granted, Denied, Releasing };
    mutable std::mutex _cameraMutex;
    CameraState _cameraState{CameraState::Idle};
    bool _releaseWarningLogged{false};

    std::atomic_bool _installed{false};
    std::atomic_bool _listenerRegistered{false};
    std::atomic_bool _apiRequested{false};
    std::atomic_bool _dispatcherAvailable{false};
    std::atomic<SmoothCamAPI::IVSmoothCam1*> _api{nullptr};
};
