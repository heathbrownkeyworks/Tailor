#pragma once
#include "InputMap.h"
#include <imgui_internal.h>

namespace Tailor::ImGuiUI::input
{
    // The stock Win32 backend polls XInput independently of Skyrim. Keep its
    // platform work, but discard only controller events added by that call.
    // Older queued Tailor events and all keyboard/mouse events stay intact.
    inline void DiscardBackendControllerEvents(ImGuiContext& context, int firstBackendEvent)
    {
        auto& events = context.InputEventsQueue;
        for (int i = events.Size - 1; i >= firstBackendEvent; --i) {
            const auto& event = events[i];
            if (event.Type == ImGuiInputEventType_Key && event.Key.Key >= ImGuiKey_Gamepad_BEGIN &&
                event.Key.Key < ImGuiKey_Gamepad_END) events.erase(events.begin() + i);
        }
    }
}
