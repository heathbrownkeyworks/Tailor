#pragma once

#include <cstdint>
#include <string_view>

namespace Tailor::Outfits
{
    // Diagnostic-only snapshot of existing inventory and the loaded actor graph.
    // Does not initialize inventory, change equipment or retain scene nodes.
    void LogPreviewEquipment(RE::Actor* actor, std::string_view phase, std::uint64_t generation);
}
