#pragma once

#include "outfit/OutfitCategory.h"
#include <filesystem>
#include <mutex>
#include <vector>

class OutfitLibrary
{
public:
    static OutfitLibrary& GetSingleton();

    void Load();
    void Save() const;

    const std::vector<OutfitCategory>& GetCategories() const;
    std::string GetCategoryDisplayName(int id) const;
    const OutfitCategory* GetCategoryById(int id) const;
    // Combine legacy and new pools, with each outfit ID represented once.
    std::vector<int> GetSituationOutfitIds(const std::string& sitType) const;

    int  AddCategory(const std::string& name);
    bool RenameCategory(int id, const std::string& newName);
    bool DeleteCategory(int id);

    bool AddOutfitToCategory(int categoryId, int outfitId);
    bool RemoveOutfitFromCategory(int categoryId, int outfitId);

    // Caller validates category IDs before saving. Retained memberships keep their order.
    void SetOutfitCategories(int outfitId, const std::vector<int>& categoryIds);

    // Remove an outfit ID from ALL categories (used when deleting an outfit)
    void RemoveOutfitFromAllCategories(int outfitId);

private:
    OutfitLibrary() = default;
    OutfitLibrary(const OutfitLibrary&) = delete;
    OutfitLibrary& operator=(const OutfitLibrary&) = delete;

    std::filesystem::path GetLibraryPath() const;
    void CreateDefaults();
    bool MergeLegacyDefaultCategories();  // Caller holds _mutex; backs up before migration.

    std::vector<OutfitCategory> _categories;
    int                         _nextId = 1;
    mutable std::mutex          _mutex;
};
