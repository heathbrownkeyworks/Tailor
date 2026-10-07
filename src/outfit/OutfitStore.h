#pragma once

#include "outfit/CustomOutfit.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

class OutfitStore
{
public:
    static OutfitStore& GetSingleton();

    // Reads outfits.json. Saves stay off unless the whole file was read: a file Tailor couldn't read, or read only in
    // part, is never written over. A missing file is an empty store with saves on; Load never creates a file.
    void Load();
    // False, and nothing written, while saves are off or when the file couldn't be written.
    bool Save() const;
    // Whether the last load read the whole file, so changes can be saved. Tailor's screens and the mod API change no
    // outfit while it is false.
    bool SaveAllowed() const;

    // Custom outfit CRUD
    const std::vector<CustomOutfit>& GetOutfits() const;
    const CustomOutfit*              GetOutfitById(int id) const;
    // Returns the new id, or 0 when the name is empty or another outfit already has it, or no id is left.
    int                              AddOutfit(const std::string& name, const std::vector<ArmorItem>& items, OutfitSex sex = OutfitSex::Unisex);
    // Takes back the outfit AddOutfit just made, when its save failed, so its id never reaches an assignment or the
    // co-save. Only the newest outfit can be taken back, and its id is not handed out again.
    bool                             DiscardNewOutfit(int id);
    // Returns false when the outfit does not exist, or the name is empty or another outfit has it.
    bool                             UpdateOutfit(int id, const std::string& name, const std::vector<ArmorItem>& items, OutfitSex sex);
    bool                             DeleteOutfit(int id);
    // Outfit names are unique ignoring capitals and surrounding spaces (OutfitNamePolicy.h).
    bool                             NameTaken(std::string_view name, int exceptId = 0) const;
    // Copies taken under the lock, safe from any thread (the mod API reads from Papyrus).
    std::optional<CustomOutfit>      FindByName(std::string_view name) const;
    std::optional<CustomOutfit>      GetOutfitCopy(int id) const;
    std::vector<CustomOutfit>        Snapshot() const;
    // How many listed outfits exist, and which of them this call changed.
    struct SexTagResult
    {
        int              tagged = 0;
        std::vector<int> changed;
    };
    // Gives every listed outfit that exists this sex.
    SexTagResult                     SetOutfitSex(const std::vector<int>& ids, OutfitSex sex);
    // Whether the outfit exists and fits an NPC of this sex (-1: unknown, fits all).
    bool                             Fits(int id, int npcSex) const;

    // Armor scanning — returns mods containing ARMO records
    std::vector<ModArmorList> ScanArmorMods() const;

    // Get armors for a specific plugin
    std::vector<ArmorItem> GetArmorForPlugin(const std::string& plugin) const;

    // List plugins that contain armor records (for the dropdown)
    std::vector<std::string> GetArmorPluginNames() const;

private:
    friend class OutfitTransfer;
    static std::string Serialize(const std::vector<CustomOutfit>& outfits, int nextId);
    OutfitStore() = default;
    OutfitStore(const OutfitStore&) = delete;
    OutfitStore& operator=(const OutfitStore&) = delete;

    std::filesystem::path GetStorePath() const;
    // Gives each outfit that shares an earlier outfit's id a new one, after outfits.json is copied to a numbered
    // outfits.pre-unique-ids.json that is never overwritten; without that copy nothing changes. Category memberships
    // and assignments stay with the first outfit of each id. Caller holds _mutex.
    bool RenumberDuplicateIds(const std::filesystem::path& path);

    std::vector<CustomOutfit> _outfits;
    int                       _nextId = 1;
    bool                      _saveAllowed = false;  // A failed or incomplete load must never replace the file.
    mutable std::mutex        _mutex;
};
