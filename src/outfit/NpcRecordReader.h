#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The game keeps only the winning override of an NPC in memory, so what the NPC's
// own plugin wrote can only be learned from that plugin's file. This reads it
// without the engine: no game state is touched and it can be tested on a desktop.
namespace Tailor::Records
{
    // A form as a plugin file names it: the owning plugin and the ID within it.
    struct FormRef
    {
        std::string plugin;
        std::uint32_t localId = 0;  // lower 24 bits, as written in the file
        bool operator==(const FormRef&) const = default;
    };

    struct NpcOutfits
    {
        std::optional<FormRef> defaultOutfit;  // DOFT; empty when the record names none
        std::optional<FormRef> sleepOutfit;    // SOFT
        bool usesTemplateInventory = false;    // the outfit worn is the template's, not this record's
    };

    // DOFT/SOFT exactly as `file` wrote them for the NPC `owner`:`localId`. Empty
    // when the file holds no such NPC record or cannot be read.
    std::optional<NpcOutfits> ReadNpcOutfits(const std::filesystem::path& file, std::string_view owner, std::uint32_t localId);
}
