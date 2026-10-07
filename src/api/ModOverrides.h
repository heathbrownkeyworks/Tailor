#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Mod API overrides: who a mod has put in which Tailor outfit, until it lets go. Per save (co-save
// record OVRD). Where Tailor decides an outfit it asks here first (SituationHandler).
class ModOverrides
{
public:
    static ModOverrides& GetSingleton();

    // `situation`: the one it was set for (OverrideWithSituation), 0 for an outfit or a category.
    void Set(RE::FormID actor, int outfitId, int situation = 0);
    // False when that person had no override.
    bool Clear(RE::FormID actor);
    [[nodiscard]] std::optional<int> Get(RE::FormID actor) const;
    // The situation the person's override was set for, 0 for an outfit or a category; nothing without one.
    [[nodiscard]] std::optional<int> SituationOf(RE::FormID actor) const;
    [[nodiscard]] bool Contains(RE::FormID actor) const;
    [[nodiscard]] std::vector<RE::FormID> Actors() const;
    // A deleted outfit: every override using it goes; returns who had one.
    std::vector<RE::FormID> ForgetOutfit(int outfitId);

    [[nodiscard]] std::string Save() const;
    void Load(std::string_view payload, const SKSE::SerializationInterface* serialization);
    void ClearAll();

private:
    ModOverrides() = default;

    struct Override
    {
        int outfit = 0;
        int situation = 0;
    };
    std::unordered_map<RE::FormID, Override> _overrides;
    // Never held while calling out.
    mutable std::mutex _mutex;
};
