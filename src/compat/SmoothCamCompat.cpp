#include "compat/SmoothCamCompat.h"
#include "compat/SmoothCamCameraHook.h"

#include <Windows.h>

#include <cstring>

namespace
{
    const char* ResultName(SmoothCamAPI::APIResult a_result)
    {
        switch (a_result) {
        case SmoothCamAPI::APIResult::OK: return "ok";
        case SmoothCamAPI::APIResult::NotOwner: return "not-owner";
        case SmoothCamAPI::APIResult::MustKeep: return "must-keep";
        case SmoothCamAPI::APIResult::AlreadyGiven: return "already-given";
        case SmoothCamAPI::APIResult::AlreadyTaken: return "already-taken";
        case SmoothCamAPI::APIResult::BadThread: return "bad-thread";
        }
        return "unknown";
    }
}

SmoothCamCompat& SmoothCamCompat::GetSingleton()
{
    static SmoothCamCompat singleton;
    return singleton;
}

void SmoothCamCompat::DetectInstalled(const SKSE::LoadInterface* a_loadInterface)
{
    const auto* pluginInfo = a_loadInterface ? a_loadInterface->GetPluginInfo(SmoothCamAPI::PluginName) : nullptr;
    _installed.store(pluginInfo != nullptr);
    logger::info("SmoothCamCompat: SmoothCam {}", pluginInfo ? "detected" : "not detected");
}

void SmoothCamCompat::RegisterInterfaceListener()
{
    if (_listenerRegistered.load()) {
        return;
    }
    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        logger::warn("SmoothCamCompat: messaging interface unavailable");
        return;
    }

    const bool registered = messaging->RegisterListener(
        SmoothCamAPI::PluginName,
        [](SKSE::MessagingInterface::Message* a_message) {
            SmoothCamCompat::GetSingleton().HandleMessage(a_message);
        });
    _listenerRegistered.store(registered);
    if (!registered && _installed.load()) {
        logger::warn("SmoothCamCompat: failed to register the API response listener");
    }
}

void SmoothCamCompat::RequestAPI()
{
    if (!_installed.load() && GetModuleHandleA("SmoothCam.dll")) {
        _installed.store(true);
        logger::info("SmoothCamCompat: detected loaded SmoothCam.dll");
    }
    if (!_installed.load() || _apiRequested.exchange(true)) {
        return;
    }
    if (!_listenerRegistered.load()) {
        logger::warn("SmoothCamCompat: API listener unavailable; live preview will fail closed");
        return;
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        logger::warn("SmoothCamCompat: messaging interface unavailable during API request");
        return;
    }

    SmoothCamAPI::InterfaceRequest request{};
    SmoothCamAPI::PluginCommand command{};
    command.commandStructure = std::addressof(request);
    if (!messaging->Dispatch(
            0, std::addressof(command), sizeof(command), SmoothCamAPI::PluginName)) {
        logger::warn("SmoothCamCompat: SmoothCam did not accept the API request");
    }
}

void SmoothCamCompat::HandleMessage(SKSE::MessagingInterface::Message* a_message)
{
    if (!a_message ||
        (a_message->sender && std::strcmp(a_message->sender, SmoothCamAPI::PluginName) != 0) ||
        a_message->type != 0 || a_message->dataLen != sizeof(SmoothCamAPI::PluginResponse)) {
        return;
    }

    const auto* response = static_cast<const SmoothCamAPI::PluginResponse*>(a_message->data);
    if (!response || response->type != SmoothCamAPI::PluginResponse::Type::InterfaceProvider ||
        !response->responseData) {
        logger::warn("SmoothCamCompat: SmoothCam returned an API error");
        return;
    }

    const auto* container = static_cast<const SmoothCamAPI::InterfaceContainer*>(response->responseData);
    auto* api = container ? static_cast<SmoothCamAPI::IVSmoothCam1*>(container->interfaceInstance) : nullptr;
    _api.store(api);
    if (api) {
        _dispatcherAvailable.store(InstallSmoothCamCameraHook());
        logger::info("SmoothCamCompat: acquired SmoothCam camera-control API (required thread={})",
            api->GetSmoothCamThreadId());
    }
}

