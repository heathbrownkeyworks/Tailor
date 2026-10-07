#pragma once

#include <imgui.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace Tailor::ImGuiUI::input
{
    [[nodiscard]] constexpr bool EqualControlName(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size()) return false;
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        for (std::size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
        return true;
    }

    // Skyrim's trigger ButtonEvent IDs (0x9/0xA) are arbitrary numbers, unlike
    // the one-bit XInput digital button IDs. Never merge them into held bits.
    [[nodiscard]] constexpr bool IsDigitalControllerButton(std::uint32_t id) noexcept
    {
        constexpr std::uint32_t digitalMask = 0xF3FF;
        return id != 0 && (id & (id - 1)) == 0 && (id & digitalMask) != 0;
    }

    [[nodiscard]] constexpr std::uint32_t ControllerButton(std::string_view name) noexcept
    {
        constexpr std::pair<std::string_view, std::uint32_t> buttons[] = {
            {"DpadUp", 1}, {"DpadDown", 2}, {"DpadLeft", 4}, {"DpadRight", 8},
            {"Start", 0x10}, {"Back", 0x20}, {"LeftThumb", 0x40}, {"RightThumb", 0x80},
            {"LeftShoulder", 0x100}, {"RightShoulder", 0x200},
            {"South", 0x1000}, {"East", 0x2000}, {"West", 0x4000}, {"North", 0x8000}
        };
        for (auto [label, value] : buttons) if (EqualControlName(label, name)) return value;
        return 0;
    }

    [[nodiscard]] constexpr const char* ControllerButtonName(std::uint32_t button) noexcept
    {
        switch (button) {
        case 1: return "DpadUp"; case 2: return "DpadDown";
        case 4: return "DpadLeft"; case 8: return "DpadRight";
        case 0x10: return "Start"; case 0x20: return "Back";
        case 0x40: return "LeftThumb"; case 0x80: return "RightThumb";
        case 0x100: return "LeftShoulder"; case 0x200: return "RightShoulder";
        case 0x1000: return "South"; case 0x2000: return "East";
        case 0x4000: return "West"; case 0x8000: return "North";
        default: return "None";
        }
    }

    struct ControllerAction
    {
        const char* name;
        const char* setting;
        const char* defaultButton;
        ImGuiKey key;
    };
    inline constexpr std::array ControllerActions{
        ControllerAction{"accept", "Accept", "South", ImGuiKey_GamepadFaceDown},
        ControllerAction{"cancel", "Cancel", "East", ImGuiKey_GamepadFaceRight},
        ControllerAction{"secondary", "Secondary", "West", ImGuiKey_GamepadFaceLeft},
        ControllerAction{"tertiary", "Tertiary", "North", ImGuiKey_GamepadFaceUp},
        ControllerAction{"previousTab", "PreviousTab", "LeftShoulder", ImGuiKey_GamepadL1},
        ControllerAction{"nextTab", "NextTab", "RightShoulder", ImGuiKey_GamepadR1},
        ControllerAction{"toggleCursor", "ToggleCursor", "RightThumb", ImGuiKey_GamepadR3}
    };
    using ControllerBindings = std::array<std::uint32_t, ControllerActions.size()>;
    inline constexpr ControllerBindings DefaultControllerBindings{0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0x80};

    [[nodiscard]] constexpr bool ValidControllerBindings(const ControllerBindings& bindings) noexcept
    {
        std::uint32_t used = 0xF; // D-pad remains available for navigation.
        for (const auto button : bindings) {
            if (!IsDigitalControllerButton(button) || (button & used)) return false;
            used |= button;
        }
        return true;
    }

    [[nodiscard]] inline float ControllerAxis(float value) noexcept
    {
        if (!std::isfinite(value) || std::abs(value) <= 0.25f) return 0;
        return std::copysign(std::clamp((std::abs(value) - 0.25f) / 0.75f, 0.0f, 1.0f), value);
    }

    // Skyrim is the sole controller source. Send releases too, including when
    // input is disarmed or handed to the optional pointer/rotation scope.
    template<class Emit>
    void SubmitControllerKeys(std::uint32_t held, float leftX, float leftY, float rightX, float rightY,
        const ControllerBindings& bindings, bool enabled, bool cursor, bool rotating, Emit emit)
    {
        for (std::size_t i = 0; i < ControllerActions.size(); ++i)
            emit(ControllerActions[i].key, enabled && !(cursor && i == 0) && (held & bindings[i]) ? 1.0f : 0.0f);
        // The screen confines rotation navigation to the preview controls.
        const bool navigate = enabled && !cursor;
        const auto direction = [&](std::uint32_t mask, ImGuiKey key, float stick) {
            emit(key, navigate ? std::max(held & mask ? 1.0f : 0.0f, stick) : 0.0f);
        };
        direction(1, ImGuiKey_GamepadDpadUp, leftY);
        direction(2, ImGuiKey_GamepadDpadDown, -leftY);
        direction(4, ImGuiKey_GamepadDpadLeft, -leftX);
        direction(8, ImGuiKey_GamepadDpadRight, leftX);
        const auto axis = [&](float value, ImGuiKey negative, ImGuiKey positive, bool active) {
            emit(negative, active ? std::max(-value, 0.0f) : 0.0f);
            emit(positive, active ? std::max(value, 0.0f) : 0.0f);
        };
        // ImGui 1.92 uses D-pad keys for focus and LStick keys for scrolling.
        // Tailor's physical left stick navigates; its right stick scrolls.
        axis(rightX, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, enabled && !rotating);
        axis(rightY, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, enabled && !rotating);
        axis(0, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, false);
        axis(0, ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp, false);
    }

    // DirectInput scan code -> ImGui key. Modifiers are deliberately absent:
    // ImGuiHost::UpdateModifiers() polls them once per frame and is the single
    // source for Ctrl/Shift/Alt. Letters and digits are mapped so the input
    // widget gets Ctrl+A / Ctrl+C / Ctrl+V / Ctrl+X and word navigation; the
    // characters themselves arrive through RE::CharEvent.
    [[nodiscard]] constexpr ImGuiKey ScanCodeToImGuiKey(std::uint32_t scanCode) noexcept
    {
        switch (scanCode) {
        case 0x01: return ImGuiKey_Escape;
        case 0x0F: return ImGuiKey_Tab;
        case 0x1C: return ImGuiKey_Enter;
        case 0x9C: return ImGuiKey_KeypadEnter;
        case 0x39: return ImGuiKey_Space;
        case 0x0E: return ImGuiKey_Backspace;
        case 0xD3: return ImGuiKey_Delete;
        case 0xD2: return ImGuiKey_Insert;
        case 0xC8: return ImGuiKey_UpArrow;
        case 0xD0: return ImGuiKey_DownArrow;
        case 0xCB: return ImGuiKey_LeftArrow;
        case 0xCD: return ImGuiKey_RightArrow;
        case 0xC9: return ImGuiKey_PageUp;
        case 0xD1: return ImGuiKey_PageDown;
        case 0xC7: return ImGuiKey_Home;
        case 0xCF: return ImGuiKey_End;
        case 0x1E: return ImGuiKey_A;
        case 0x30: return ImGuiKey_B;
        case 0x2E: return ImGuiKey_C;
        case 0x20: return ImGuiKey_D;
        case 0x12: return ImGuiKey_E;
        case 0x21: return ImGuiKey_F;
        case 0x22: return ImGuiKey_G;
        case 0x23: return ImGuiKey_H;
        case 0x17: return ImGuiKey_I;
        case 0x24: return ImGuiKey_J;
        case 0x25: return ImGuiKey_K;
        case 0x26: return ImGuiKey_L;
        case 0x32: return ImGuiKey_M;
        case 0x31: return ImGuiKey_N;
        case 0x18: return ImGuiKey_O;
        case 0x19: return ImGuiKey_P;
        case 0x10: return ImGuiKey_Q;
        case 0x13: return ImGuiKey_R;
        case 0x1F: return ImGuiKey_S;
        case 0x14: return ImGuiKey_T;
        case 0x16: return ImGuiKey_U;
        case 0x2F: return ImGuiKey_V;
        case 0x11: return ImGuiKey_W;
        case 0x2D: return ImGuiKey_X;
        case 0x15: return ImGuiKey_Y;
        case 0x2C: return ImGuiKey_Z;
        case 0x0B: return ImGuiKey_0;
        case 0x02: return ImGuiKey_1;
        case 0x03: return ImGuiKey_2;
        case 0x04: return ImGuiKey_3;
        case 0x05: return ImGuiKey_4;
        case 0x06: return ImGuiKey_5;
        case 0x07: return ImGuiKey_6;
        case 0x08: return ImGuiKey_7;
        case 0x09: return ImGuiKey_8;
        case 0x0A: return ImGuiKey_9;
        default: return ImGuiKey_None;
        }
    }

    // Characters worth typing into a field: everything printable, no control
    // codes (Backspace, Enter and Ctrl+letter arrive as keys, not characters).
    [[nodiscard]] constexpr bool IsTextCharacter(std::uint32_t code) noexcept
    {
        return code >= 0x20 && code != 0x7F && code <= 0x10FFFF &&
            !(code >= 0xD800 && code <= 0xDFFF);
    }
}
