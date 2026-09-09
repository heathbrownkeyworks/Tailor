#pragma once
#include "outfit/OutfitCategory.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class OutfitTransfer
{
public:
    static std::string CategoryKey(const OutfitCategory& category);
    static std::string ValidateExportName(const std::string& name); // Empty means valid.
    static bool IsImportFilename(const std::string& filename);
    static nlohmann::json Catalog();
    static std::vector<std::string> ListFiles();
    static nlohmann::json Export(const std::string& name, const std::vector<int>& ids);
    static nlohmann::json Import(const std::string& filename);
};
