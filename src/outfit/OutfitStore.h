#pragma once

#include "outfit/CustomOutfit.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

class OutfitStore
{
public:
    static OutfitStore& GetSingleton();

    void Load();
    void Save() const;

    // Custom outfit CRUD
    const std::vector<CustomOutfit>& GetOutfits() const;
    const CustomOutfit*              GetOutfitById(int id) const;
    int                              AddOutfit(const std::string& name, const std::vector<ArmorItem>& items);
    bool                             UpdateOutfit(int id, const std::string& name, const std::vector<ArmorItem>& items);
    bool                             DeleteOutfit(int id);

    // Armor scanning — returns mods containing ARMO records
    std::vector<ModArmorList> ScanArmorMods() const;

    // Get armors for a specific plugin
    std::vector<ArmorItem> GetArmorForPlugin(const std::string& plugin) const;

    // List plugins that contain armor records (for the dropdown)
    std::vector<std::string> GetArmorPluginNames() const;

private:
    OutfitStore() = default;
    OutfitStore(const OutfitStore&) = delete;
    OutfitStore& operator=(const OutfitStore&) = delete;

    std::filesystem::path GetStorePath() const;

    std::vector<CustomOutfit> _outfits;
    int                       _nextId = 1;
    mutable std::mutex        _mutex;
};
