#include "compat/SmoothCamCompat.h"

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
        logger::info("SmoothCamCompat: acquired SmoothCam camera-control API");
    }
}

bool SmoothCamCompat::AcquireCameraControl()
{
    if (!_installed.load()) {
        return true;
    }
    auto* api = _api.load();
    if (!api) {
        logger::warn("SmoothCamCompat: declining preview because SmoothCam is installed without an API");
        return false;
    }

    const auto result = api->RequestCameraControl(SKSE::GetPluginHandle());
    if (result == SmoothCamAPI::APIResult::OK || result == SmoothCamAPI::APIResult::AlreadyGiven) {
        _ownsCamera.store(true);
        return true;
    }

    logger::warn("SmoothCamCompat: camera-control request declined ({})", ResultName(result));
    return false;
}

void SmoothCamCompat::ReleaseCameraControl() noexcept
{
    if (!_ownsCamera.exchange(false)) {
        return;
    }
    auto* api = _api.load();
    if (!api) {
        logger::warn("SmoothCamCompat: API disappeared while Tailor owned camera control");
        return;
    }

    const auto result = api->ReleaseCameraControl(SKSE::GetPluginHandle());
    if (result != SmoothCamAPI::APIResult::OK) {
        logger::warn("SmoothCamCompat: camera-control release returned {}", ResultName(result));
    }
}
