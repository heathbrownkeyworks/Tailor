#pragma once

#include <vector>

// A temporary situation owns a copy of Tailor's mutable outfit contents.
// Native outfit pointers and change flags are restored as captured, including null.
struct OutfitSnapshot
{
    RE::BGSOutfit* defaultOutfit = nullptr;
    RE::BGSOutfit* sleepOutfit = nullptr;
    bool defaultWasActorPair = false;
    bool sleepWasActorPair = false;
    bool defaultHadChange = false;
    bool sleepHadChange = false;
    std::vector<RE::TESForm*> items;
    int outfitId = 0;
};
