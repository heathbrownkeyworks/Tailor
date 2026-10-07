#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <exception>
#include <optional>
#include <string>

namespace Tailor::ImGuiUI
{
    // Runs a frame's drawing, after NewFrame. If it throws, every window, ID, style, font and disabled block it left
    // open is unwound to where NewFrame left them, so Render can still end the frame and nothing carries into the next
    // one; the error comes back as text. Nothing comes back when it drew to the end. Engine-free, so the screen tests
    // run it too.
    template <class Draw>
    std::optional<std::string> DrawRecovering(Draw&& draw)
    {
        ImGuiErrorRecoveryState state;
        ImGui::ErrorRecoveryStoreState(&state);
        try {
            draw();
            return std::nullopt;
        } catch (const std::exception& e) {
            ImGui::ErrorRecoveryTryToRecoverState(&state);
            return std::string(e.what());
        } catch (...) {
            ImGui::ErrorRecoveryTryToRecoverState(&state);
            return std::string("an unknown error");
        }
    }
}
