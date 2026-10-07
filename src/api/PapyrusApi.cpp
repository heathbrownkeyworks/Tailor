#include "api/PapyrusApi.h"

#include "api/ModApi.h"

#include <string>
#include <string_view>
#include <vector>

namespace Tailor::Api
{
    namespace
    {
        // The script's functions as the VM calls them: a tag for a global function first, then the Papyrus
        // parameters by value (String as BSFixedString, Int as std::int32_t, a form as its pointer, None as
        // nullptr). Each hands its arguments to the core.
        namespace Native
        {
            using Tag = RE::StaticFunctionTag;

            std::string_view Text(const RE::BSFixedString& text)
            {
                return std::string_view(text.c_str());
            }

            float GetApiVersion(Tag*) { return Api::GetApiVersion(); }

            bool DoesOutfitExist(Tag*, RE::BSFixedString asOutfit) { return Api::DoesOutfitExist(Text(asOutfit)); }
            RE::BSFixedString GetOutfit(Tag*, RE::Actor* akActor) { return RE::BSFixedString(Api::GetOutfit(akActor).c_str()); }
            std::int32_t GetOutfitGender(Tag*, RE::BSFixedString asOutfit) { return Api::GetOutfitGender(Text(asOutfit)); }
            std::vector<RE::TESObjectARMO*> GetOutfitArmors(Tag*, RE::BSFixedString asOutfit) { return Api::GetOutfitArmors(Text(asOutfit)); }
            bool OutfitHasKeyword(Tag*, RE::BSFixedString asOutfit, RE::BGSKeyword* akKeyword)
            {
                return Api::OutfitHasKeyword(Text(asOutfit), akKeyword);
            }
            bool OutfitUsesSlot(Tag*, RE::BSFixedString asOutfit, std::int32_t aiSlot) { return Api::OutfitUsesSlot(Text(asOutfit), aiSlot); }
            bool IsOutfitInCategory(Tag*, RE::BSFixedString asOutfit, RE::BSFixedString asCategory)
            {
                return Api::IsOutfitInCategory(Text(asOutfit), Text(asCategory));
            }

            bool DoesCategoryExist(Tag*, RE::BSFixedString asCategory) { return Api::DoesCategoryExist(Text(asCategory)); }
            std::vector<std::string> GetOutfitsByCategory(Tag*, RE::BSFixedString asCategory, std::int32_t aiGender)
            {
                return Api::GetOutfitsByCategory(Text(asCategory), aiGender);
            }
            std::int32_t GetOutfitCount(Tag*, RE::BSFixedString asCategory, std::int32_t aiGender)
            {
                return Api::GetOutfitCount(Text(asCategory), aiGender);
            }
            std::vector<std::string> GetAdventuringOutfits(Tag*, RE::BSFixedString asArmorType, std::int32_t aiGender)
            {
                return Api::GetAdventuringOutfits(Text(asArmorType), aiGender);
            }
            std::vector<std::string> GetOutfitsByArmorKeyword(Tag*, RE::BGSKeyword* akKeyword, std::int32_t aiGender)
            {
                return Api::GetOutfitsByArmorKeyword(akKeyword, aiGender);
            }

            bool CreateOutfit(Tag*, RE::BSFixedString asOutfit, RE::BSFixedString asCategory, std::int32_t aiGender)
            {
                return Api::CreateOutfit(Text(asOutfit), Text(asCategory), aiGender);
            }
            bool AddArmorToOutfit(Tag*, RE::BSFixedString asOutfit, RE::TESObjectARMO* akArmor)
            {
                return Api::AddArmorToOutfit(Text(asOutfit), akArmor);
            }
            bool RemoveArmorFromOutfit(Tag*, RE::BSFixedString asOutfit, RE::TESObjectARMO* akArmor)
            {
                return Api::RemoveArmorFromOutfit(Text(asOutfit), akArmor);
            }
            bool CreateCustomCategory(Tag*, RE::BSFixedString asCategory) { return Api::CreateCustomCategory(Text(asCategory)); }
            bool AddCategory(Tag*, RE::BSFixedString asOutfit, RE::BSFixedString asCategory)
            {
                return Api::AddCategory(Text(asOutfit), Text(asCategory));
            }

            RE::BSFixedString GetSituation(Tag*, RE::Actor* akActor) { return RE::BSFixedString(Api::GetSituation(akActor).c_str()); }
            bool HasSituation(Tag*, RE::Actor* akActor, RE::BSFixedString asSituation) { return Api::HasSituation(akActor, Text(asSituation)); }

