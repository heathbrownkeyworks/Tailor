#pragma once

#include <cstdint>

// Minimal ABI declarations for SmoothCam's public V1 camera-ownership API.
// Source interface: SmoothCam/include/SmoothCamAPI.h.
namespace SmoothCamAPI
{
    inline constexpr auto PluginName = "SmoothCam";

    enum class InterfaceVersion : std::uint8_t
    {
        V1,
        V2,
        V3
    };

    enum class APIResult : std::uint8_t
    {
        OK,
        NotOwner,
        MustKeep,
        AlreadyGiven,
        AlreadyTaken,
        BadThread
    };

    class IVSmoothCam1
    {
    public:
        [[nodiscard]] virtual unsigned long GetSmoothCamThreadId() const noexcept = 0;
        [[nodiscard]] virtual APIResult RequestCameraControl(SKSE::PluginHandle a_handle) noexcept = 0;
        [[nodiscard]] virtual APIResult RequestCrosshairControl(
            SKSE::PluginHandle a_handle, bool a_restoreDefaults = true) noexcept = 0;
        [[nodiscard]] virtual APIResult RequestStealthMeterControl(
            SKSE::PluginHandle a_handle, bool a_restoreDefaults = true) noexcept = 0;
        virtual SKSE::PluginHandle GetCameraOwner() const noexcept = 0;
        virtual SKSE::PluginHandle GetCrosshairOwner() const noexcept = 0;
        virtual SKSE::PluginHandle GetStealthMeterOwner() const noexcept = 0;
        virtual APIResult ReleaseCameraControl(SKSE::PluginHandle a_handle) noexcept = 0;
        virtual APIResult ReleaseCrosshairControl(SKSE::PluginHandle a_handle) noexcept = 0;
        virtual APIResult ReleaseStealthMeterControl(SKSE::PluginHandle a_handle) noexcept = 0;
    };

    struct PluginCommand
    {
        enum class Type : std::uint8_t
        {
            RequestInterface
        };

        std::uint32_t header{0x9007CA50};
        Type type{Type::RequestInterface};
        void* commandStructure{nullptr};
    };

    struct InterfaceRequest
    {
        InterfaceVersion interfaceVersion{InterfaceVersion::V1};
    };

    struct PluginResponse
    {
        enum class Type : std::uint8_t
        {
            Error,
            InterfaceProvider
        };

        Type type{Type::Error};
        void* responseData{nullptr};
    };

    struct InterfaceContainer
    {
        void* interfaceInstance{nullptr};
        InterfaceVersion interfaceVersion{InterfaceVersion::V1};
    };
}
