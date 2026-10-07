#include "events/SituationHelmets.h"
#include "events/HideSettingsPolicy.h"
#include "wig/WigAssignments.h"
#include "wig/WigLibrary.h"
#include "wig/WigManager.h"

#include <algorithm>
#include <optional>

SituationHelmets& SituationHelmets::GetSingleton()
{
    static SituationHelmets singleton;
    return singleton;
}

void SituationHelmets::Update(RE::Actor* actor, bool hidden, int look)
{
    if (!actor) return;
    const auto actorId = actor->GetFormID();
    if (hidden) {
        // Wigs are never helmets: the person's own Tailor wig first, then the library's. Asked only for a
        // piece that passes the slot rule, since the poll asks every 250 ms.
        std::optional<RE::TESObjectARMO*> ownWig;
        const auto isWig = [&](RE::TESObjectARMO* armor) {
            if (!ownWig) {
                const auto state = WigAssignments::GetSingleton().GetState(actorId);
                ownWig = state ? state->currentWig.Resolve() : nullptr;
            }
            return armor == *ownWig || IsLibraryWig(armor->GetFormID());
        };
        const auto taken = Tailor::Wigs::TakeOffHeadwear(actor, [&](RE::TESObjectARMO* armor) {
            const auto mask = armor->GetSlotMask().underlying();
            return Tailor::Situations::HelmetToTakeOff(mask, false) && Tailor::Situations::HelmetToTakeOff(mask, isWig(armor));
        });
        std::size_t added = 0, forgotten = 0;
        {
            std::scoped_lock lock(_mutex);
            // Copies from another look came off an outfit that is gone: they never go back on.
            if (const auto it = _taken.find(actorId); it != _taken.end() && it->second.look != look) {
                forgotten = it->second.copies.size();
                _taken.erase(it);
            }
            if (!taken.empty()) {
                auto& ledger = _taken[actorId];
                ledger.look = look;
                for (const auto& copy : taken) {
                    if (std::ranges::find(ledger.copies, copy) != ledger.copies.end()) continue;
                    ledger.copies.push_back(copy);
                    ++added;
                }
            }
        }
        if (forgotten) logger::info("Hide Helmets: forgot {} helmet(s) or hood(s) of {:08X} from a look no longer worn", forgotten, actorId);
        // Only copies new to the ledger: one the game puts back on is taken off again without a word.
        if (added) logger::info("Hide Helmets: took {} helmet(s) or hood(s) off {:08X}", added, actorId);
        if (!taken.empty()) WigManager::GetSingleton().ReEquipWigAfterOutfitChange(actor);  // the wig the helmet hid
        return;
    }

    Taken taken;
    {
        std::scoped_lock lock(_mutex);
        const auto it = _taken.find(actorId);
        if (it == _taken.end()) return;
        taken = it->second;
    }
    // Only a copy that is there and off, from the look still worn, goes back on; the rest are forgotten.
    std::vector<Tailor::Wigs::HeadwearCopy> back;
    std::vector<RE::TESForm*> pieces;
    std::size_t forgotten = 0;
    for (const auto& copy : taken.copies) {
        if (Tailor::Situations::DecideCopy(taken.look == look, Tailor::Wigs::HeadwearCopyState(actor, copy)) ==
            Tailor::Situations::CopyFate::Forget) {
            ++forgotten;
            continue;
        }
        back.push_back(copy);
        if (auto* armor = RE::TESForm::LookupByID(copy.form)) pieces.push_back(armor);
    }
    // The wig steps aside for the helmet, as it does for an outfit's; with nothing going back it stays.
    if (!back.empty() && !WigManager::GetSingleton().PrepareForOutfitChange(actor, pieces)) return;
    std::vector<Tailor::Wigs::HeadwearCopy> failed;
    std::size_t worn = 0;
    for (const auto& copy : back) {
        switch (Tailor::Wigs::PutHeadwearBack(actor, copy)) {
        case Tailor::Wigs::PutBack::Worn: ++worn; break;
        case Tailor::Wigs::PutBack::Gone: ++forgotten; break;
        case Tailor::Wigs::PutBack::Failed: failed.push_back(copy); break;
        }
    }
    if (worn) logger::info("Hide Helmets: put {} helmet(s) or hood(s) back on {:08X}", worn, actorId);
    if (forgotten) {
        logger::info("Hide Helmets: forgot {} helmet(s) or hood(s) of {:08X}: gone, worn again, or from a look no longer worn",
            forgotten, actorId);
    }
    std::scoped_lock lock(_mutex);
    if (failed.empty()) _taken.erase(actorId);
    else _taken[actorId] = {taken.look, std::move(failed)};
}

