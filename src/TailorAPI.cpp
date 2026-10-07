#include "pch.h"

#include "TailorAPI.h"
#include "api/ModApi.h"
#include "ui/TailorUI.h"

#include <atomic>
#include <cstdint>

namespace
{
    std::atomic<bool> g_hotkeyEnabled{true};
}

namespace TailorAPI
{
    bool IsHotkeyEnabled()
    {
        return g_hotkeyEnabled.load(std::memory_order_relaxed);
    }

    void SetHotkeyEnabled(bool enabled)
    {
        g_hotkeyEnabled.store(enabled, std::memory_order_relaxed);
    }
}

// ================================================================
//  Exported C API
//
//  For other SKSE plugins that want to drive Tailor's UI themselves
//  (menu managers, hotkey frameworks). Resolve at runtime:
//
//      auto* h = GetModuleHandleA("Tailor.dll");
//      auto  open = (void(*)())GetProcAddress(h, "OpenTailor");
//
//  A null module handle just means Tailor is not installed, so callers
//  should treat every one of these as optional.
//
//  The four UI functions are safe to call from any thread. Open/Close
//  marshal onto the game thread via the SKSE task interface, so they
//  return immediately and the UI changes on the next frame.
// ================================================================

extern "C"
{
    // Opens the Tailor UI on the NPC under the crosshair, or on the player when
    // there is none. No-op if already open, or while the game cannot safely show
    // the native menu. With a child under the crosshair it doesn't open: Tailor
    // never handles children, and shows a HUD message instead.
    DLLEXPORT void OpenTailor()
    {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([]() { TailorUI::GetSingleton().Open(); });
        }
    }

    // Closes the Tailor UI, reverting any in-progress preview and unfreezing
    // the target. No-op if already closed.
    DLLEXPORT void CloseTailor()
    {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([]() { TailorUI::GetSingleton().Close(); });
        }
    }

    // Enables or disables Tailor's own configurable keyboard hotkey (Shift+Z by
    // default). Disable this before driving the UI with your own binding, so the
    // two do not both fire. Runtime only: not persisted, and it resets to enabled
    // on game restart, so set it again on load.
    //
    // Does NOT affect the Tailor Lesser Power. If you want that gone too, users
    // can set GrantPower=0 in Data/SKSE/Plugins/Tailor.ini.
    DLLEXPORT void SetTailorHotkeyEnabled(bool enabled)
    {
        TailorAPI::SetHotkeyEnabled(enabled);
        logger::info("TailorAPI: native hotkey {} by external caller",
            enabled ? "enabled" : "disabled");
    }

    // Whether the Tailor UI is currently open. Useful for keeping a menu
    // manager's own toggle state in sync, since the Lesser Power can also
    // open Tailor without going through this API.
    DLLEXPORT bool IsTailorOpen()
    {
        return TailorUI::GetSingleton().IsOpen();
    }

    // Tailor 3.0: the mod API's C++ interface, TailorAPI::ITailorInterface1 for version 1 (the same
    // instance on every call), else nullptr. docs/api/TailorAPI.h's RequestInterface1() calls it.
    DLLEXPORT void* RequestTailorInterface(std::uint32_t version)
    {
        return version == 1 ? Tailor::Api::Interface1() : nullptr;
    }
}
