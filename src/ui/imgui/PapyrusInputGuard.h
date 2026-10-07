#pragma once

namespace Tailor::ImGuiUI
{
    // Scripts that poll SKSE's Input.IsKeyPressed / GetNumKeysPressed /
    // GetNthKeyPressed read raw device state beneath the game's input events, so
    // the dispatch guard cannot reach them. While the native menu owns input these
    // three report that nothing is pressed; at any other time they are SKSE's own.
    // Call once the VM and SKSE's natives exist (kDataLoaded).
    [[nodiscard]] bool InstallPapyrusInputGuard();
}
