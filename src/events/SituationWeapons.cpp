#include "events/SituationWeapons.h"
#include "events/SituationWeaponsPolicy.h"

namespace
{
    bool AncestorOrSelf(const RE::NiAVObject* ancestor, const RE::NiAVObject* node)
    {
        for (auto* current = node; current; current = current->parent) {
            if (current == ancestor) return true;
        }
        return false;
    }
}

SituationWeapons& SituationWeapons::GetSingleton()
{
    // Never destroyed: the nodes it holds must not be released after the game has torn its world down.
    static auto* singleton = new SituationWeapons();
    return *singleton;
}

void SituationWeapons::Update(RE::Actor* actor, Set set)
{
    if (!actor) return;
    std::scoped_lock lock(_mutex);
    const auto actorId = actor->GetFormID();
    if (set == Set::None) {
        if (ShowLocked(actorId)) logger::info("Situations: the weapons of {:08X} are back", actorId);
        return;
    }
    auto [it, fresh] = _ledgers.try_emplace(actorId);
    auto& hidden = it->second;
    if (hidden.set != set) {
        // A torch the Sleep look hid comes back once only Hide Weapons applies, and the other way round.
        hidden.ledger.Restore();
        hidden.set = set;
        fresh = true;
    }
    Hide(actor, hidden);
    if (fresh && hidden.ledger.Size() > 0) {
        logger::info("Situations: hid {} weapon model(s) on {:08X}{}", hidden.ledger.Size(), actorId,
            set == Set::WithTorch ? " for their Sleep or Swimming look" : " (Hide Weapons)");
    }
}

void SituationWeapons::Show(RE::FormID actorId)
{
    std::scoped_lock lock(_mutex);
    ShowLocked(actorId);
}

void SituationWeapons::ShowAllExcept(const std::unordered_set<RE::FormID>& updated)
{
    std::scoped_lock lock(_mutex);
    for (auto it = _ledgers.begin(); it != _ledgers.end();) {
        if (updated.contains(it->first)) {
            ++it;
        } else {
            it->second.ledger.Restore();
            it = _ledgers.erase(it);
        }
    }
}

void SituationWeapons::ShowAll()
{
    std::scoped_lock lock(_mutex);
    for (auto& [actorId, hidden] : _ledgers) hidden.ledger.Restore();
    _ledgers.clear();
}

bool SituationWeapons::ShowLocked(RE::FormID actorId)
{
    const auto it = _ledgers.find(actorId);
    if (it == _ledgers.end()) return false;
    const bool hidden = it->second.ledger.Size() > 0;
    it->second.ledger.Restore();
    _ledgers.erase(it);
    return hidden;
}

void SituationWeapons::Hide(RE::Actor* actor, Hidden& hidden)
{
    auto& ledger = hidden.ledger;
    const bool torch = hidden.set == Set::WithTorch;
    auto* root = actor->Get3D(false);
    // A model rebuild leaves hidden nodes behind: those go back as they were, the rest are hidden
    // again, and the sweeps below find what the rebuild created.
    ledger.Reassert([&](const RE::NiAVObject* node) { return !root || !AncestorOrSelf(root, node); });
    if (!root) return;
    // A node something else already hides (Immersive Equipment Displays' own conditions, say) is
    // left to it, so showing never hides again what that mod has shown since.
    const auto hide = [&](RE::NiAVObject* node) {
        if (ledger.Owns(node) || !node->GetAppCulled()) ledger.Hide(node);
    };
    // Weapons drawn or sheathed, the quiver, the shield, and with the look a held torch.
    if (const auto biped = actor->GetBiped(false)) {
        for (const auto& part : biped->objects) {
            if (!part.item || !part.partClone) continue;
            const auto* armor = part.item->As<RE::TESObjectARMO>();
            const bool shield = armor && armor->IsShield();
            if ((torch ? Tailor::Situations::HiddenInSleepOrSwim(part.item->GetFormType(), shield) :
                    Tailor::Situations::HiddenByHideWeapons(part.item->GetFormType(), shield)) &&
                AncestorOrSelf(root, part.partClone.get())) {
                hide(part.partClone.get());
            }
        }
    }
    // A drawn weapon's scabbard, and Immersive Equipment Displays' weapon, quiver, torch and shield models.
    RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* node) {
        if (torch ? Tailor::Situations::HiddenNodeInSleepOrSwim(node->name.c_str()) :
                Tailor::Situations::HiddenNodeByHideWeapons(node->name.c_str())) hide(node);
        return RE::BSVisit::BSVisitControl::kContinue;
    });
}