            bool OverrideWithOutfit(Tag*, RE::Actor* akActor, RE::BSFixedString asOutfit)
            {
                return Api::OverrideWithOutfit(akActor, Text(asOutfit));
            }
            bool OverrideWithSituation(Tag*, RE::Actor* akActor, RE::BSFixedString asSituation)
            {
                return Api::OverrideWithSituation(akActor, Text(asSituation));
            }
            bool OverrideWithCategory(Tag*, RE::Actor* akActor, RE::BSFixedString asCategory)
            {
                return Api::OverrideWithCategory(akActor, Text(asCategory));
            }
            bool HasOutfitOverride(Tag*, RE::Actor* akActor) { return Api::HasOutfitOverride(akActor); }
            bool ClearOutfitOverride(Tag*, RE::Actor* akActor) { return Api::ClearOutfitOverride(akActor); }

            bool EvaluateOutfit(Tag*, RE::Actor* akActor) { return Api::EvaluateOutfit(akActor); }
        }

        // Binds natives on the VM and counts them for the log.
        struct Registrar
        {
            RE::BSScript::IVirtualMachine* vm;
            int count = 0;

            // RegisterFunction's default leaves each one not callable from tasklets: the VM runs every call
            // in step with the game thread, which the core's actor functions require.
            template <class F>
            void RegisterFunction(std::string_view name, std::string_view script, F function)
            {
                vm->RegisterFunction(name, script, function);
                ++count;
            }
        };

        bool Register(RE::BSScript::IVirtualMachine* vm)
        {
            Registrar natives{vm};
            natives.RegisterFunction("GetApiVersion", "Tailor", Native::GetApiVersion);

            natives.RegisterFunction("DoesOutfitExist", "Tailor", Native::DoesOutfitExist);
            natives.RegisterFunction("GetOutfit", "Tailor", Native::GetOutfit);
            natives.RegisterFunction("GetOutfitGender", "Tailor", Native::GetOutfitGender);
            natives.RegisterFunction("GetOutfitArmors", "Tailor", Native::GetOutfitArmors);
            natives.RegisterFunction("OutfitHasKeyword", "Tailor", Native::OutfitHasKeyword);
            natives.RegisterFunction("OutfitUsesSlot", "Tailor", Native::OutfitUsesSlot);
            natives.RegisterFunction("IsOutfitInCategory", "Tailor", Native::IsOutfitInCategory);

            natives.RegisterFunction("DoesCategoryExist", "Tailor", Native::DoesCategoryExist);
            natives.RegisterFunction("GetOutfitsByCategory", "Tailor", Native::GetOutfitsByCategory);
            natives.RegisterFunction("GetOutfitCount", "Tailor", Native::GetOutfitCount);
            natives.RegisterFunction("GetAdventuringOutfits", "Tailor", Native::GetAdventuringOutfits);
            natives.RegisterFunction("GetOutfitsByArmorKeyword", "Tailor", Native::GetOutfitsByArmorKeyword);

            natives.RegisterFunction("CreateOutfit", "Tailor", Native::CreateOutfit);
            natives.RegisterFunction("AddArmorToOutfit", "Tailor", Native::AddArmorToOutfit);
            natives.RegisterFunction("RemoveArmorFromOutfit", "Tailor", Native::RemoveArmorFromOutfit);
            natives.RegisterFunction("CreateCustomCategory", "Tailor", Native::CreateCustomCategory);
            natives.RegisterFunction("AddCategory", "Tailor", Native::AddCategory);

            natives.RegisterFunction("GetSituation", "Tailor", Native::GetSituation);
            natives.RegisterFunction("HasSituation", "Tailor", Native::HasSituation);

            natives.RegisterFunction("OverrideWithOutfit", "Tailor", Native::OverrideWithOutfit);
            natives.RegisterFunction("OverrideWithSituation", "Tailor", Native::OverrideWithSituation);
            natives.RegisterFunction("OverrideWithCategory", "Tailor", Native::OverrideWithCategory);
            natives.RegisterFunction("HasOutfitOverride", "Tailor", Native::HasOutfitOverride);
            natives.RegisterFunction("ClearOutfitOverride", "Tailor", Native::ClearOutfitOverride);

            natives.RegisterFunction("EvaluateOutfit", "Tailor", Native::EvaluateOutfit);

            logger::info("Tailor API: registered {} Papyrus functions", natives.count);
            return true;
        }
    }

    void RegisterPapyrus()
    {
        auto* papyrus = SKSE::GetPapyrusInterface();
        if (!papyrus || !papyrus->Register(Register)) logger::error("Tailor API: the Papyrus functions could not be registered");
    }
}
