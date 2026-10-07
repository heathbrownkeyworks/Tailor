#include "outfit/OutfitManager.h"
#include "api/ModOverrides.h"
#include "outfit/ArmorEquipmentStatus.h"
#include "events/SituationHandler.h"
#include "events/SituationHelmets.h"
#include "player/PlayerModel.h"
#include "player/PlayerEquipmentWarning.h"
#include "player/PlayerIds.h"
#include "player/PlayerTarget.h"
#include "player/PlayerWardrobe.h"
#include "ui/TailorUI.h"
#include "wig/WigManager.h"

#include <chrono>
#include <thread>

// OutfitManager's player half. PlayerWardrobe dresses the player through the
// inventory; the NPC code in OutfitManager.cpp only branches here.

using Tailor::Player::RefreshPlayerModel;

namespace
{
    std::vector<RE::TESObjectARMO*> ResolveArmor(const std::vector<ArmorItem>& items)
    {
        std::vector<RE::TESObjectARMO*> armor;
        for (const auto& item : items) {
            if (auto* form = item.Resolve()) {
                armor.push_back(form);
            } else {
                logger::warn("Player outfit: skipping '{}' (0x{:06X} from '{}'), no such armor is loaded",
                    item.name, item.formId, item.plugin);
            }
        }
        return armor;
    }

    std::vector<RE::TESObjectARMO*> ResolveArmor(const std::vector<RE::TESForm*>& items)
    {
        std::vector<RE::TESObjectARMO*> armor;
        for (auto* item : items) {
            if (auto* form = item ? item->As<RE::TESObjectARMO>() : nullptr) armor.push_back(form);
        }
        return armor;
    }

    // The outfit's pieces as saved, never resolved: a load compares them with the outfit again.
    std::vector<Tailor::Player::PieceKey> PiecesOf(const CustomOutfit& outfit)
    {
        std::vector<Tailor::Player::PieceKey> pieces;
        for (const auto& item : outfit.items) pieces.push_back({item.plugin, item.formId});
        return pieces;
    }

    // Why CanDress turns the player down, for the log.
    const char* WhyNotDressable(RE::Actor* player)
    {
        if (!player) return "there is no player";
        if (player->IsDead()) return "the player is dead";
        if (!player->Is3DLoaded()) return "the player is not loaded";
        if (Tailor::Player::InBeastForm(player)) return "the player is in beast form";
        return "the player can't be dressed";
    }

    // A piece with no model that shows on the player's body would only hide it, as a
    // female-only cuirass does on a male character: such pieces stay off.
    std::vector<RE::TESObjectARMO*> FittingPieces(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& armor,
        std::vector<RE::TESObjectARMO*>& leftOff)
    {
        std::vector<RE::TESObjectARMO*> fitting;
        for (auto* piece : armor) {
            if (Tailor::Outfits::FitsActorBody(player, piece)) {
                fitting.push_back(piece);
            } else {
                leftOff.push_back(piece);
                logger::warn("Player outfit: leaving off '{}' ({:08X}), it has no model for the player's race and sex",
                    piece->GetName(), piece->GetFormID());
            }
        }
        return fitting;
    }

    // "A", "A and B", "A, B and C" or "A, B and 3 more": short enough for a toast.
    std::string NamePieces(const std::vector<RE::TESObjectARMO*>& pieces)
    {
        std::vector<std::string> names;
        for (auto* piece : pieces) names.emplace_back(piece->GetName());
        return Tailor::Player::NameList(names);
    }

    // Tagging the outfit helps only when the pieces are made for the other sex. When
    // no model shows on the player's race at all, a tag would be the wrong advice.
    std::string TagAdvice(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& leftOff, std::string_view subject)
    {
        auto* npc = player->GetActorBase();
        const auto other = npc && npc->GetSex() == RE::SEX::kFemale ? RE::SEX::kMale : RE::SEX::kFemale;
        const bool otherSex = std::ranges::all_of(leftOff, [&](RE::TESObjectARMO* piece) {
            return Tailor::Outfits::FitsBody(piece, player->GetRace(), other);
        });
        if (!otherSex) return {};
        return std::format(" If {} is made for one sex, tag it {} in Manage Outfits.", subject,
            other == RE::SEX::kMale ? "Male" : "Female");
    }
}

