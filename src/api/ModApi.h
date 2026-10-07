#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace RE
{
    class Actor;
    class BGSKeyword;
    class TESObjectARMO;
}

namespace TailorAPI
{
    class ITailorInterface1;
    class ITailorListener1;
}

// The one core behind the Papyrus script and the C++ interface. Outfits and categories are named as
// Tailor's screens show them, matched ignoring capitals and surrounding spaces; a name that is empty
// after trimming matches nothing. A name's bytes that aren't valid UTF-8 become '?' (ApiName), for creating
// and for matching alike. A missing outfit, category, form or actor makes a function return false,
// -2, 0, "" or an empty list, and logs one line.
//
// Threads: the library's queries (GetApiVersion through GetOutfitsByArmorKeyword) read the stores through
// their locks and may run on any thread. The building functions (CreateOutfit through AddCategory) change the
// stores through their locks but must run on the game thread: it holds pointers into the stores and walks
// them between their locks, which a change from another thread could leave dangling. Library changes save
// the JSON files and refresh an open Tailor on the game thread. The functions that take an RE::Actor*
// (GetOutfit, GetSituation, HasSituation, the OverrideWith functions, HasOutfitOverride, ClearOutfitOverride
// and EvaluateOutfit) read the game's and Tailor's unlocked state and must run on the game thread too. The
// Papyrus natives all do: they are not callable from tasklets, so the VM runs them in step with the game
// thread. The change events and their listeners live on the game thread too.
namespace Tailor::Api
{
    // The version of this interface: 1.0.
    float GetApiVersion();

    // Whether an outfit of this name exists.
    bool DoesOutfitExist(std::string_view outfit);
    // -1 unisex, 0 male, 1 female; -2 when there is no such outfit.
    int GetOutfitGender(std::string_view outfit);
    // The outfit's pieces whose plugins are loaded; empty when there is no such outfit.
    std::vector<RE::TESObjectARMO*> GetOutfitArmors(std::string_view outfit);
    // Whether any piece carries the keyword.
    bool OutfitHasKeyword(std::string_view outfit, RE::BGSKeyword* keyword);
    // Whether any piece covers the body slot (30-61).
    bool OutfitUsesSlot(std::string_view outfit, int slot);
    // Whether both exist and the outfit is in the category.
    bool IsOutfitInCategory(std::string_view outfit, std::string_view category);

    bool DoesCategoryExist(std::string_view category);
    // Gender: -1 all outfits; 0 what a man can wear; 1 what a woman can wear (unisex included).
    std::vector<std::string> GetOutfitsByCategory(std::string_view category, int gender);
    int GetOutfitCount(std::string_view category, int gender);
    // Outfits in the Adventuring pool; armorType "clothing", "light" or "heavy" keeps those also in a
    // category of that type, and "" keeps all. An unknown type gives an empty list.
    std::vector<std::string> GetAdventuringOutfits(std::string_view armorType, int gender);
    // Outfits with any piece carrying the keyword.
    std::vector<std::string> GetOutfitsByArmorKeyword(RE::BGSKeyword* keyword, int gender);

    // The building functions below return false, changing nothing, until MarkLibraryLoaded: before then the
    // stores are empty, and saving them would write an empty library over the player's files.

    // Adds an outfit to the player's library, in the category when one is given (it must exist). Gender
    // is -1, 0 or 1. False when the name is empty or taken, or the category or gender is wrong.
    bool CreateOutfit(std::string_view outfit, std::string_view category, int gender);
    // Adds a piece, re-dressing whoever wears the outfit. True when the outfit already has it. False for
    // a missing outfit, or an armor that is missing or was made at run time, since it has no plugin.
    bool AddArmorToOutfit(std::string_view outfit, RE::TESObjectARMO* armor);
    // Removes a piece, re-dressing whoever wears the outfit. False when the outfit doesn't have it.
    bool RemoveArmorFromOutfit(std::string_view outfit, RE::TESObjectARMO* armor);
    // False when the name is empty or taken.
    bool CreateCustomCategory(std::string_view category);
    // Puts the outfit in the category. True when it already is.
    bool AddCategory(std::string_view outfit, std::string_view category);

