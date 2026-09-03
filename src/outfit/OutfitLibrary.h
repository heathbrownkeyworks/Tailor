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
    std::vector<OutfitCategory> GetCategoriesForSex(const std::string& sex) const;
    const OutfitCategory* GetCategoryById(int id) const;
    // Situation pool lookup — `sex` must be "female" or "male". v2.0+ situation pools are sex-specific.
    const OutfitCategory* GetCategoryBySituationType(const std::string& sitType, const std::string& sex) const;

    int  AddCategory(const std::string& name, const std::string& sex);
    bool RenameCategory(int id, const std::string& newName);
    bool DeleteCategory(int id);

    bool AddOutfitToCategory(int categoryId, int outfitId);
    bool RemoveOutfitFromCategory(int categoryId, int outfitId);

    // Remove an outfit ID from ALL categories (used when deleting an outfit)
    void RemoveOutfitFromAllCategories(int outfitId);

private:
    OutfitLibrary() = default;
    OutfitLibrary(const OutfitLibrary&) = delete;
    OutfitLibrary& operator=(const OutfitLibrary&) = delete;

    std::filesystem::path GetLibraryPath() const;
    void CreateDefaults();

    std::vector<OutfitCategory> _categories;
    int                         _nextId = 1;
    mutable std::mutex          _mutex;
};