bool OutfitManager::TargetPlayer()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!Tailor::Player::CanDress(player)) return false;
    _currentTarget = player->GetHandle();
    _playerTarget = true;
    return true;
}

bool OutfitManager::TargetSessionNpc()
{
    auto* npc = GetSessionNpc();
    if (!npc) return false;
    _currentTarget = npc->GetHandle();
    _playerTarget = false;
    return true;
}

bool OutfitManager::IsPlayerTarget() const
{
    auto* target = GetTarget();
    return target && target->IsPlayerRef();
}

RE::Actor* OutfitManager::GetSessionNpc() const
{
    auto actor = _sessionNpc.get();
    auto* npc = actor.get();
    return npc && IsValidTarget(npc) && npc->Is3DLoaded() ? npc : nullptr;
}

bool OutfitManager::ApplyPlayerOutfit(RE::Actor* player, const CustomOutfit& outfit)
{
    const auto loaded = ResolveArmor(outfit.items);
    // An outfit whose plugin is gone would strip the player bare: leave them as they are.
    if (loaded.empty()) {
        logger::warn("ApplyCustomOutfit: no piece of '{}' is loaded; the player keeps what they wear", outfit.name);
        return false;
    }
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    std::vector<RE::TESObjectARMO*> leftOff;
    const auto items = FittingPieces(player, loaded, leftOff);
    if (items.empty()) {
        TailorUI::GetSingleton().ShowEquipmentWarning(player, std::format(
            "None of {}'s pieces have a model for your character, so it stays off.{}",
            outfit.name, TagAdvice(player, leftOff, "it")));
        // A preview goes on showing the look before this outfit, and Set refuses to save it.
        return wardrobe.IsPreviewing();
    }
    // Tailor's wig follows the headwear rules: it comes off before a hood in the outfit goes on.
    if (!WigManager::GetSingleton().PrepareForOutfitChange(player, std::vector<RE::TESForm*>(items.begin(), items.end()))) {
        logger::warn("ApplyCustomOutfit: the player's headgear is still being put back; '{}' waits", outfit.name);
        return false;
    }
    // Dressing Room choices are previews until Set. A piece the game refuses is
    // named by the audit; retrying would not change the answer.
    const bool complete = wardrobe.IsPreviewing() ? wardrobe.PreviewItems(player, items)
                                                  : wardrobe.Dress(player, items, outfit.id, PiecesOf(outfit));
    RefreshPlayerModel(player);
    NotifyOutfitChanged(player);
    QueuePlayerEquipmentAudit(player, items);
    if (!leftOff.empty()) {
        TailorUI::GetSingleton().ShowEquipmentWarning(player, std::format(
            "Left off {}: no model for your character.{}",
            NamePieces(leftOff), TagAdvice(player, leftOff, outfit.name)));
    }
    logger::info("Dressed the player in '{}' ({} pieces, complete={})", outfit.name, items.size(), complete);
    return true;
}

bool OutfitManager::RestorePlayerOwnGear(RE::Actor* player)
{
    // Own Gear is not being checked; drop any audit still pending from what wore before.
    QueuePlayerEquipmentAudit(player, {});
    if (!Tailor::Player::PlayerWardrobe::GetSingleton().RestoreOwnGear(player)) {
        logger::warn("RestoreOriginalOutfit: some of the player's own gear could not be re-equipped");
    }
    RefreshPlayerModel(player);
    return true;
}

bool OutfitManager::CancelPlayerPreview(RE::Actor* player)
{
    // The preview is coming off, not being kept: no check on it is relevant anymore.
    QueuePlayerEquipmentAudit(player, {});
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    // Nothing to put back (a load has already dropped the preview): leave the model alone.
    if (!wardrobe.IsPreviewing()) return true;
    const bool restored = wardrobe.EndPreview(player, false);
    RefreshPlayerModel(player);
    return restored;
}

