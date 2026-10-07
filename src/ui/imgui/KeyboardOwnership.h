#pragma once
#include <bitset>
#include <cstdint>

namespace Tailor::ImGuiUI::input
{
    // Decides which keyboard events other input listeners may see while Tailor
    // owns the keyboard. A press is hidden as a whole: once its down is hidden,
    // its repeats and its release are too, even after ownership ends; once its
    // down is seen, its release is always delivered. No listener is left with a
    // stuck key, or handed a release for a key it never saw pressed.
    class KeyboardOwnership
    {
    public:
        enum class Phase { Down, Held, Up };
        // The engine's screenshot key stays live so outfits can be captured.
        static constexpr std::uint32_t PrintScreen = 0xB7;

        [[nodiscard]] bool HideKey(bool owned, std::uint32_t scanCode, Phase phase)
        {
            if (scanCode >= _hidden.size()) return false;
            if (phase == Phase::Down) _hidden[scanCode] = owned && scanCode != PrintScreen;
            const bool hide = _hidden[scanCode];
            if (phase == Phase::Up) _hidden[scanCode] = false;
            return hide;
        }
        // Typed characters have no release to balance; they follow ownership directly.
        [[nodiscard]] static bool HideCharacter(bool owned) { return owned; }

    private:
        std::bitset<256> _hidden;
    };
}
