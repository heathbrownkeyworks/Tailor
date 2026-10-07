#pragma once

namespace Tailor::ImGuiUI
{
    // Deliberately unpaused: the actual loaded NPC keeps its AI and animation.
    class TailorMenu final : public RE::IMenu
    {
    public:
        static constexpr std::string_view MENU_NAME = "TailorNativeMenu";
        static bool Register();
        static RE::IMenu* Create();
        void PostDisplay() override;
        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage&) override;
    private:
        TailorMenu();
    };
}