bool OutfitManager::ResetPlayerOutfit(RE::Actor* player)
{
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    if (wardrobe.IsPreviewing()) wardrobe.EndPreview(player, false);
    RestorePlayerOwnGear(player);
    _cycleState.reset();
    SituationHandler::GetSingleton()->ClearOutfitOverrides(player->GetFormID());
    // Resetting the player in Tailor ends a mod's override too.
    ModOverrides::GetSingleton().Clear(Tailor::Player::kPlayerRef);
    NotifyOutfitChanged(player);
    auto& assignments = OutfitAssignments::GetSingleton();
    if (assignments.HasAssignment(player->GetFormID()) || assignments.HasAnySituation(player->GetFormID())) {
        assignments.Unassign(player->GetFormID());
    }
    // Reset doesn't reach ForceApplyForSituation: with no wig situations left either, their state ends here.
    auto* situations = SituationHandler::GetSingleton();
    if (!situations->PlayerHasSituations()) situations->ForgetPlayerSituationState();
    logger::info("Reset the player's outfit to Own Gear");
    return true;
}

bool OutfitManager::ConfirmPlayerCycle(RE::Actor* player, OutfitSituation situation, int outfitId)
{
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    auto& assignments = OutfitAssignments::GetSingleton();
    const auto* outfit = OutfitStore::GetSingleton().GetOutfitById(outfitId);
    // Set never saves an outfit the player would wear nothing of: the look on screen is
    // still the one before it, which must not be recorded under this outfit's name.
    if (outfit) {
        std::vector<RE::TESObjectARMO*> leftOff;
        const auto loaded = ResolveArmor(outfit->items);
        if (loaded.empty() || FittingPieces(player, loaded, leftOff).empty()) {
            CancelPlayerPreview(player);
            _cycleState.reset();
            NotifyOutfitChanged(player);
            TailorUI::GetSingleton().ShowEquipmentWarning(player, std::format(
                "{} was not set: {}.", outfit->name,
                loaded.empty() ? "none of its pieces are loaded" : "none of its pieces have a model for your character"));
            logger::warn("ConfirmCycle: the player can wear no piece of '{}', so it was not set", outfit->name);
            return false;
        }
    }
    if (static_cast<int>(situation) > 0) {
        // A situation's outfit is worn when its situation comes, as for NPCs.
        CancelPlayerPreview(player);
        assignments.AssignSituation(player->GetFormID(), situation, outfitId);
    } else {
        wardrobe.EndPreview(player, true, outfitId, outfit ? PiecesOf(*outfit) : std::vector<Tailor::Player::PieceKey>{});
        assignments.Assign(player->GetFormID(), outfitId);
    }
    // An outfit the player confirms in Tailor ends a mod's override. When it was all that managed their outfit,
    // their situation state ends now, as when the last situation is cleared.
    ModOverrides::GetSingleton().Clear(Tailor::Player::kPlayerRef);
    if (!SituationHandler::GetSingleton()->PlayerHasSituations()) SituationHandler::GetSingleton()->ForgetPlayerSituationState();
    _cycleState.reset();
    NotifyOutfitChanged(player);
    if (assignments.HasAnySituation(player->GetFormID())) {
        SituationHandler::GetSingleton()->ForceApplyForSituation(player);
    }
    logger::info("ConfirmCycle: the player keeps outfit {} (situation {})", outfitId, static_cast<int>(situation));
    return true;
}

void OutfitManager::BeginPlayerCreateOutfit(RE::Actor* player)
{
    if (!Tailor::Player::PlayerWardrobe::GetSingleton().BeginPreview(player)) {
        logger::warn("BeginCreateOutfit: the player's wardrobe is already previewing");
        return;
    }
    _createSessionActor = player->GetHandle();
    _createSessionActive = true;
    _createSessionEnding = false;
    ++_createSessionGeneration;
    _editOutfit.desiredItems.clear();
    logger::info("BeginCreateOutfit: ready for the player");
}

