#include "Settings.h"

#include <Windows.h>
#include <fstream>
#include <filesystem>

Settings& Settings::GetSingleton()
{
    static Settings singleton;
    return singleton;
}

void Settings::Load()
{
    auto path = std::filesystem::path("Data/SKSE/Plugins/Tailor.ini");

    // Only an INI that is really missing gets the defaults: one Tailor can't check is never written over.
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) {
        CreateDefaultINI(path.string());
    } else if (error) {
        logger::warn("Settings: could not check {} ({}); reading it as it is", path.string(), error.message());
    }

    auto file = path.string();

    _modifierKey = static_cast<uint32_t>(
        GetPrivateProfileIntA("Hotkey", "ModifierKey", 0x2A, file.c_str()));
    _activateKey = static_cast<uint32_t>(
        GetPrivateProfileIntA("Hotkey", "ActivateKey", 0x2C, file.c_str()));

    // ModifierKey=0 means no modifier required
    if (_activateKey == 0) {
        _activateKey = 0x2C;  // Fallback to Z if invalid
        logger::warn("Settings: ActivateKey was 0, defaulting to Z (0x2C)");
    }

    _refreshMorphs = GetPrivateProfileIntA("Compatibility", "RefreshMorphs", 1, file.c_str()) != 0;

    _grantPower = GetPrivateProfileIntA("Power", "GrantPower", 1, file.c_str()) != 0;
    _controllerEnabled = GetPrivateProfileIntA("Controller", "Enabled", 1, file.c_str()) != 0;
    char control[64]{};

    // Preserve the previous prompt preference if that optional INI is present.
    // Tailor's own setting wins; Meridian is not loaded or required.
    using namespace Tailor::ImGuiUI::input;
    char previousGlyphs[64]{};
    GetPrivateProfileStringA("Controller", "GlyphFamily", "Xbox", previousGlyphs, sizeof(previousGlyphs),
        "Data/SKSE/Plugins/MeridianUI.ini");
    GetPrivateProfileStringA("Controller", "GlyphFamily", previousGlyphs, control, sizeof(control), file.c_str());
    _controllerGlyphs = EqualControlName(control, "PlayStation") ? "playstation" :
        EqualControlName(control, "Generic") ? "generic" : "xbox";
    for (std::size_t i = 0; i < ControllerActions.size(); ++i) {
        const auto& action = ControllerActions[i];
        GetPrivateProfileStringA("Controller", action.setting, action.defaultButton, control, sizeof(control), file.c_str());
        _controllerBindings[i] = ControllerButton(control);
        _controllerBindingNames[i] = ControllerButtonName(_controllerBindings[i]);
    }
    if (!ValidControllerBindings(_controllerBindings)) {
        logger::warn("Tailor controller bindings invalid or duplicated; using the complete default mapping");
        _controllerBindings = DefaultControllerBindings;
        for (std::size_t i = 0; i < ControllerActions.size(); ++i)
            _controllerBindingNames[i] = ControllerActions[i].defaultButton;
    }

    logger::info("Settings: ModifierKey=0x{:02X}, ActivateKey=0x{:02X}, RefreshMorphs={}, GrantPower={}",
        _modifierKey, _activateKey, _refreshMorphs, _grantPower);
}

void Settings::CreateDefaultINI(const std::string& path) const
{
    // Never over the player's INI, nor one Tailor can't check.
    std::error_code error;
    const bool missing = !std::filesystem::exists(path, error) && !error;
    if (!missing) {
        logger::warn("Settings: not writing a default INI at {}; one is there, or it can't be checked", path);
        return;
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        logger::warn("Settings: Could not create default INI at {}", path);
        return;
    }

    file << R"(; Tailor Configuration
; ====================

[Hotkey]
; Hotkey to toggle the Tailor UI open/close.
; Press ModifierKey + ActivateKey together.
; Values are DirectX scan codes (decimal).
;
; Set ModifierKey=0 to use ActivateKey alone (no modifier).
;
; Common modifier keys:
;   42  = Left Shift
;   54  = Right Shift
;   29  = Left Ctrl
;   157 = Right Ctrl
;   56  = Left Alt
;   184 = Right Alt
;   0   = None (no modifier)
;
; Common keys:
;   2-11  = 1-0 (number row)
;   16    = Q       30 = A       44 = Z
;   17    = W       31 = S       45 = X
;   18    = E       32 = D       46 = C
;   19    = R       33 = F       47 = V
;   20    = T       34 = G       48 = B
;   21    = Y       35 = H       49 = N
;   22    = U       36 = J       50 = M
;   23    = I       37 = K
;   24    = O       38 = L
;   25    = P
;
; Function keys:
;   59 = F1    63 = F5    67 = F9
;   60 = F2    64 = F6    68 = F10
;   61 = F3    65 = F7    87 = F11
;   62 = F4    66 = F8    88 = F12

; Default: Shift+Z
ModifierKey=42
ActivateKey=44

[Compatibility]
; RefreshMorphs: After applying an outfit, trigger a NiNode update for
; compatible body morph mods such as AutoBody and CBPC. When OBody NG is
; detected, Tailor leaves morph updates to OBody regardless of this setting.
; Set to 0 if you experience visual glitches with RaceMenu overlays.
; Default: 1 (enabled)
RefreshMorphs=1

[Power]
; GrantPower: Grant the "Tailor" Lesser Power (from Tailor.esp) to the player
; on load, so the UI can be opened from the magic/favorites menus.
; On a controller, the power is how to open Tailor.
; Tailor keeps it in Favorites unless Disable Tailor Favorite is on in its Settings.
; Set to 0 if you only want the hotkey.
; Default: 1 (enabled)
GrantPower=1

[Controller]
; Native controller navigation. Keyboard and mouse remain available.
; No controller button opens Tailor: use the Tailor power from Favorites.
Enabled=1
; Digital names: DpadUp/Down/Left/Right, Start, Back, LeftThumb, RightThumb,
; LeftShoulder, RightShoulder, South (A), East (B), West (X), North (Y).
; R3 toggles cursor mode.
; Prompt family: Xbox, PlayStation, Generic. If omitted, uses an existing
; MeridianUI.ini preference when available, otherwise Xbox. No Meridian dependency.
;GlyphFamily=Xbox
; Optional action remaps. Names are case insensitive, unique digital buttons.
; D-pad is reserved for navigation. Invalid/duplicate bindings restore all defaults.
Accept=South
Cancel=East
Secondary=West
Tertiary=North
PreviousTab=LeftShoulder
NextTab=RightShoulder
ToggleCursor=RightThumb
)";

    logger::info("Settings: Created default INI at {}", path);
}
