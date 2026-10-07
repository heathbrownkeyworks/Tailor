#pragma once
#include <cstdint>
#include <functional>

namespace Tailor::ImGuiUI
{
    // Sample the predicate once per input batch, shared by the guard and callback.
    void ConfigureToggleHotkey(std::uint32_t key, std::function<bool()> predicate);
    [[nodiscard]] bool IsToggleHotkeyActive();
    // While the native menu has focus, keyboard and character input reaches
    // Tailor only. Other plugins' input listeners, and the engine's, receive
    // the rest of each batch (mouse, controller, device events) unchanged.
    [[nodiscard]] bool InstallInputDispatchHook();
    // True while the engine walks its listeners with a batch Tailor has already
    // read in full; Tailor's own listeners skip that second visit.
    [[nodiscard]] bool IsForwardingInput();
    // Whether the native menu held input focus at the last poll. Safe from any
    // thread; Papyrus natives read it on the VM's threads.
    [[nodiscard]] bool IsInputOwned();
}
