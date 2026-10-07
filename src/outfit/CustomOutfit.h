#pragma once

#include "outfit/ArmorItem.h"
#include "outfit/OutfitSex.h"
#include <string>
#include <vector>

struct CustomOutfit
{
    int                     id = 0;
    std::string             name;
    std::vector<ArmorItem>  items;
    OutfitSex               sex = OutfitSex::Unisex;
};
