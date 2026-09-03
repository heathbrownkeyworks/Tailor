#pragma once

#include "wig/WigCategory.h"
#include <array>
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

    void Clear();

private:
    WigLibrary() = default;
    ~WigLibrary() = default;

    WigLibrary(const WigLibrary&) = delete;
    WigLibrary& operator=(const WigLibrary&) = delete;

    std::filesystem::path GetLibraryPath() const;

    std::array<std::vector<WigEntry>, kCategoryCount> _categories;
    mutable std::mutex _mutex;
};