SmoothCamCompat::CameraControlResult SmoothCamCompat::AcquireCameraControl()
{
    if (!_installed.load()) {
        return CameraControlResult::Granted;
    }
    std::scoped_lock lock(_cameraMutex);
    if (!_api.load()) {
        logger::warn("SmoothCamCompat: declining preview because SmoothCam is installed without an API");
        return CameraControlResult::Denied;
    }
    if (!_dispatcherAvailable.load()) {
        logger::warn("SmoothCamCompat: declining preview because the camera dispatcher is unavailable");
        return CameraControlResult::Denied;
    }
    if (_cameraState == CameraState::Idle) _cameraState = CameraState::Pending;
    if (_cameraState == CameraState::Granted) return CameraControlResult::Granted;
    if (_cameraState == CameraState::Denied) return CameraControlResult::Denied;
    // A previous close must finish releasing its lease before a new open acquires one.
    return CameraControlResult::Pending;
}

void SmoothCamCompat::ReleaseCameraControl() noexcept
{
    std::scoped_lock lock(_cameraMutex);
    if (_cameraState == CameraState::Granted) {
        _cameraState = CameraState::Releasing;
        _releaseWarningLogged = false;
    } else if (_cameraState != CameraState::Releasing) {
        // Cancel an unprocessed request too: closing/loading cannot acquire later.
        _cameraState = CameraState::Idle;
    }
    // Lifecycle messages already on the required thread release immediately.
    // Worker-thread closes are completed by the main-update dispatcher.
    auto* api = _api.load();
    if (api && GetCurrentThreadId() == api->GetSmoothCamThreadId()) {
        ProcessCameraRequestsLocked();
    }
}

bool SmoothCamCompat::OwnsCameraControl() const noexcept
{
    std::scoped_lock lock(_cameraMutex);
    return _cameraState == CameraState::Granted || _cameraState == CameraState::Releasing;
}

void SmoothCamCompat::ProcessCameraRequests()
{
    std::scoped_lock lock(_cameraMutex);
    ProcessCameraRequestsLocked();
}

void SmoothCamCompat::ProcessCameraRequestsLocked()
{
    if (_cameraState != CameraState::Pending && _cameraState != CameraState::Releasing) return;
    auto* api = _api.load();
    if (!api) return;
    const auto currentThread = GetCurrentThreadId();
    const auto requiredThread = api->GetSmoothCamThreadId();
    if (currentThread != requiredThread) {
        if (_cameraState == CameraState::Pending || !_releaseWarningLogged) {
            logger::warn("SmoothCamCompat: camera dispatcher thread mismatch (current={}, required={})",
                currentThread, requiredThread);
        }
        if (_cameraState == CameraState::Pending) _cameraState = CameraState::Denied;
        else _releaseWarningLogged = true;
        return;
    }

    if (_cameraState == CameraState::Pending) {
        const auto result = api->RequestCameraControl(SKSE::GetPluginHandle());
        if (result == SmoothCamAPI::APIResult::OK || result == SmoothCamAPI::APIResult::AlreadyGiven) {
            _cameraState = CameraState::Granted;
            logger::info("SmoothCamCompat: camera control acquired (current={}, required={}, result={})",
                currentThread, requiredThread, ResultName(result));
        } else {
            _cameraState = CameraState::Denied;
            logger::warn("SmoothCamCompat: camera-control request declined ({}, current={}, required={})",
                ResultName(result), currentThread, requiredThread);
        }
        return;
    }
    const auto result = api->ReleaseCameraControl(SKSE::GetPluginHandle());
    if (result == SmoothCamAPI::APIResult::OK || result == SmoothCamAPI::APIResult::NotOwner) {
        _cameraState = CameraState::Idle;
        logger::info("SmoothCamCompat: camera control released (current={}, required={}, result={})",
            currentThread, requiredThread, ResultName(result));
    } else if (!_releaseWarningLogged) {
        // Keep the outstanding lease tracked until release succeeds or ownership is gone.
        _releaseWarningLogged = true;
        logger::warn("SmoothCamCompat: camera-control release returned {}", ResultName(result));
    }
}