    // The Tailor outfit this person wears now: the player's as the wardrobe has it; for an NPC with an override or
    // situation outfits, as Tailor last put it on; for one with only a regular outfit, or not dressed by Tailor
    // yet, their override if they can wear it, else their regular outfit when they have no situations and can
    // wear it (SituationHandler::CurrentOutfitId). "" for their own gear.
    std::string GetOutfit(RE::Actor* actor);
    // "adventuring", "town", "home", "sleep", "swimming" or "warm", as Tailor judges it now.
    std::string GetSituation(RE::Actor* actor);
    // Whether the person has their own Tailor outfit for the situation, one Tailor would put on them: a fixed outfit
    // they can wear, or Random with an outfit they can wear in the pool (see SituationHandler::HasSituationChoice).
    // Never picks today's random outfit. False for a name that isn't a situation, a None actor or a child.
    bool HasSituation(RE::Actor* actor, std::string_view situation);

    // Overrides: the player, or an NPC Tailor can dress (alive, not a child, not a creature), wears a Tailor outfit until
    // it is cleared; Sleep, Swimming, Warm and location never replace it. A fight switches to the Adventuring
    // outfit only for people who normally change for fights (an Adventuring outfit for an NPC; situation
    // outfits for the player), and the override returns after it. Setting, clearing and EvaluateOutfit return
    // true when the request is accepted; the dressing runs in a later game-thread task, and someone open in
    // Tailor's outfit editor is dressed when it closes. The last override set wins.

    // That outfit, if it fits the actor's sex.
    bool OverrideWithOutfit(RE::Actor* actor, std::string_view outfit);
    // Their own choice for the situation, else a random outfit that fits from its pool, picked once. False
    // when there is none.
    bool OverrideWithSituation(RE::Actor* actor, std::string_view situation);
    // A random outfit that fits from the category, picked once. False when there is none.
    bool OverrideWithCategory(RE::Actor* actor, std::string_view category);
    bool HasOutfitOverride(RE::Actor* actor);
    // Dresses them as Tailor normally would; an NPC Tailor keeps no outfit for gets their own back. False
    // when they had no override.
    bool ClearOutfitOverride(RE::Actor* actor);
    // Re-decides the outfit of the player, or of an NPC Tailor dresses or overrides, and dresses them now.
    // False for anyone else.
    bool EvaluateOutfit(RE::Actor* actor);

    // Change events, sent at the end of the situation poll (actorIds: the NPCs it polled) to Papyrus as SKSE
    // mod events and to the C++ listeners. Watched: the player, the NPCs Tailor dresses, anyone with an
    // override, and anyone watched since the last load or new game whose form the game still holds, while
    // loaded and alive. Each one's outfit and situation are compared with the last seen; the first sighting
    // only records. An outfit Tailor is still putting back on (SituationHandler::OutfitPending) is not known
    // yet: the last known one stays, so after a load it is a first sighting too. An override set on someone not
    // watched yet records what they wore then, so it is told when it goes on.
    void PollEvents(const std::unordered_set<RE::FormID>& actorIds);
    // A load or a new game: everyone's next sighting is a first one again.
    void ForgetEvents();
    // The C++ interface's listeners. Adding one twice, or removing one never added, does nothing; one removed
    // while the listeners are being called is not called again.
    void AddListener(TailorAPI::ITailorListener1* listener);
    void RemoveListener(TailorAPI::ITailorListener1* listener);

    // Called once the outfit store and the library have loaded at kDataLoaded: the building functions work
    // from then on.
    void MarkLibraryLoaded();

    // The C++ interface version 1, which RequestTailorInterface(1) gives other SKSE plugins.
    TailorAPI::ITailorInterface1* Interface1();
}
