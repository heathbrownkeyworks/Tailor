#pragma once

// The versioned C++ interface Tailor gives other SKSE plugins through the export RequestTailorInterface
// (TailorAPI.cpp), implemented in CppInterface.cpp over the core in ModApi.h. docs/api/TailorAPI.h, the header
// mod authors copy, holds an identical copy of everything from StringCallback to the end of ITailorInterface1,
// with the notes that document it (ModApiContractTests checks the copy): change both together. Version 1 is
// frozen once released; additions come as a new interface version. The RE:: types come from pch.h.
namespace TailorAPI
{
    // Receives a string from Tailor: UTF-8, valid until the callback returns. context is the pointer you gave
    // the function, handed back unchanged.
    using StringCallback = void (*)(const char* value, void* context);
    // Receives an armor form from Tailor. context is the pointer you gave the function, handed back unchanged.
    using ArmorCallback = void (*)(RE::TESObjectARMO* armor, void* context);

    // Your listener for Tailor's two change events, the same as the Papyrus mod events Tailor_OnOutfitChanged
    // and Tailor_OnSituationChanged (see EVENTS in the notes that open TailorAPI.h's mod API). Register it with
    // ITailorInterface1::AddListener. Tailor calls it on the game thread, so it may call any function of the
    // interface. The listener is yours: Tailor never deletes it (the destructor is protected, so it can't be
    // deleted through this type).
    class ITailorListener1
    {
    public:
        // The Tailor outfit this person wears changed.
        // Same as the Papyrus mod event Tailor_OnOutfitChanged. Required API version: 1.0.
        // - actor: the person, the player or an NPC; never nullptr
        // - outfit: the Tailor outfit they wear now, as GetOutfit names it, or "" when they wear none (their
        //   own gear); valid until this call returns
        virtual void OnOutfitChanged(RE::Actor* actor, const char* outfit) = 0;

        // The situation Tailor judges this person to be in changed.
        // Same as the Papyrus mod event Tailor_OnSituationChanged. Required API version: 1.0.
        // - actor: the person, the player or an NPC; never nullptr
        // - situation: their new situation, as GetSituation names it: "adventuring", "town", "home", "sleep",
        //   "swimming" or "warm" (for the player that can differ from the situation dressing them; see
        //   GetSituation); valid until this call returns
        virtual void OnSituationChanged(RE::Actor* actor, const char* situation) = 0;

    protected:
        ~ITailorListener1() = default;
    };

    // Tailor's mod API, version 1: the functions of Tailor.psc, documented as there, plus AddListener and
    // RemoveListener. The notes that open TailorAPI.h's mod API give the rules every function shares (names,
    // gender, situations, slots, threads, strings and lists). Tailor owns the interface, which lasts as long as
    // the game: never delete it (the destructor is protected).
    class ITailorInterface1
    {
    public:
        // The version of this API: 1.0 in Tailor 3.0. Each function below names the version it needs.
        // Same as Tailor.GetApiVersion in Papyrus. Required API version: 1.0.
        // - returns the API version, 1.0
        virtual float GetApiVersion() = 0;

        // ---- Outfits ----

        // Checks if the player's Tailor library has an outfit of this name. The player can rename or delete
        // any outfit, your mod's own included: check before you use one, and create it again when it is gone.
        // Same as Tailor.DoesOutfitExist in Papyrus. Required API version: 1.0.
        // - outfit: the outfit's name
        // - returns true if the outfit exists; false if not (an empty name is never an outfit)
        virtual bool DoesOutfitExist(const char* outfit) = 0;

