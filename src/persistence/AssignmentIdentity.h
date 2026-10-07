#pragma once

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Tailor::Persistence
{
    inline RE::FormID ReadLocalFormID(std::string_view text)
    {
        if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
        RE::FormID id = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id, 16);
        if (error != std::errc{} || end != text.data() + text.size() || id == 0 || id > 0xFFFFFF) {
            throw std::invalid_argument("invalid local FormID");
        }
        return id;
    }

    // Resolve the plugin index, not the actor. Placed references need not exist
    // in the form map at kDataLoaded; the same ID works when their cells load.
    inline RE::FormID ActorRuntimeID(RE::FormID localId, const std::string& plugin)
    {
        if (plugin.empty()) throw std::invalid_argument("missing actor plugin");
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) throw std::runtime_error("TESDataHandler unavailable");
        const auto id = dataHandler->LookupFormID(localId, plugin);
        if ((id >> 24) == 0xFE && localId > 0xFFF) throw std::invalid_argument("invalid light-plugin local FormID");
        return id;  // Zero means an unavailable plugin. Retain its row unchanged.
    }
}