bool OutfitManager::PreviewPlayerItems(RE::Actor* player, const std::vector<RE::TESForm*>& items)
{
    const auto loaded = ResolveArmor(items);
    std::vector<RE::TESObjectARMO*> leftOff;
    const auto armor = FittingPieces(player, loaded, leftOff);
    // Only pieces the player's body can't show: the preview stays as it was.
    if (armor.empty() && !loaded.empty()) {
        TailorUI::GetSingleton().ShowEquipmentWarning(player, std::format(
            "{} {} no model for your character, so the preview is unchanged.{}",
            NamePieces(leftOff), leftOff.size() == 1 ? "has" : "have", TagAdvice(player, leftOff, "the outfit")));
        return true;
    }
    if (!WigManager::GetSingleton().PrepareForOutfitChange(player, std::vector<RE::TESForm*>(armor.begin(), armor.end()))) {
        logger::warn("Create/Edit: the player's headgear is still being put back; the preview waits");
        return false;
    }
    const bool complete = Tailor::Player::PlayerWardrobe::GetSingleton().PreviewItems(player, armor);
    RefreshPlayerModel(player);
    NotifyOutfitChanged(player);
    QueuePlayerEquipmentAudit(player, armor);
    if (!leftOff.empty()) {
        TailorUI::GetSingleton().ShowEquipmentWarning(player, std::format(
            "Left off {}: no model for your character.{}",
            NamePieces(leftOff), TagAdvice(player, leftOff, "the outfit")));
    }
    logger::info("Create/Edit: the player previews {} pieces (complete={})", armor.size(), complete);
    return true;
}

void OutfitManager::KeepPlayerLookForUnloadedOutfit(RE::Actor* player)
{
    // As Set refuses such an outfit: the look on screen stays, and the player learns why.
    TailorUI::GetSingleton().ShowEquipmentWarning(player, "None of this outfit's pieces are loaded, so the preview is unchanged.");
    logger::warn("Create/Edit: no piece of the outfit is loaded; the player's preview is unchanged");
}

void OutfitManager::EndPlayerCreateOutfit(RE::Actor* player)
{
    CancelPlayerPreview(player);
    _editOutfit.desiredItems.clear();
    _createSessionActor = RE::ActorHandle{};
    _createSessionActive = false;
    _createSessionEnding = false;
    NotifyOutfitChanged(player);
    // The preview already put the exact look back, so only an outfit that no longer
    // applies (for example its sex changed while the editor was open) is re-dressed.
    ReconcilePlayer();
    logger::info("EndCreateOutfit: the player's preview was put back");
}

void OutfitManager::RedressPlayer()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto& wardrobe = Tailor::Player::PlayerWardrobe::GetSingleton();
    if (!Tailor::Player::CanDress(player)) {
        logger::info("RedressPlayer: skipped because {}", WhyNotDressable(player));
        return;
    }
    if (wardrobe.IsPreviewing()) {
        logger::info("RedressPlayer: skipped because a preview is open");
        return;
    }
    // With situations, the situation's outfit is the one put on again.
    if (SituationHandler::GetSingleton()->PlayerHasSituations()) {
        SituationHandler::GetSingleton()->ReconcilePlayerSituation(true);
        return;
    }
    const int outfitId = OutfitAssignments::GetSingleton().GetOutfitId(player->GetFormID());
    const auto* outfit = outfitId > 0 ? OutfitStore::GetSingleton().GetOutfitById(outfitId) : nullptr;
    // An assigned outfit that stops fitting is skipped, never removed.
    if (outfit && OutfitFits(outfit->sex, GetNpcSex(player))) {
        ApplyCustomOutfit(player, *outfit);
    } else if (wardrobe.State().ownGearRecorded) {
        RestorePlayerOwnGear(player);
        NotifyOutfitChanged(player);
    }
}

void OutfitManager::ReconcilePlayerAfterLoad()
{
    // A load can come before the player's 3D is ready: the poll then runs the load's reconcile,
    // outfit and wig, once the player can be dressed.
    if (!Tailor::Player::CanDress(RE::PlayerCharacter::GetSingleton())) {
        SituationHandler::GetSingleton()->ReconcilePlayerWhenDressable();
        return;
    }
    ReconcilePlayer();
}