        // The Tailor outfit this person wears now. For the player, the outfit Tailor has on them; for an NPC
        // with an override or situation outfits, the outfit Tailor last put on them (during a fight, their
        // Adventuring outfit if they have one); for an NPC with only a regular outfit, or one Tailor hasn't put
        // an outfit on yet (one not loaded), their override if they can wear it, or else their regular outfit if
        // they have no situation outfits and can wear it.
        // Same as Tailor.GetOutfit in Papyrus, with the name handed to a callback. Required API version: 1.0.
        // Game thread only.
        // - actor: the person, the player or an NPC
        // - callback: called once, before GetOutfit returns, with the outfit's name; with "" when they wear
        //   their own gear or anything Tailor didn't put on, and for an NPC Tailor hasn't put an outfit on yet
        //   who has situation outfits (or no regular outfit they can wear) and no override they can wear; not
        //   called when actor is nullptr or a child
        // - returns false when actor is nullptr or a child; true otherwise
        virtual bool GetOutfit(RE::Actor* actor, StringCallback callback, void* context) = 0;

        // Who the outfit is for, as set in Tailor; an NPC is only ever dressed in outfits that fit their sex.
        // Same as Tailor.GetOutfitGender in Papyrus. Required API version: 1.0.
        // - outfit: the outfit's name
        // - returns -1 unisex, 0 male, 1 female; -2 when there is no such outfit
        virtual int GetOutfitGender(const char* outfit) = 0;

        // The outfit's pieces as Armor forms, in the outfit's order. A piece whose plugin is not loaded is left
        // out.
        // Same as Tailor.GetOutfitArmors in Papyrus, with the pieces handed to a callback. Required API
        // version: 1.0.
        // - outfit: the outfit's name
        // - callback: called once for each piece, in order, before GetOutfitArmors returns; not at all when
        //   there is no such outfit or none of its pieces is loaded
        virtual void GetOutfitArmors(const char* outfit, ArmorCallback callback, void* context) = 0;

        // Checks if any piece of the outfit carries the keyword. Any keyword from any plugin works
        // (ArmorClothing, ArmorHelmet, a DLC or mod keyword).
        // Same as Tailor.OutfitHasKeyword in Papyrus. Required API version: 1.0.
        // - outfit: the outfit's name
        // - keyword: the keyword to look for
        // - returns true if a loaded piece has the keyword; false if none has it, there is no such outfit, or
        //   keyword is nullptr
        virtual bool OutfitHasKeyword(const char* outfit, RE::BGSKeyword* keyword) = 0;

        // Checks if any piece of the outfit covers the body slot.
        // Same as Tailor.OutfitUsesSlot in Papyrus. Required API version: 1.0.
        // - outfit: the outfit's name
        // - slot: the body slot number, 30-61 (30 head, 32 body, 33 hands, 37 feet, 52 for many mods)
        // - returns true if a loaded piece covers the slot; false if none does, there is no such outfit, or
        //   slot is outside 30-61
        virtual bool OutfitUsesSlot(const char* outfit, int slot) = 0;

        // Checks if the outfit is in the category.
        // Same as Tailor.IsOutfitInCategory in Papyrus. Required API version: 1.0.
        // - outfit: the outfit's name
        // - category: the category's name as Tailor shows it ("Clothing", "Town", a custom one)
        // - returns true if both exist and the outfit is in the category; false otherwise
        virtual bool IsOutfitInCategory(const char* outfit, const char* category) = 0;

        // ---- Lists ----

        // Checks if Tailor shows a category under this name.
        // Same as Tailor.DoesCategoryExist in Papyrus. Required API version: 1.0.
        // - category: the category's name as Tailor shows it
        // - returns true if the category exists; false if not (an empty name is never a category)
        virtual bool DoesCategoryExist(const char* category) = 0;

        // The names of the outfits in a category, optionally only those a man or a woman can wear.
        // Same as Tailor.GetOutfitsByCategory in Papyrus, with the names handed to a callback. Required API
        // version: 1.0.
        // - category: the category's name as Tailor shows it
        // - gender: -1 every outfit; 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex
        //   outfits)
        // - callback: called once for each outfit name before GetOutfitsByCategory returns; not at all when
        //   the category doesn't exist, has no outfit that passes the filter, or gender is not -1, 0 or 1
        virtual void GetOutfitsByCategory(const char* category, int gender, StringCallback callback, void* context) = 0;

