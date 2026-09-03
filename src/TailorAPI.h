#pragma once

// Internal side of Tailor's exported C API. The exported functions themselves
// live in TailorAPI.cpp; this header is only for Tailor's own translation units
// that need to honour the runtime hotkey toggle.
//
// The distributable header for other mod authors is docs/api/TailorAPI.h.

namespace TailorAPI
{
    // False once another plugin has called SetTailorHotkeyEnabled(false).
    // Only gates the configurable keyboard hotkey; the Tailor Lesser Power and
    // any in-game UI controls keep working.
    bool IsHotkeyEnabled();
    void SetHotkeyEnabled(bool enabled);
}
