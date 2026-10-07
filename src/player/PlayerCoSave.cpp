#include "player/PlayerCoSave.h"
#include "api/ModOverrides.h"
#include "events/SituationHandler.h"
#include "events/SituationHelmets.h"
#include "outfit/OutfitAssignments.h"
#include "player/PlayerRecord.h"
#include "player/PlayerWardrobe.h"
#include "wig/WigAssignments.h"

namespace Tailor::Player
{
    namespace
    {
        constexpr std::uint32_t kUniqueID = 'TLOR';
        constexpr std::uint32_t kPlayerRecord = 'PLYR';
        constexpr std::uint32_t kPlayerRecordVersion = 1;
        // Hide Helmets: what Tailor took off, NPCs included, so it can go back on after a load.
        constexpr std::uint32_t kHelmetsRecord = 'HELM';
        constexpr std::uint32_t kHelmetsRecordVersion = 1;
        // Mod API overrides, NPCs included.
        constexpr std::uint32_t kOverridesRecord = 'OVRD';
        constexpr std::uint32_t kOverridesRecordVersion = 1;

        void Save(SKSE::SerializationInterface* serialization)
        {
            const PlayerRecord record{OutfitAssignments::GetSingleton().ExportPlayer(), PlayerWardrobe::GetSingleton().State(),
                WigAssignments::GetSingleton().ExportPlayer(), SituationHandler::GetSingleton()->ExportPlayerWakeUp()};
            const auto payload = ToJson(record).dump();
            if (!serialization->WriteRecord(kPlayerRecord, kPlayerRecordVersion, payload.data(), static_cast<std::uint32_t>(payload.size()))) {
                logger::error("Player co-save: could not write the player record");
            }
            const auto helmets = SituationHelmets::GetSingleton().Save();
            if (!serialization->WriteRecord(kHelmetsRecord, kHelmetsRecordVersion, helmets.data(), static_cast<std::uint32_t>(helmets.size()))) {
                logger::error("Co-save: could not write the Hide Helmets record");
            }
            const auto overrides = ModOverrides::GetSingleton().Save();
            if (!serialization->WriteRecord(kOverridesRecord, kOverridesRecordVersion, overrides.data(), static_cast<std::uint32_t>(overrides.size()))) {
                logger::error("Co-save: could not write the mod API overrides record");
            }
        }

        void Load(SKSE::SerializationInterface* serialization)
        {
            std::uint32_t type = 0, version = 0, length = 0;
            while (serialization->GetNextRecordInfo(type, version, length)) {
                if (type == kHelmetsRecord) {
                    if (version > kHelmetsRecordVersion) {
                        logger::warn("Co-save: Hide Helmets record version {} is newer than this build; skipped", version);
                        continue;
                    }
                    std::string payload(length, '\0');
                    if (serialization->ReadRecordData(payload.data(), length) != length) {
                        logger::error("Co-save: the Hide Helmets record is truncated");
                        continue;
                    }
                    SituationHelmets::GetSingleton().Load(payload, serialization);
                    continue;
                }
                if (type == kOverridesRecord) {
                    if (version > kOverridesRecordVersion) {
                        logger::warn("Co-save: mod API overrides record version {} is newer than this build; skipped", version);
                        continue;
                    }
                    std::string payload(length, '\0');
                    if (serialization->ReadRecordData(payload.data(), length) != length) {
                        logger::error("Co-save: the mod API overrides record is truncated");
                        continue;
                    }
                    ModOverrides::GetSingleton().Load(payload, serialization);
                    continue;
                }
                if (type != kPlayerRecord) continue;
                if (version > kPlayerRecordVersion) {
                    logger::warn("Player co-save: record version {} is newer than this build; skipped", version);
                    continue;
                }
                std::string payload(length, '\0');
                if (serialization->ReadRecordData(payload.data(), length) != length) {
                    logger::error("Player co-save: the player record is truncated");
                    continue;
                }
                try {
                    const auto record = FromJson(nlohmann::json::parse(payload));
                    OutfitAssignments::GetSingleton().ImportPlayer(record.outfits);
                    PlayerWardrobe::GetSingleton().SetState(record.wardrobe);
                    WigAssignments::GetSingleton().ImportPlayer(record.wigs);
                    SituationHandler::GetSingleton()->ImportPlayerWakeUp(record.wakeUp);
                    logger::info("Player co-save: loaded outfit {} with {} Tailor copies, wig '{}'",
                        record.wardrobe.wornOutfitId, record.wardrobe.tailorCopies.size(),
                        record.wigs.state ? record.wigs.state->currentWig.name : std::string("none"));
                } catch (const std::exception& e) {
                    logger::error("Player co-save: unreadable player record: {}", e.what());
                }
            }
        }

        // Before any load and on a new game: the previous save's player is gone.
        void Revert(SKSE::SerializationInterface*)
        {
            OutfitAssignments::GetSingleton().ImportPlayer(std::nullopt);
            PlayerWardrobe::GetSingleton().Clear();
            WigAssignments::GetSingleton().ImportPlayer({});
            SituationHandler::GetSingleton()->ImportPlayerWakeUp({});
            SituationHelmets::GetSingleton().Clear();
            ModOverrides::GetSingleton().ClearAll();
        }
    }

    void RegisterCoSave()
    {
        auto* serialization = SKSE::GetSerializationInterface();
        if (!serialization) {
            logger::error("Player co-save: SKSE serialization is unavailable; player outfits will not be saved");
            return;
        }
        serialization->SetUniqueID(kUniqueID);
        serialization->SetSaveCallback(Save);
        serialization->SetLoadCallback(Load);
        serialization->SetRevertCallback(Revert);
    }
}
