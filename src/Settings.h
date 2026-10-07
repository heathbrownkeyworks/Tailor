#pragma once

#include <cstdint>
#include <string>
#include "ui/imgui/InputMap.h"

class Settings
{
public:
    static Settings& GetSingleton();

    void Load();

    uint32_t GetModifierKey() const { return _modifierKey; }
    uint32_t GetActivateKey() const { return _activateKey; }
    bool GetRefreshMorphs() const { return _refreshMorphs; }
    bool GetGrantPower() const { return _grantPower; }
    bool GetControllerEnabled() const { return _controllerEnabled; }
    const Tailor::ImGuiUI::input::ControllerBindings& GetControllerBindings() const { return _controllerBindings; }
    const std::array<std::string, 7>& GetControllerBindingNames() const { return _controllerBindingNames; }
    const std::string& GetControllerGlyphs() const { return _controllerGlyphs; }

private:
    Settings() = default;

    void CreateDefaultINI(const std::string& path) const;

    uint32_t _modifierKey = 0x2A;  // LShift
    uint32_t _activateKey = 0x2C;  // Z
    bool _refreshMorphs = true;
    bool _grantPower = true;
    bool _controllerEnabled = true;
    Tailor::ImGuiUI::input::ControllerBindings _controllerBindings = Tailor::ImGuiUI::input::DefaultControllerBindings;
    std::array<std::string, 7> _controllerBindingNames;
    std::string _controllerGlyphs = "xbox";
};
