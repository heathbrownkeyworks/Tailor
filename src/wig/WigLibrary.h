#pragma once

#include "wig/WigCategory.h"
#include <array>
#include <cstdint>
#include <vector>
#include <mutex>
#include <filesystem>

class WigLibrary
{
public:
    static WigLibrary& GetSingleton();

    void Load();
    void Save() const;

    std::vector<WigEntry> GetCategory(WigCategory cat) const;
    size_t GetCategoryCount(WigCategory cat) const;

    void AddWig(WigCategory cat, const WigEntry& entry);
    bool RemoveWig(WigCategory cat, const WigEntry& entry);
    bool HasWig(WigCategory cat, const WigEntry& entry) const;
    // Every library wig that resolves, as forms.
    std::vector<RE::TESObjectARMO*> ResolvedWigs() const;
    // Counts every change to the library (load, add, remove, clear), so a copy of its wigs knows when it is stale.
    std::uint64_t Revision() const;

    void Clear();

private:
    WigLibrary() = default;
    ~WigLibrary() = default;

    WigLibrary(const WigLibrary&) = delete;
    WigLibrary& operator=(const WigLibrary&) = delete;

    std::filesystem::path GetLibraryPath() const;

    std::array<std::vector<WigEntry>, kCategoryCount> _categories;
    std::uint64_t _revision = 0;
    bool _saveAllowed = false;
    mutable std::mutex _mutex;
};