        // How many outfits in a category pass the gender filter: the number of names GetOutfitsByCategory
        // would give.
        // Same as Tailor.GetOutfitCount in Papyrus. Required API version: 1.0.
        // - category: the category's name as Tailor shows it
        // - gender: -1 every outfit; 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex
        //   outfits)
        // - returns the count; 0 when the category doesn't exist, has no outfit that passes the filter, or
        //   gender is not -1, 0 or 1
        virtual int GetOutfitCount(const char* category, int gender) = 0;

        // The outfits in the Adventuring pool, optionally only those also in an armor-type category. With an
        // armor type this is exactly the outfits in both the Adventuring pool and that type's category
        // (Clothing, Light Armor or Heavy Armor); when none is in both, there are none (Tailor's own dressing
        // falls back to the whole pool then, but this list does not).
        // Same as Tailor.GetAdventuringOutfits in Papyrus, with the names handed to a callback. Required API
        // version: 1.0.
        // - armorType: "clothing", "light" or "heavy" (any capitals), or "" (or nullptr) for the whole pool
        // - gender: -1 every outfit; 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex
        //   outfits)
        // - callback: called once for each outfit name before GetAdventuringOutfits returns; not at all when
        //   none passes, armorType is not one of the three or "", or gender is not -1, 0 or 1
        virtual void GetAdventuringOutfits(const char* armorType, int gender, StringCallback callback, void* context) = 0;

        // Every outfit in the library with a piece that carries the keyword. Any keyword from any plugin works.
        // Same as Tailor.GetOutfitsByArmorKeyword in Papyrus, with the names handed to a callback. Required
        // API version: 1.0.
        // - keyword: the keyword to look for (ArmorHeavy, ArmorClothing, a DLC or mod keyword)
        // - gender: -1 every outfit; 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex
        //   outfits)
        // - callback: called once for each outfit name before GetOutfitsByArmorKeyword returns; not at all when
        //   no outfit passes, keyword is nullptr, or gender is not -1, 0 or 1
        virtual void GetOutfitsByArmorKeyword(RE::BGSKeyword* keyword, int gender, StringCallback callback, void* context) = 0;

        // ---- Building ----

        // Adds a new outfit with no pieces to the player's Tailor library; add its pieces with
        // AddArmorToOutfit. It belongs to the player like any other outfit: they can wear it, edit it, rename it
        // or delete it. The library is shared by every save and every character (Tailor's JSON files), so the
        // outfit is there in every save once created: check DoesOutfitExist before creating it. Add the pieces
        // before you put it in a category: an outfit with no pieces in a situation pool can be picked at random
        // and dress someone in nothing, so for a pool create it with no category, add the pieces, then call
        // AddCategory.
        // Same as Tailor.CreateOutfit in Papyrus. Required API version: 1.0. Game thread only.
        // - outfit: the new outfit's name; surrounding spaces are dropped, and no other outfit may have it
        //   (capitals ignored)
        // - category: a category to put it in, which must exist; "" (or nullptr, or only spaces) for none
        // - gender: who it is for: -1 unisex, 0 male, 1 female
        // - returns true when it was created; false when the name is empty or taken, the category doesn't
        //   exist, gender is not -1, 0 or 1, Tailor can't change its outfit files this session, the new outfit
        //   couldn't be saved (then Tailor takes it back for this session; Tailor.log says if a file still lists
        //   it), or when called before Tailor has loaded its library (SKSE plugins only: call after kDataLoaded)
        virtual bool CreateOutfit(const char* outfit, const char* category, int gender) = 0;