bool SituationHelmets::IsLibraryWig(RE::FormID form)
{
    const auto revision = WigLibrary::GetSingleton().Revision();
    {
        std::scoped_lock lock(_mutex);
        if (_wigRevision == revision) return _libraryWigs.contains(form);
    }
    // Resolving every wig scans the plugin list: done once per change of the library, outside the lock.
    std::unordered_set<RE::FormID> wigs;
    for (auto* armor : WigLibrary::GetSingleton().ResolvedWigs()) wigs.insert(armor->GetFormID());
    std::scoped_lock lock(_mutex);
    _libraryWigs = std::move(wigs);
    _wigRevision = revision;
    return _libraryWigs.contains(form);
}

std::vector<RE::FormID> SituationHelmets::Actors() const
{
    std::scoped_lock lock(_mutex);
    std::vector<RE::FormID> actors;
    for (const auto& [actorId, taken] : _taken) actors.push_back(actorId);
    return actors;
}

std::vector<RE::TESObjectARMO*> SituationHelmets::TakenPieces(RE::Actor* actor) const
{
    std::vector<RE::TESObjectARMO*> pieces;
    if (!actor) return pieces;
    std::vector<Tailor::Wigs::HeadwearCopy> copies;
    {
        std::scoped_lock lock(_mutex);
        const auto it = _taken.find(actor->GetFormID());
        if (it == _taken.end()) return pieces;
        copies = it->second.copies;
    }
    // Only copies still there and off: one that is gone or worn again no longer lets its armor pass as not headgear.
    for (const auto& copy : copies) {
        if (Tailor::Wigs::HeadwearCopyState(actor, copy) != Tailor::Situations::CopyState::Off) continue;
        if (auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(copy.form)) pieces.push_back(armor);
    }
    return pieces;
}

std::vector<Tailor::Wigs::HeadwearCopy> SituationHelmets::Copies(RE::FormID actorId) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _taken.find(actorId);
    return it != _taken.end() ? it->second.copies : std::vector<Tailor::Wigs::HeadwearCopy>{};
}

void SituationHelmets::Forget(RE::FormID actorId)
{
    std::scoped_lock lock(_mutex);
    if (_taken.erase(actorId)) logger::info("Hide Helmets: forgot what it took off {:08X}, who is dead", actorId);
}

std::string SituationHelmets::Save() const
{
    std::scoped_lock lock(_mutex);
    std::vector<Tailor::Situations::TakenHelmet> helmets;
    for (const auto& [actorId, taken] : _taken) {
        for (const auto& copy : taken.copies) helmets.push_back({actorId, copy.form, copy.owner, copy.unique, taken.look});
    }
    return Tailor::Situations::EncodeTakenHelmets(helmets);
}

void SituationHelmets::Load(std::string_view payload, const SKSE::SerializationInterface* serialization)
{
    std::size_t skipped = 0;
    const auto helmets = Tailor::Situations::DecodeTakenHelmets(payload, skipped);
    std::scoped_lock lock(_mutex);
    _taken.clear();
    for (const auto& helmet : helmets) {
        RE::FormID actor = 0, form = 0, owner = 0;
        if (!serialization->ResolveFormID(helmet.actor, actor) || !serialization->ResolveFormID(helmet.form, form) ||
            !serialization->ResolveFormID(helmet.owner, owner)) {
            ++skipped;
            continue;
        }
        auto& taken = _taken[actor];
        taken.look = helmet.look;
        taken.copies.push_back({form, owner, helmet.unique});
    }
    if (skipped) logger::warn("Hide Helmets: {} saved helmet record(s) could not be read or resolved", skipped);
    logger::info("Hide Helmets: {} person(s) with helmets Tailor took off", _taken.size());
}

void SituationHelmets::Clear()
{
    std::scoped_lock lock(_mutex);
    _taken.clear();
}
