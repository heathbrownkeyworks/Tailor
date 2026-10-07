#include "api/ModOverrides.h"

#include "api/ModApiPolicy.h"
#include "outfit/OutfitStore.h"

ModOverrides& ModOverrides::GetSingleton()
{
    static ModOverrides singleton;
    return singleton;
}

void ModOverrides::Set(RE::FormID actor, int outfitId, int situation)
{
    std::scoped_lock lock(_mutex);
    _overrides[actor] = {outfitId, situation};
}

bool ModOverrides::Clear(RE::FormID actor)
{
    std::scoped_lock lock(_mutex);
    return _overrides.erase(actor) > 0;
}

std::optional<int> ModOverrides::Get(RE::FormID actor) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _overrides.find(actor);
    return it != _overrides.end() ? std::optional<int>(it->second.outfit) : std::nullopt;
}

std::optional<int> ModOverrides::SituationOf(RE::FormID actor) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _overrides.find(actor);
    return it != _overrides.end() ? std::optional<int>(it->second.situation) : std::nullopt;
}

bool ModOverrides::Contains(RE::FormID actor) const
{
    std::scoped_lock lock(_mutex);
    return _overrides.contains(actor);
}

std::vector<RE::FormID> ModOverrides::Actors() const
{
    std::scoped_lock lock(_mutex);
    std::vector<RE::FormID> actors;
    actors.reserve(_overrides.size());
    for (const auto& [actor, entry] : _overrides) actors.push_back(actor);
    return actors;
}

std::vector<RE::FormID> ModOverrides::ForgetOutfit(int outfitId)
{
    std::scoped_lock lock(_mutex);
    std::vector<RE::FormID> forgotten;
    for (auto it = _overrides.begin(); it != _overrides.end();) {
        if (it->second.outfit == outfitId) {
            forgotten.push_back(it->first);
            it = _overrides.erase(it);
        } else {
            ++it;
        }
    }
    return forgotten;
}

std::string ModOverrides::Save() const
{
    std::vector<Tailor::Api::OverrideEntry> entries;
    {
        std::scoped_lock lock(_mutex);
        entries.reserve(_overrides.size());
        for (const auto& [actor, entry] : _overrides) entries.push_back({actor, entry.outfit, entry.situation});
    }
    return Tailor::Api::EncodeOverrides(entries);
}

void ModOverrides::Load(std::string_view payload, const SKSE::SerializationInterface* serialization)
{
    std::size_t skipped = 0;
    const auto entries = Tailor::Api::DecodeOverrides(payload, skipped);
    // The outfit store has its own lock, so the map is built first and swapped in under ours.
    std::unordered_map<RE::FormID, Override> loaded;
    for (const auto& entry : entries) {
        RE::FormID actor = 0;
        if (!serialization->ResolveFormID(entry.actor, actor) || !OutfitStore::GetSingleton().GetOutfitCopy(entry.outfit)) {
            ++skipped;
            continue;
        }
        loaded[actor] = {entry.outfit, entry.situation};
    }
    const auto count = loaded.size();
    {
        std::scoped_lock lock(_mutex);
        _overrides = std::move(loaded);
    }
    if (skipped) logger::warn("Mod API: {} saved override(s) could not be read, resolved or no longer have their outfit", skipped);
    logger::info("Mod API: {} override(s) loaded", count);
}

void ModOverrides::ClearAll()
{
    std::scoped_lock lock(_mutex);
    _overrides.clear();
}
