#pragma once

namespace Tailor::Player
{
    // Registers Tailor's co-save record, so the player's assignments and wardrobe
    // travel with each save. Call once from SKSEPlugin_Load, after SKSE::Init.
    void RegisterCoSave();
}