        // Adds a piece to the outfit, and dresses everyone wearing it again so they show it. The armor must
        // come from a plugin: one the game created while running (an armor the player enchanted, for one) has
        // no plugin, and is refused.
        // Same as Tailor.AddArmorToOutfit in Papyrus. Required API version: 1.0. Game thread only.
        // - outfit: the outfit's name
        // - armor: the piece to add
        // - returns true when it was added, or the outfit already has it (then nothing changes); false when
        //   there is no such outfit, armor is nullptr or was created while the game ran, Tailor
        //   can't change its outfit files this session, or when called before Tailor has loaded its library
        //   (SKSE plugins only: call after kDataLoaded)
        virtual bool AddArmorToOutfit(const char* outfit, RE::TESObjectARMO* armor) = 0;

        // Removes a piece from the outfit, and dresses everyone wearing it again. Removing the last piece
        // leaves the outfit empty; it is not deleted, and in a situation pool it can still be picked at random
        // and dress someone in nothing.
        // Same as Tailor.RemoveArmorFromOutfit in Papyrus. Required API version: 1.0. Game thread only.
        // - outfit: the outfit's name
        // - armor: the piece to remove
        // - returns true when it was removed; false when there is no such outfit, the outfit doesn't have the
        //   piece, armor is nullptr or was created while the game ran, Tailor
        //   can't change its outfit files this session, or when called before Tailor has loaded its library
        //   (SKSE plugins only: call after kDataLoaded)
        virtual bool RemoveArmorFromOutfit(const char* outfit, RE::TESObjectARMO* armor) = 0;

        // Adds a custom category to the player's Tailor library, which the player can rename or delete.
        // Same as Tailor.CreateCustomCategory in Papyrus. Required API version: 1.0. Game thread only.
        // - category: the new category's name; surrounding spaces are dropped, and no other category may have
        //   it (capitals ignored; the built-in names such as "Warm" are taken)
        // - returns true when it was created; false when the name is empty or taken, Tailor
        //   can't change its outfit files this session, the new category couldn't be saved (then Tailor takes it
        //   back for this session; Tailor.log says if a file still lists it), or when called before Tailor has
        //   loaded its library (SKSE plugins only: call after kDataLoaded)
        virtual bool CreateCustomCategory(const char* category) = 0;

        // Puts the outfit in a category, which can be any category, a situation pool included: an outfit in
        // the Town pool is one of the outfits Tailor picks from for anyone set to a random Town outfit. Add the
        // outfit's pieces first: an outfit with no pieces in a situation pool can be picked at random and dress
        // someone in nothing.
        // Same as Tailor.AddCategory in Papyrus. Required API version: 1.0. Game thread only.
        // - outfit: the outfit's name
        // - category: the category's name as Tailor shows it
        // - returns true when the outfit is now in the category, or already was; false when the outfit or the
        //   category doesn't exist, Tailor can't change its outfit files this session, or when called before
        //   Tailor has loaded its library (SKSE plugins only: call after kDataLoaded)
        virtual bool AddCategory(const char* outfit, const char* category) = 0;

        // ---- Situations ----

        // The situation Tailor judges this person to be in right now, for anyone, whether Tailor dresses them
        // or not. In this order: "swimming" in water, "sleep" in bed, "warm" outdoors in cold weather or a
        // snowy region, "home" in a house (for the player, only their own houses), "town" in a city, town,
        // settlement, inn or other dwelling, and "adventuring" anywhere else. A fight is not a situation: it
        // switches some people to their Adventuring outfit (see GetOutfit). For the player this can differ from
        // the situation dressing them: their outfit counts wading knee-deep as water until they are fully out,
        // and keeps their Sleep outfit on after they wake until they walk away.
        // Same as Tailor.GetSituation in Papyrus, with the name handed to a callback. Required API version:
        // 1.0. Game thread only.
        // - actor: the person, the player or an NPC
        // - callback: called once, before GetSituation returns, with "adventuring", "town", "home", "sleep",
        //   "swimming" or "warm"; not called when actor is nullptr or a child
        // - returns false when actor is nullptr or a child; true otherwise
        virtual bool GetSituation(RE::Actor* actor, StringCallback callback, void* context) = 0;

