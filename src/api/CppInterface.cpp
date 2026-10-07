#include "api/TailorInterface1.h"

#include "api/ModApi.h"

#include <string>
#include <string_view>
#include <vector>

namespace Tailor::Api
{
    namespace
    {
        using TailorAPI::ArmorCallback;
        using TailorAPI::StringCallback;

        // A string from another plugin; nullptr counts as "".
        std::string_view Text(const char* text)
        {
            return text ? std::string_view(text) : std::string_view{};
        }

        // Hands each value to the caller's callback, in order; each string lives until its call returns.
        void Each(const std::vector<std::string>& values, StringCallback callback, void* context)
        {
            if (!callback) return;
            for (const auto& value : values) callback(value.c_str(), context);
        }

        // Version 1 of the C++ interface: each function hands its arguments to the core, as the Papyrus
        // natives do, and gives strings and lists back through the caller's callback.
        class Interface final : public TailorAPI::ITailorInterface1
        {
        public:
            float GetApiVersion() override { return Api::GetApiVersion(); }

            bool DoesOutfitExist(const char* outfit) override { return Api::DoesOutfitExist(Text(outfit)); }
            bool GetOutfit(RE::Actor* actor, StringCallback callback, void* context) override
            {
                // The core logs a missing actor, and a child.
                const auto outfit = Api::GetOutfit(actor);
                if (!actor || actor->IsChild()) return false;
                if (callback) callback(outfit.c_str(), context);
                return true;
            }
            int GetOutfitGender(const char* outfit) override { return Api::GetOutfitGender(Text(outfit)); }
            void GetOutfitArmors(const char* outfit, ArmorCallback callback, void* context) override
            {
                const auto armors = Api::GetOutfitArmors(Text(outfit));
                if (!callback) return;
                for (auto* armor : armors) callback(armor, context);
            }
            bool OutfitHasKeyword(const char* outfit, RE::BGSKeyword* keyword) override
            {
                return Api::OutfitHasKeyword(Text(outfit), keyword);
            }
            bool OutfitUsesSlot(const char* outfit, int slot) override { return Api::OutfitUsesSlot(Text(outfit), slot); }
            bool IsOutfitInCategory(const char* outfit, const char* category) override
            {
                return Api::IsOutfitInCategory(Text(outfit), Text(category));
            }

            bool DoesCategoryExist(const char* category) override { return Api::DoesCategoryExist(Text(category)); }
            void GetOutfitsByCategory(const char* category, int gender, StringCallback callback, void* context) override
            {
                Each(Api::GetOutfitsByCategory(Text(category), gender), callback, context);
            }
            int GetOutfitCount(const char* category, int gender) override { return Api::GetOutfitCount(Text(category), gender); }
            void GetAdventuringOutfits(const char* armorType, int gender, StringCallback callback, void* context) override
            {
                Each(Api::GetAdventuringOutfits(Text(armorType), gender), callback, context);
            }
            void GetOutfitsByArmorKeyword(RE::BGSKeyword* keyword, int gender, StringCallback callback, void* context) override
            {
                Each(Api::GetOutfitsByArmorKeyword(keyword, gender), callback, context);
            }

            bool CreateOutfit(const char* outfit, const char* category, int gender) override
            {
                return Api::CreateOutfit(Text(outfit), Text(category), gender);
            }
            bool AddArmorToOutfit(const char* outfit, RE::TESObjectARMO* armor) override
            {
                return Api::AddArmorToOutfit(Text(outfit), armor);
            }
            bool RemoveArmorFromOutfit(const char* outfit, RE::TESObjectARMO* armor) override
            {
                return Api::RemoveArmorFromOutfit(Text(outfit), armor);
            }
            bool CreateCustomCategory(const char* category) override { return Api::CreateCustomCategory(Text(category)); }
            bool AddCategory(const char* outfit, const char* category) override
            {
                return Api::AddCategory(Text(outfit), Text(category));
            }

            bool GetSituation(RE::Actor* actor, StringCallback callback, void* context) override
            {
                // The core logs a missing actor, and a child.
                const auto situation = Api::GetSituation(actor);
                if (!actor || actor->IsChild()) return false;
                if (callback) callback(situation.c_str(), context);
                return true;
            }

            bool OverrideWithOutfit(RE::Actor* actor, const char* outfit) override
            {
                return Api::OverrideWithOutfit(actor, Text(outfit));
            }
            bool OverrideWithSituation(RE::Actor* actor, const char* situation) override
            {
                return Api::OverrideWithSituation(actor, Text(situation));
            }
            bool OverrideWithCategory(RE::Actor* actor, const char* category) override
            {
                return Api::OverrideWithCategory(actor, Text(category));
            }
            bool HasOutfitOverride(RE::Actor* actor) override { return Api::HasOutfitOverride(actor); }
            bool ClearOutfitOverride(RE::Actor* actor) override { return Api::ClearOutfitOverride(actor); }

            bool EvaluateOutfit(RE::Actor* actor) override { return Api::EvaluateOutfit(actor); }

            void AddListener(TailorAPI::ITailorListener1* listener) override { Api::AddListener(listener); }
            void RemoveListener(TailorAPI::ITailorListener1* listener) override { Api::RemoveListener(listener); }

            bool HasSituation(RE::Actor* actor, const char* situation) override
            {
                return Api::HasSituation(actor, Text(situation));
            }
        };
    }

    TailorAPI::ITailorInterface1* Interface1()
    {
        static Interface instance;
        return &instance;
    }
}
