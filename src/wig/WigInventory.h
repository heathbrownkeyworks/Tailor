#pragma once

#include <cstdint>
#include <unordered_set>

namespace RE { class Actor; }

namespace Tailor::Wigs
{
    // Explicit Default Hair only: remove every inventory copy of the supplied
    // runtime wig forms, including copies not added by Tailor. Game-thread only.
    bool RemoveInventoryWigs(RE::Actor* actor, const std::unordered_set<std::uint32_t>& wigForms);
}
