#pragma once

#include "outfit/Utf8.h"

#include <cstdint>
#include <string>

struct ArmorItem
{
    RE::FormID  formId = 0;
    std::string plugin;
    std::string name;

    bool operator==(const ArmorItem& other) const
    {
        return formId == other.formId && plugin == other.plugin;
    }

    RE::TESObjectARMO* Resolve() const
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;
        auto* form = dataHandler->LookupForm(formId, plugin);
        return form ? form->As<RE::TESObjectARMO>() : nullptr;
    }
};

struct ModArmorList
{
    std::string             modName;
    std::vector<ArmorItem>  armors;
};
