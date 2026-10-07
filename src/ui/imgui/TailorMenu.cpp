#include "ui/imgui/TailorMenu.h"
#include "ui/imgui/ImGuiHost.h"

namespace Tailor::ImGuiUI
{
    TailorMenu::TailorMenu()
    {
        using Flag = RE::UI_MENU_FLAGS;
        menuFlags.set(Flag::kUsesCursor, Flag::kUpdateUsesCursor,
            Flag::kUsesMenuContext, Flag::kCustomRendering, Flag::kDisablePauseMenu);
        inputContext.set(RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode);
        depthPriority = 11;
    }

    bool TailorMenu::Register()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        ui->Register(MENU_NAME, Create);
        return true;
    }

    RE::IMenu* TailorMenu::Create() { return new TailorMenu(); }
    void TailorMenu::PostDisplay() { ImGuiHost::GetSingleton().DrawFrame(); }

    RE::UI_MESSAGE_RESULTS TailorMenu::ProcessMessage(RE::UIMessage& message)
    {
        switch (*message.type) {
        case RE::UI_MESSAGE_TYPE::kShow:
            ImGuiHost::GetSingleton().OnShown();
            break;
        case RE::UI_MESSAGE_TYPE::kHide:
        case RE::UI_MESSAGE_TYPE::kForceHide:
            ImGuiHost::GetSingleton().OnHidden();
            break;
        case RE::UI_MESSAGE_TYPE::kUserEvent:
        case RE::UI_MESSAGE_TYPE::kScaleformEvent:
            // Raw input is queued to ImGui. Screen-scoped Back is handled there,
            // so a cancel user event cannot also close the entire menu.
            return RE::UI_MESSAGE_RESULTS::kHandled;
        default:
            break;
        }
        return RE::IMenu::ProcessMessage(message);
    }
}
