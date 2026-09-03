#pragma once

#include <string>
#include <vector>

struct OutfitCategory
{
    int              id = 0;
    std::string      name;
    std::string      sex;            // "female", "male", or "" (unisex)
    bool             isDefault = false;
    std::string      situationType;  // "", "adventuring", "town", "home", "sleep"
    std::vector<int> outfitIds;      // references to CustomOutfit IDs in OutfitStore
};