void OutfitManager::ReconcilePlayer()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    // With situations, the situation decides the outfit. It goes first: a player who can't be
    // dressed yet, as at a load before their 3D is ready, leaves the change pending for the poll.
    if (SituationHandler::GetSingleton()->PlayerHasSituations()) {
        SituationHandler::GetSingleton()->ReconcilePlayerSituation(false);
        return;
    }
    if (!Tailor::Player::CanDress(player)) {
        logger::info("ReconcilePlayer: skipped because {}", WhyNotDressable(player));
        return;
    }
    const int outfitId = OutfitAssignments::GetSingleton().GetOutfitId(player->GetFormID());
    const auto* outfit = outfitId > 0 ? OutfitStore::GetSingleton().GetOutfitById(outfitId) : nullptr;
    const int wanted = outfit && OutfitFits(outfit->sex, GetNpcSex(player)) ? outfitId : 0;
    // The save already holds what the player wore; only a changed outfit is redressed, or the same
    // one when its pieces were edited since, in another save.
    if (wanted != Tailor::Player::PlayerWardrobe::GetSingleton().WornOutfitId() || PlayerOutfitPiecesChanged(wanted)) RedressPlayer();
}

bool OutfitManager::PlayerOutfitPiecesChanged(int outfitId) const
{
    const auto state = Tailor::Player::PlayerWardrobe::GetSingleton().State();
    const auto* outfit = outfitId > 0 && state.wornOutfitId == outfitId ? OutfitStore::GetSingleton().GetOutfitById(outfitId) : nullptr;
    if (!outfit || !Tailor::Player::PiecesChanged(state.wornOutfitPieces, PiecesOf(*outfit))) return false;
    logger::info("The player wears '{}', whose pieces changed since Tailor dressed them in it", outfit->name);
    return true;
}

void OutfitManager::QueuePlayerEquipmentAudit(RE::Actor* player, const std::vector<RE::TESObjectARMO*>& items) const
{
    std::lock_guard lock(_mutex);
    const auto actorId = player->GetFormID();
    const auto token = ++_nextEquipmentAudit;
    // A new look, or the old one put back, supersedes any check still pending.
    _equipmentAudits.erase(actorId);
    std::vector<RE::FormID> expected;
    for (auto* item : items) {
        if (item) expected.push_back(item->GetFormID());
    }
    if (expected.empty()) return;
    _equipmentAudits[actorId] = token;
    // What this dressing took off: a piece that is on again when the check runs would not come off.
    auto takenOff = Tailor::Player::PlayerWardrobe::GetSingleton().TakenOff();
    std::thread([this, actorId, token, expected = std::move(expected), takenOff = std::move(takenOff)]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        SKSE::GetTaskInterface()->AddTask([this, actorId, token, expected, takenOff]() {
            std::lock_guard auditLock(_mutex);
            const auto it = _equipmentAudits.find(actorId);
            if (it == _equipmentAudits.end() || it->second != token) return;
            _equipmentAudits.erase(it);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded() || player->IsDead()) return;
            if (WigManager::GetSingleton().IsWigScreen(player)) return;  // intentionally displaced equipment
            // Helmets Hide Helmets has off are off on purpose.
            const auto helmetsOff = SituationHelmets::GetSingleton().TakenPieces(player);
            std::vector<std::string> notShown, stayedOn;
            for (const auto id : expected) {
                auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
                if (armor && std::ranges::find(helmetsOff, armor) != helmetsOff.end()) continue;
                const auto status = Tailor::Outfits::InspectArmorEquipment(player, armor);
                if (status.worn && status.compatibleModel && status.attached && status.graphVisible) continue;
                notShown.emplace_back(armor ? armor->GetName() : "an unloaded piece");
                logger::warn("Player equipment: armor={:08X} name='{}' worn={} compatibleModel={} attached={} graphVisible={}",
                    id, armor ? armor->GetName() : "unresolved", status.worn, status.compatibleModel,
                    status.attached, status.graphVisible);
            }
            for (const auto form : Tailor::Player::PlayerWardrobe::GetSingleton().WornForms(player, takenOff)) {
                auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(form);
                stayedOn.emplace_back(armor ? armor->GetName() : "an unloaded piece");
                logger::warn("Player equipment: armor={:08X} name='{}' is on again after the dressing took it off",
                    form, armor ? armor->GetName() : "unresolved");
            }
            logger::info("Player equipment audit: {} requested pieces, {} incomplete, {} back on after settling",
                expected.size(), notShown.size(), stayedOn.size());
            if (const auto warning = Tailor::Player::EquipmentWarning(notShown, stayedOn); !warning.empty()) {
                TailorUI::GetSingleton().ShowEquipmentWarning(player, warning);
            }
        });
    }).detach();
}
