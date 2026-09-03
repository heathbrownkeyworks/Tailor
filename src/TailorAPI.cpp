#include "pch.h"

#include "TailorAPI.h"
#include "ui/TailorUI.h"

#include <atomic>

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
//  All four are safe to call from any thread. Open/Close marshal onto the
//  game thread via the SKSE task interface, so they return immediately and
//  the UI changes on the next frame.
// ================================================================

extern "C"
{
    // Opens the Tailor UI on the NPC under the crosshair. No-op if already
    // open, or if another Meridian UI view currently holds focus.
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
}
