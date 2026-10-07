#include "player/PlayerRecord.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Tailor::Player
{
    namespace
    {
        // A form as plugin + local ID; null for a runtime form, which has no plugin.
        nlohmann::json EncodeForm(RE::FormID id)
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data || !id) return nullptr;
            const auto index = (id >> 24) & 0xFF;
            const RE::TESFile* file = nullptr;
            RE::FormID local = 0;
            if (index == 0xFE) {
                file = data->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((id & 0x00FFF000) >> 12));
                local = id & 0x00000FFF;
            } else if (index != 0xFF) {
                file = data->LookupLoadedModByIndex(static_cast<std::uint8_t>(index));
                local = id & 0x00FFFFFF;
            }
            if (!file) return nullptr;
            return {{"plugin", std::string(file->GetFilename())}, {"id", local}};
        }

        std::optional<RE::FormID> DecodeForm(const nlohmann::json& value)
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data || !value.is_object() || !value.contains("plugin") || !value.contains("id")) return std::nullopt;
            const auto id = value["id"].get<RE::FormID>();
            const auto plugin = value["plugin"].get<std::string>();
            auto* form = data->LookupForm(id, plugin);
            if (!form) {
                // Its plugin was removed, or no longer has the form, since the game was saved.
                logger::warn("Player co-save: dropping 0x{:06X} from '{}', which is no longer loaded", id, plugin);
                return std::nullopt;
            }
            return form->GetFormID();
        }

        nlohmann::json EncodeCopy(const CopyKey& copy)
        {
            auto entry = EncodeForm(copy.form);
            if (entry.is_null()) return nullptr;
            entry["owner"] = copy.owner;
            entry["unique"] = copy.unique;
            return entry;
        }

        std::optional<CopyKey> DecodeCopy(const nlohmann::json& value)
        {
            const auto form = DecodeForm(value);
            if (!form) return std::nullopt;
            return CopyKey{*form, value.value("owner", std::uint32_t{0}), value.value("unique", std::uint16_t{0})};
        }

        nlohmann::json EncodeCopies(const std::vector<CopyKey>& copies)
        {
            auto array = nlohmann::json::array();
            for (const auto& copy : copies) {
                if (auto entry = EncodeCopy(copy); !entry.is_null()) array.push_back(std::move(entry));
            }
            return array;
        }

        std::vector<CopyKey> DecodeCopies(const nlohmann::json& array)
        {
            std::vector<CopyKey> copies;
            if (!array.is_array()) return copies;
            for (const auto& entry : array) {
                if (auto copy = DecodeCopy(entry)) copies.push_back(*copy);
            }
            return copies;
        }

        nlohmann::json EncodeAssignment(const SituationalAssignment& a)
        {
            return {
                {"outfitId", a.outfitId}, {"adventuringId", a.adventuringId}, {"townId", a.townId},
                {"homeId", a.homeId}, {"sleepId", a.sleepId}, {"swimmingId", a.swimmingId}, {"warmId", a.warmId},
                {"adventuringRandom", a.adventuringRandom}, {"townRandom", a.townRandom},
                {"homeRandom", a.homeRandom}, {"sleepRandom", a.sleepRandom},
                {"swimmingRandom", a.swimmingRandom}, {"warmRandom", a.warmRandom},
                {"adventuringArmorType", OutfitArmorTypeName(a.adventuringArmorType)}};
        }

        SituationalAssignment DecodeAssignment(const nlohmann::json& json)
        {
            SituationalAssignment a;
            a.outfitId = json.value("outfitId", 0);
            a.adventuringId = json.value("adventuringId", 0);
            a.townId = json.value("townId", 0);
            a.homeId = json.value("homeId", 0);
            a.sleepId = json.value("sleepId", 0);
            a.swimmingId = json.value("swimmingId", 0);
            a.warmId = json.value("warmId", 0);
            a.adventuringRandom = json.value("adventuringRandom", false);
            a.townRandom = json.value("townRandom", false);
            a.homeRandom = json.value("homeRandom", false);
            a.sleepRandom = json.value("sleepRandom", false);
            a.swimmingRandom = json.value("swimmingRandom", false);
            a.warmRandom = json.value("warmRandom", false);
            a.adventuringArmorType = json.contains("adventuringArmorType") && json["adventuringArmorType"].is_string()
                ? ParseOutfitArmorType(json["adventuringArmorType"].get<std::string>()) : OutfitArmorType::Any;
            return a;
        }

        // A wig as its plugin and local ID, the way the Wiggy files keep it.
        nlohmann::json EncodeWig(const WigEntry& wig)
        {
            if (wig.formId == 0 || wig.plugin.empty()) return nullptr;
            return {{"plugin", wig.plugin}, {"id", wig.formId}, {"name", wig.name}};
        }

        std::optional<WigEntry> DecodeWig(const nlohmann::json& value)
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data || !value.is_object() || !value.contains("plugin") || !value.contains("id")) return std::nullopt;
            WigEntry wig{value["id"].get<RE::FormID>(), value["plugin"].get<std::string>(), value.value("name", std::string{})};
            // A choice, not an item: as NPC rows do, it stays dormant and applies again when the plugin returns.
            if (!data->LookupForm(wig.formId, wig.plugin)) {
                logger::warn("Player co-save: keeping wig '{}' (0x{:06X} from '{}') dormant; it is not loaded",
                    wig.name, wig.formId, wig.plugin);
            }
            return wig;
        }

        constexpr std::array<std::pair<OutfitSituation, const char*>, 4> kWigSituations{{
            {OutfitSituation::Adventuring, "adventuring"}, {OutfitSituation::Town, "town"},
            {OutfitSituation::Home, "home"}, {OutfitSituation::Sleep, "sleep"}}};

        nlohmann::json EncodeWigs(const PlayerWigRow& row)
        {
            auto json = nlohmann::json::object();
            if (row.state) {
                if (auto current = EncodeWig(row.state->currentWig); !current.is_null()) json["current"] = std::move(current);
                // Always written, null when none, so a load tells it from a save made before it was kept.
                json["assigned"] = EncodeWig(row.state->assignedWig);
                if (row.state->HasHairColor()) {
                    json["hairColor"] = {row.state->hairColorR, row.state->hairColorG, row.state->hairColorB};
                }
            }
            if (row.situations) {
                auto situations = nlohmann::json::object();
                for (const auto& [situation, key] : kWigSituations) {
                    if (auto wig = EncodeWig(row.situations->GetSlot(situation)); !wig.is_null()) situations[key] = std::move(wig);
                }
                if (!situations.empty()) json["situations"] = std::move(situations);
            }
            return json;
        }

        PlayerWigRow DecodeWigs(const nlohmann::json& json)
        {
            PlayerWigRow row;
            if (!json.is_object()) return row;
            ActorWigState state;
            if (json.contains("current")) {
                if (auto wig = DecodeWig(json["current"])) state.currentWig = *wig;
            }
            if (json.contains("assigned")) {
                if (auto wig = DecodeWig(json["assigned"])) state.assignedWig = *wig;
            }
            if (const auto color = json.value("hairColor", nlohmann::json::array()); color.is_array() && color.size() == 3) {
                state.hairColorR = color[0].get<std::int16_t>();
                state.hairColorG = color[1].get<std::int16_t>();
                state.hairColorB = color[2].get<std::int16_t>();
            }
            WigSituationalAssignment situations;
            const auto slots = json.value("situations", nlohmann::json::object());
            for (const auto& [situation, key] : kWigSituations) {
                if (!slots.contains(key)) continue;
                if (auto wig = DecodeWig(slots[key])) situations.SetSlot(situation, *wig);
            }
            if (situations.HasAnySituation()) row.situations = situations;
            // Saves from before the assigned wig was kept: a worn wig that isn't one of the player's
            // situation wigs is taken as the one set in the Hair Dresser.
            if (!json.contains("assigned") && state.currentWig.formId != 0) {
                if (!situations.Holds(state.currentWig)) state.assignedWig = state.currentWig;
            }
            if (!state.IsEmpty()) row.state = state;
            return row;
        }
    }

    nlohmann::json ToJson(const PlayerRecord& record)
    {
        nlohmann::json json{{"version", 1}};
        if (record.outfits) json["outfits"] = EncodeAssignment(*record.outfits);
        auto displaced = nlohmann::json::array();
        for (const auto& item : record.wardrobe.displaced) {
            auto entry = EncodeCopy(item.copy);
            if (entry.is_null()) continue;
            entry["hand"] = item.hand == Hand::Right ? "right" : "left";
            displaced.push_back(std::move(entry));
        }
        auto pieces = nlohmann::json::array();
        for (const auto& piece : record.wardrobe.wornOutfitPieces) {
            pieces.push_back(nlohmann::json{{"plugin", piece.plugin}, {"id", piece.id}});
        }
        json["wardrobe"] = {
            {"ownGearRecorded", record.wardrobe.ownGearRecorded},
            {"ownGear", EncodeCopies(record.wardrobe.ownGear)},
            {"tailorCopies", EncodeCopies(record.wardrobe.tailorCopies)},
            {"displaced", displaced},
            {"wornOutfitId", record.wardrobe.wornOutfitId},
            {"keptWig", EncodeForm(record.wardrobe.keptWig)},
            {"wigCopies", EncodeCopies(record.wardrobe.wigCopies)},
            {"ownGearNote", EncodeCopies(record.wardrobe.ownGearNote)},
            {"wornOutfitPieces", pieces}};
        json["wigs"] = EncodeWigs(record.wigs);
        if (record.wakeUp.active) json["wakeUp"] = {record.wakeUp.x, record.wakeUp.y, record.wakeUp.z};
        return json;
    }

    PlayerRecord FromJson(const nlohmann::json& json)
    {
        PlayerRecord record;
        if (!json.is_object()) return record;
        if (json.contains("outfits") && json["outfits"].is_object()) {
            auto outfits = DecodeAssignment(json["outfits"]);
            if (outfits.HasSettings()) record.outfits = outfits;
        }
        const auto wardrobe = json.value("wardrobe", nlohmann::json::object());
        record.wardrobe.ownGearRecorded = wardrobe.value("ownGearRecorded", false);
        record.wardrobe.ownGear = DecodeCopies(wardrobe.value("ownGear", nlohmann::json::array()));
        record.wardrobe.tailorCopies = DecodeCopies(wardrobe.value("tailorCopies", nlohmann::json::array()));
        for (const auto& entry : wardrobe.value("displaced", nlohmann::json::array())) {
            if (auto copy = DecodeCopy(entry)) {
                record.wardrobe.displaced.push_back({*copy, entry.value("hand", std::string("left")) == "right" ? Hand::Right : Hand::Left});
            }
        }
        record.wardrobe.wornOutfitId = wardrobe.value("wornOutfitId", 0);
        if (const auto kept = DecodeForm(wardrobe.value("keptWig", nlohmann::json()))) record.wardrobe.keptWig = *kept;
        record.wardrobe.wigCopies = DecodeCopies(wardrobe.value("wigCopies", nlohmann::json::array()));
        record.wardrobe.ownGearNote = DecodeCopies(wardrobe.value("ownGearNote", nlohmann::json::array()));
        // Kept as written, loaded or not: the load compares them with the outfit's own list.
        for (const auto& piece : wardrobe.value("wornOutfitPieces", nlohmann::json::array())) {
            if (piece.is_object() && piece.contains("plugin") && piece.contains("id")) {
                record.wardrobe.wornOutfitPieces.push_back({piece["plugin"].get<std::string>(), piece["id"].get<std::uint32_t>()});
            }
        }
        record.wigs = DecodeWigs(json.value("wigs", nlohmann::json::object()));
        // Three numbers, or no note at all.
        if (const auto woke = json.value("wakeUp", nlohmann::json()); woke.is_array() && woke.size() == 3 &&
            std::all_of(woke.begin(), woke.end(), [](const nlohmann::json& n) { return n.is_number(); })) {
            record.wakeUp.Woke(woke[0].get<float>(), woke[1].get<float>(), woke[2].get<float>());
        }
        return record;
    }
}
