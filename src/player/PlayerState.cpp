#include "player/PlayerState.h"
#include "player/PlayerEquipment.h"
#include "outfit/OutfitAssignments.h"
#include "wig/WigAssignments.h"
#include "wig/WigManager.h"

namespace Tailor::Player
{
    namespace
    {
        constexpr std::uint32_t PluginID = 0x54414C52; // TALR
        constexpr std::uint32_t RecordID = 0x504C5952; // PLYR
        constexpr std::uint32_t Version = 1;
        constexpr std::uint32_t MaxRecordSize = 1024 * 1024;

        nlohmann::json WriteWig(const WigEntry& wig)
        {
            return {{"formId", wig.formId}, {"plugin", wig.plugin}, {"name", wig.name}};
        }
        WigEntry ReadWig(const nlohmann::json& json)
        {
            return {json.at("formId").get<RE::FormID>(), json.at("plugin").get<std::string>(), json.at("name").get<std::string>()};
        }
        void ResolveEquipment(EquipmentState& state, SKSE::SerializationInterface* api)
        {
            const auto resolve = [api](Instance& item) {
                RE::FormID form = 0, owner = 0;
                if (!api->ResolveFormID(item.form, form) || !api->ResolveFormID(item.owner, owner)) return false;
                item.form = form;
                item.owner = owner;
                return true;
            };
            std::erase_if(state.supplied, [&](Instance& item) { return !resolve(item); });
            std::erase_if(state.baseline.worn, [&](Instance& item) { return !resolve(item); });
            for (auto& hand : state.baseline.hands) {
                RE::FormID resolved = 0;
                if (hand) api->ResolveFormID(hand, resolved);
                hand = resolved;
            }
        }
    }

    void State::Register()
    {
        auto* api = SKSE::GetSerializationInterface();
        if (!api) return;
        api->SetUniqueID(PluginID);
        api->SetSaveCallback(Save);
        api->SetLoadCallback(Load);
        api->SetRevertCallback([](SKSE::SerializationInterface*) { Clear(); });
    }

    void State::Clear()
    {
        WigManager::GetSingleton().PreparePlayerForGameLoad();
        auto& outfits = OutfitAssignments::GetSingleton();
        auto& wigs = WigAssignments::GetSingleton();
        std::scoped_lock lock(outfits._mutex, wigs._mutex);
        outfits._assignments.erase(ReferenceID);
        wigs._assignments.erase(ReferenceID);
        wigs._situations.erase(ReferenceID);
        Equipment::GetSingleton().Clear();
    }

    void State::Save(SKSE::SerializationInterface* api)
    {
        try {
            auto& outfits = OutfitAssignments::GetSingleton();
            auto& wigs = WigAssignments::GetSingleton();
            std::scoped_lock lock(outfits._mutex, wigs._mutex);
            nlohmann::json data;
            data["equipment"] = Equipment::GetSingleton().GetState();
            if (const auto it = outfits._assignments.find(ReferenceID); it != outfits._assignments.end()) {
                const auto& a = it->second;
                data["outfit"] = {{"generic", a.outfitId}, {"armorType", OutfitArmorTypeName(a.adventuringArmorType)},
                    {"ids", {a.adventuringId, a.townId, a.homeId, a.sleepId}},
                    {"random", {a.adventuringRandom, a.townRandom, a.homeRandom, a.sleepRandom}}};
            }
            if (const auto it = wigs._assignments.find(ReferenceID); it != wigs._assignments.end()) {
                const auto& a = it->second;
                data["wig"] = {{"current", WriteWig(a.currentWig)}, {"added", a.itemAdded},
                    {"color", {a.hairColorR, a.hairColorG, a.hairColorB}}};
            }
            if (const auto it = wigs._situations.find(ReferenceID); it != wigs._situations.end()) {
                data["wigSituations"] = nlohmann::json::array();
                for (int slot = 1; slot <= 4; ++slot) data["wigSituations"].push_back(WriteWig(it->second.GetSlot(static_cast<OutfitSituation>(slot))));
            }
            const auto json = data.dump();
            if (json.size() > MaxRecordSize || !api->WriteRecord(RecordID, Version, json.data(), static_cast<std::uint32_t>(json.size()))) {
                logger::error("Player state: could not write SKSE cosave record");
            }
        } catch (const std::exception& error) {
            logger::error("Player state save failed: {}", error.what());
        }
    }

    void State::Load(SKSE::SerializationInterface* api)
    {
        Clear();
        std::uint32_t type = 0, version = 0, length = 0;
        while (api->GetNextRecordInfo(type, version, length)) {
            if (type != RecordID || version != Version || length > MaxRecordSize) continue;
            try {
                std::string text(length, '\0');
                if (api->ReadRecordData(text.data(), length) != length) throw std::runtime_error("truncated record");
                const auto json = nlohmann::json::parse(text);
                auto equipment = json.at("equipment").get<std::array<EquipmentState, 2>>();
                for (auto& state : equipment) ResolveEquipment(state, api);
                SituationalAssignment outfit;
                ActorWigState wig;
                WigSituationalAssignment situations;
                if (json.contains("outfit")) {
                    const auto& a = json.at("outfit");
                    outfit.outfitId = a.at("generic").get<int>();
                    outfit.adventuringArmorType = ParseOutfitArmorType(a.at("armorType").get<std::string>());
                    const auto ids = a.at("ids").get<std::array<int, 4>>();
                    const auto random = a.at("random").get<std::array<bool, 4>>();
                    for (int i = 0; i < 4; ++i) {
                        outfit.SetSlot(static_cast<OutfitSituation>(i + 1), ids[i]);
                        outfit.SetRandomFlag(static_cast<OutfitSituation>(i + 1), random[i]);
                    }
                }
                if (json.contains("wig")) {
                    const auto& a = json.at("wig");
                    wig.currentWig = ReadWig(a.at("current"));
                    wig.itemAdded = a.at("added").get<bool>();
                    const auto color = a.at("color").get<std::array<int, 3>>();
                    for (auto value : color) if (value < -1 || value > 255) throw std::runtime_error("invalid hair color");
                    wig.hairColorR = static_cast<int16_t>(color[0]);
                    wig.hairColorG = static_cast<int16_t>(color[1]);
                    wig.hairColorB = static_cast<int16_t>(color[2]);
                }
                if (json.contains("wigSituations")) {
                    const auto& list = json.at("wigSituations");
                    if (!list.is_array() || list.size() != 4) throw std::runtime_error("invalid wig situations");
                    for (int i = 0; i < 4; ++i) situations.SetSlot(static_cast<OutfitSituation>(i + 1), ReadWig(list.at(i)));
                }
                auto& outfits = OutfitAssignments::GetSingleton();
                auto& wigs = WigAssignments::GetSingleton();
                std::scoped_lock lock(outfits._mutex, wigs._mutex);
                if (outfit.HasSettings()) outfits._assignments[ReferenceID] = std::move(outfit);
                if (json.contains("wig")) wigs._assignments[ReferenceID] = std::move(wig);
                if (situations.HasAnySituation()) wigs._situations[ReferenceID] = std::move(situations);
                Equipment::GetSingleton().SetState(std::move(equipment));
                logger::info("Loaded character-specific Tailor state from SKSE cosave");
            } catch (const std::exception& error) {
                Clear();
                logger::error("Player state load failed: {}", error.what());
            }
        }
    }
}