        // ---- Overrides ----
        //
        // How an override works: it dresses someone in a Tailor outfit until it is cleared.
        // - anyone can have one: the player, or any NPC who is alive, not a child and not a creature, set up in
        //   Tailor or not
        // - one per person: the last override set wins, whichever mod set it
        // - it is kept in the save, and lasts through saving and loading until it is cleared
        // - while it is on, sleeping, swimming, the cold and changing location never replace it; a fight can:
        //   an NPC with an Adventuring outfit fights in it, and an NPC without one keeps the override; the
        //   player with situation outfits fights in their Adventuring outfit, or in their own gear without one,
        //   and the player without situation outfits keeps the override; in every case the override comes back
        //   when the fight ends
        // - it is cleared by ClearOutfitOverride, by the player confirming an outfit for that person in
        //   Tailor's screens or resetting them there, and by deleting the outfit it uses
        // - an outfit the person can't wear any more (the player changed its gender) is skipped, not cleared,
        //   until it fits them again
        // - a random pick (OverrideWithSituation, OverrideWithCategory) is made once, when it is set, and kept
        // - wigs don't change, and Tailor's Hide Weapons and Hide Helmets keep following the situation
        // - one set by OverrideWithSituation for "swimming" or "sleep" is that look while they wear it,
        //   wherever they are: their weapons, shields, quivers and torches are hidden as in Tailor's own
        //   Swimming and Sleep looks, and show in a fight or when drawn; one set for any other situation, or
        //   by OverrideWithOutfit or OverrideWithCategory, hides none itself, even in bed (Tailor's Hide
        //   Weapons setting still applies)
        // - it goes on at once, also on someone already in water: an NPC swimming in their own Tailor Swimming
        //   outfit changes into the override there, and keeps it when they leave the water
        // - setting one returns true when Tailor accepts it, and they are dressed from the next frame on,
        //   sometimes a little later; an NPC who isn't loaded is dressed when they load, and someone open in
        //   Tailor's outfit editor when it closes
        // Every function here takes an actor and is game thread only.

        // Dresses the person in this outfit until the override is cleared.
        // Same as Tailor.OverrideWithOutfit in Papyrus. Required API version: 1.0.
        // - actor: the person, the player or an NPC
        // - outfit: the outfit's name; it must fit the person's sex (GetOutfitGender)
        // - returns true when accepted; false when actor is nullptr, dead, a child or a creature, there is no
        //   such outfit, or it doesn't fit them
        virtual bool OverrideWithOutfit(RE::Actor* actor, const char* outfit) = 0;

        // Dresses the person for a situation, wherever they are, until the override is cleared: in their own
        // Tailor choice for that situation (a fixed outfit, or today's random pick when they are set to
        // Random), or without one in a random outfit they can wear from that situation's pool; the outfit is
        // picked now and kept.
        // Same as Tailor.OverrideWithSituation in Papyrus. Required API version: 1.0.
        // - actor: the person, the player or an NPC
        // - situation: "adventuring", "town", "home", "sleep", "swimming" or "warm" (any capitals)
        // - returns true when accepted; false when actor is nullptr, dead, a child or a creature, situation is
        //   not a situation, or there is no outfit for it they can wear
        virtual bool OverrideWithSituation(RE::Actor* actor, const char* situation) = 0;

        // Dresses the person in a random outfit they can wear from a category until the override is cleared;
        // the outfit is picked now and kept.
        // Same as Tailor.OverrideWithCategory in Papyrus. Required API version: 1.0.
        // - actor: the person, the player or an NPC
        // - category: the category's name as Tailor shows it: an armor type, a situation pool or a custom
        //   category
        // - returns true when accepted; false when actor is nullptr, dead, a child or a creature, the category
        //   doesn't exist, or it has no outfit they can wear
        virtual bool OverrideWithCategory(RE::Actor* actor, const char* category) = 0;

