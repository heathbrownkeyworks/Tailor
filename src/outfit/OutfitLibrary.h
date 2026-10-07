#pragma once

#include "outfit/OutfitCategory.h"
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class OutfitLibrary
{
public:
    // Whether an NPC can wear a saved outfit at all: it exists and fits the NPC's sex.
    using Wearable = std::function<bool(int)>;

    static OutfitLibrary& GetSingleton();

    // Reads library.json. Saves stay off unless the whole file was read, and only a complete load merges legacy
    // defaults or adds missing pools. A missing file is the defaults with saves on; Load never creates a file.
    void Load();
    // False, and nothing written, while saves are off or when the file couldn't be written.
    bool Save() const;
    // Whether the last load read the whole file, so changes can be saved. Tailor's screens and the mod API change no
    // category while it is false.
    bool SaveAllowed() const;

    const std::vector<OutfitCategory>& GetCategories() const;
    std::string GetCategoryDisplayName(int id) const;
    const OutfitCategory* GetCategoryById(int id) const;
    // Combine legacy and new pools, with each outfit ID represented once.
    std::vector<int> GetSituationOutfitIds(const std::string& sitType) const;
    std::vector<int> FilterByArmorType(const std::vector<int>& outfitIds, OutfitArmorType type) const;
    // What an NPC limited to `type` may wear for Adventuring: outfits that are in the
    // Adventuring pool AND in a category of that armor type. When nothing is in both,
    // the type cannot be honored and all of Adventuring stands in. Any asks for nothing.
    // Only outfits the NPC can wear count, including when deciding nothing is in both.
    std::vector<int> FilterAdventuringEligible(const std::vector<int>& outfitIds, OutfitArmorType type, const Wearable& wearable) const;
    bool IsAdventuringEligible(int outfitId, OutfitArmorType type, const Wearable& wearable) const;
    bool HasAdventuringOutfitsOfType(OutfitArmorType type, const Wearable& wearable) const;

    // Category names are unique ignoring capitals and surrounding spaces (OutfitNamePolicy.h).
    // Older libraries may still hold same-named categories; those keep showing as "Warm (2)".
    bool CategoryNameTaken(std::string_view name, int exceptId = 0) const;
    // The category whose display name (GetCategoryDisplayName) is this one, ignoring capitals and surrounding spaces.
    std::optional<int> FindCategoryByDisplayName(std::string_view name) const;
    // A copy taken under the lock, safe from any thread (the mod API reads from Papyrus).
    std::vector<OutfitCategory> Snapshot() const;

    // Returns the new id, or 0 when the name is empty or another category already has it, or no id is left.
    int  AddCategory(const std::string& name);
    // Returns false when the category does not exist, or the name is empty or another category has it.
    bool RenameCategory(int id, const std::string& newName);
    bool DeleteCategory(int id);

    bool AddOutfitToCategory(int categoryId, int outfitId);
    bool RemoveOutfitFromCategory(int categoryId, int outfitId);

    // Caller validates category IDs before saving. Retained memberships keep their order.
    void SetOutfitCategories(int outfitId, const std::vector<int>& categoryIds);

    // Remove an outfit ID from ALL categories (used when deleting an outfit)
    void RemoveOutfitFromAllCategories(int outfitId);

private:
    friend class OutfitTransfer;
    static std::string Serialize(const std::vector<OutfitCategory>& categories, int nextId);
    OutfitLibrary() = default;
    OutfitLibrary(const OutfitLibrary&) = delete;
    OutfitLibrary& operator=(const OutfitLibrary&) = delete;

    std::filesystem::path GetLibraryPath() const;
    void CreateDefaults();
    bool MergeLegacyDefaultCategories();  // Caller holds _mutex; backs up before migration.
    bool NameTakenLocked(std::string_view name, int exceptId) const;  // Caller holds _mutex.
    // Every category's id and display label, in order. Caller holds _mutex.
    std::vector<std::pair<int, std::string>> DisplayLabels() const;

    std::vector<OutfitCategory> _categories;
    int                         _nextId = 1;
    bool                        _saveAllowed = false;  // A failed or incomplete load must never replace the file.
    mutable std::mutex          _mutex;
};