        // Checks if the person has an override, set by any mod: true from the moment it is set, before the
        // outfit is on, and while it is skipped because they can't wear it.
        // Same as Tailor.HasOutfitOverride in Papyrus. Required API version: 1.0.
        // - actor: the person, the player or an NPC
        // - returns true if they have an override; false if not, or actor is nullptr or a child
        virtual bool HasOutfitOverride(RE::Actor* actor) = 0;

        // Ends the person's override, whichever mod set it, and dresses them as Tailor normally would: the
        // player wears their situation or regular outfit, or their own gear when they have neither; an NPC
        // with an outfit assignment (a regular outfit or situation outfits) wears it; any other NPC, one with
        // only wigs in Tailor included, gets their own outfit back and keeps their wig.
        // Same as Tailor.ClearOutfitOverride in Papyrus. Required API version: 1.0.
        // - actor: the person, the player or an NPC
        // - returns true when they had an override and it is cleared (they are dressed from the next frame on,
        //   sometimes a little later); false when they had none, or actor is nullptr or a child
        virtual bool ClearOutfitOverride(RE::Actor* actor) = 0;

        // ---- Utility ----

        // Makes Tailor decide the person's outfit again and dress them, from the next frame on, as it does
        // when their situation changes; use it after changing something Tailor's choice depends on. Works for
        // the player, and for an NPC Tailor dresses (an outfit assignment or situation wigs) or one with an
        // override; an NPC who isn't loaded is dressed when they load.
        // Same as Tailor.EvaluateOutfit in Papyrus. Required API version: 1.0. Game thread only.
        // - actor: the person, the player or an NPC
        // - returns true when accepted; false when actor is nullptr or a child, or the NPC has no override, no
        //   outfit assignment and no situation wigs (a Hair Dresser wig or hair color alone doesn't count)
        virtual bool EvaluateOutfit(RE::Actor* actor) = 0;

        // ---- Events ----

        // Starts telling the listener about changes (see EVENTS in the notes that open TailorAPI.h's mod API):
        // its functions are called for each change Tailor sees from now on. It stays registered, through loads,
        // until RemoveListener, and must outlive its registration. Adding one already added, or nullptr, does
        // nothing.
        // Papyrus registers for the same events with RegisterForModEvent. Required API version: 1.0. Game
        // thread only.
        // - listener: your listener
        virtual void AddListener(ITailorListener1* listener) = 0;

        // Stops telling the listener about changes; call it before destroying the listener. It may be called
        // from inside a listener's function: a listener removed then is not called again. Removing one that was
        // never added, or nullptr, does nothing.
        // Required API version: 1.0. Game thread only.
        // - listener: the listener given to AddListener
        virtual void RemoveListener(ITailorListener1* listener) = 0;

        // ---- Situations, continued: added last to keep the methods above in their vtable order ----

        // Checks if the person has their own Tailor outfit for a situation, one Tailor would put on them: a fixed
        // outfit they can wear, or Random with at least one outfit they can wear in that situation's pool (for
        // "adventuring", one their armor type allows); that is when OverrideWithSituation dresses them in their own
        // choice for it, rather than falling back to a random outfit from the situation's pool. Where they are now
        // doesn't matter, and neither do a mod's override or situation wigs; an outfit Tailor falls back to when a
        // situation has none of its own doesn't count ("home" doesn't count a town outfit); asking never picks
        // today's random outfit.
        // Same as Tailor.HasSituation in Papyrus. Required API version: 1.0. Game thread only.
        // - actor: the person, the player or an NPC
        // - situation: "adventuring", "town", "home", "sleep", "swimming" or "warm" (any capitals)
        // - returns true if they have such an outfit; false if they have none for that situation (including when
        //   the outfit set for it no longer fits their sex, or for "adventuring" their armor type, or it is set
        //   to Random with no outfit they can wear in that situation's pool), situation is not a situation or is
        //   nullptr, or actor is nullptr or a child
        virtual bool HasSituation(RE::Actor* actor, const char* situation) = 0;

    protected:
        ~ITailorInterface1() = default;
    };
}
