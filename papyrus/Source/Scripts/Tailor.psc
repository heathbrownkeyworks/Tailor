;/* Tailor
* * the mod API of Tailor, the outfit and wig manager
* * read the player's Tailor outfits and categories, build outfits in their library, ask which outfit
* * someone wears and which situation they are in, and dress anyone in a Tailor outfit until your mod
* * lets go (an override)
* *
* * required: Tailor 3.0 or newer, API version 1.0 (check Tailor.GetApiVersion())
* *
* * HOW TO CALL
* * call each function on the script by name: Tailor.FunctionName(...)
* * every function is Global Native, so there is nothing to attach, no property to fill and no ESP or
* * master to add; compile against this file, and at run time Tailor's SKSE plugin provides the functions
* *
* * without Tailor installed a call fails and Papyrus logs it, so check the version before anything else:
* *
* *     If Tailor.GetApiVersion() >= 1.0
* *         ; Tailor is there
* *     EndIf
* *
* * RULES EVERY FUNCTION SHARES
* * - outfits and categories go by the names Tailor's screens show, ignoring capitals and surrounding
* *   spaces: " steel plate" finds "Steel Plate" (Papyrus compares strings ignoring capitals too)
* * - names are UTF-8: in an outfit or category name you pass, a byte that isn't valid UTF-8 becomes "?",
* *   for creating and for finding alike, so save your scripts as UTF-8
* * - outfit names are unique, ignoring capitals; so are the names of new categories
* * - categories: the armor types (Clothing, Light Armor, Heavy Armor), the situation pools (Adventuring,
* *   Town, Home, Sleep, Swimming, Warm) and the player's custom categories; a custom category with the
* *   same name as another shows in Tailor as "Warm (2)", and that is its name here as well
* * - gender: an outfit is unisex (-1), male (0) or female (1); the aiGender filter of the list
* *   functions takes -1 for every outfit, 0 for what a man can wear and 1 for what a woman can wear
* *   (both with the unisex outfits); any other value matches no outfit
* * - situations are named "adventuring", "town", "home", "sleep", "swimming" and "warm" (any capitals)
* * - slots are the body slot numbers 30-61, as the Creation Kit shows them (32 body, 33 hands, 37 feet)
* * - a missing outfit, category or form, or a None actor or a child (Tailor never handles children),
* *   makes a function return "", -2, 0, false or an empty array (each function says which), and
* *   Tailor's log (SKSE\Tailor.log) says why in one line
* * - the outfit library (the outfits and categories, and everything the building functions change) is
* *   shared by every save and every character, in Tailor's JSON files; overrides are kept per save
* * - when Tailor couldn't fully read its outfit files at startup (outfits.json or library.json: a
* *   broken line, say), it changes neither until it is restarted with the file fixed: the building
* *   functions return false and change nothing, and Tailor's log says why
* * - a change Tailor accepted but couldn't write to its files (another program holding one, a full
* *   disk) holds for this session and is written with the next save that succeeds, and Tailor's
* *   log says so; CreateOutfit and CreateCustomCategory instead take back what they made and return
* *   false
* * - Tailor dresses an NPC who has an outfit assignment in Tailor (a regular outfit or situation
* *   outfits) or situation wigs; a Hair Dresser wig or hair color alone doesn't count
* * - the functions that dress someone (the overrides, ClearOutfitOverride, EvaluateOutfit, and adding or
* *   removing a piece of an outfit someone wears) return true when Tailor accepts the request; the
* *   outfit goes on from the next frame on, sometimes a little later, and GetOutfit can name the
* *   previous outfit until then
* *
* * EVENTS
* * Tailor sends two SKSE mod events. Register for them with RegisterForModEvent on any form, alias or
* * magic effect script, and register again after every game load, since SKSE forgets registrations
* * (for example from OnInit and from a player alias's OnPlayerLoadGame):
* *
* *     RegisterForModEvent("Tailor_OnOutfitChanged", "OnTailorOutfitChanged")
* *     RegisterForModEvent("Tailor_OnSituationChanged", "OnTailorSituationChanged")
* *
* *     Event OnTailorOutfitChanged(String asEventName, String asOutfit, Float afNumArg, Form akSender)
* *         Actor akWearer = akSender as Actor
* *     EndEvent
* *
* *     Event OnTailorSituationChanged(String asEventName, String asSituation, Float afNumArg, Form akSender)
* *         Actor akPerson = akSender as Actor
* *     EndEvent
* *
* * - Tailor_OnOutfitChanged: asOutfit is the Tailor outfit the person wears now, as GetOutfit names it,
* *   or "" when they wear none (their own gear)
* * - Tailor_OnSituationChanged: asSituation is the person's new situation, as GetSituation names it (for
* *   the player that can differ from the situation dressing them; see GetSituation)
* * - akSender is the actor the event is about (cast it with "as Actor"); afNumArg is always 0
* * - Tailor checks for changes about every 250 ms, and an event is sent by the first check after the
* *   change; no check runs while Tailor's menu is open, the game is paused, or the main menu or a loading
* *   screen is up, and a change made then is sent by the first check after, if it still holds
* * - no event is sent for the first sighting of someone after a load or a new game; an outfit Tailor is
* *   still putting back on (an NPC's situation outfit after a load, for one) is not known until it is on,
* *   and then sends an event only if it differs from the last outfit known since the load, so after a
* *   load it counts as a first sighting too
* * - an override set on someone Tailor isn't watching yet (an NPC it doesn't dress, say) sends
* *   Tailor_OnOutfitChanged at the first check after it is set, naming the override as GetOutfit does: the
* *   outfit they wore when it was set counts as the last one known, unless Tailor was still putting that
* *   one back on
* * - Tailor watches the player, every NPC it dresses or has an override for, and anyone it has watched
* *   since the last load or new game (until the game drops them from memory), while they are loaded and
* *   alive, and never a child; so an event is still sent when Tailor stops dressing someone or their
* *   override ends
* *
* * EXAMPLE
* * a trigger box around a bath house dresses whoever walks in for swimming, and gives them their usual
* * outfit back when they leave (it may cover the water itself: an override goes on at once, in water too):
* *
* *     Event OnTriggerEnter(ObjectReference akActionRef)
* *         Actor akBather = akActionRef as Actor
* *         If akBather
* *             Tailor.OverrideWithSituation(akBather, "swimming")
* *         EndIf
* *     EndEvent
* *
* *     Event OnTriggerLeave(ObjectReference akActionRef)
* *         Actor akBather = akActionRef as Actor
* *         If akBather
* *             Tailor.ClearOutfitOverride(akBather)
* *         EndIf
* *     EndEvent
*/;
Scriptname Tailor Hidden

;/* GetApiVersion
* * the version of this API, 1.0 in Tailor 3.0; each function below names the version it needs
* * check it once before using the API: without Tailor installed the call fails and Papyrus logs it
* *
* * required API version: 1.0
* *
* * @return: the API version, 1.0
*/;
Float Function GetApiVersion() Global Native


;  ██████╗ ██╗   ██╗████████╗███████╗██╗████████╗███████╗
; ██╔═══██╗██║   ██║╚══██╔══╝██╔════╝██║╚══██╔══╝██╔════╝
; ██║   ██║██║   ██║   ██║   █████╗  ██║   ██║   ███████╗
; ██║   ██║██║   ██║   ██║   ██╔══╝  ██║   ██║   ╚════██║
; ╚██████╔╝╚██████╔╝   ██║   ██║     ██║   ██║   ███████║
;  ╚═════╝  ╚═════╝    ╚═╝   ╚═╝     ╚═╝   ╚═╝   ╚══════╝

;/* DoesOutfitExist
* * checks if the player's Tailor library has an outfit of this name
* * the player can rename or delete any outfit, your mod's own included: check before you use one, and
* * create it again when it is gone
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* *
* * @return: true if the outfit exists, false if not (an empty name is never an outfit)
*/;
Bool Function DoesOutfitExist(String asOutfit) Global Native

;/* GetOutfit
* * the Tailor outfit this person wears now
* * for the player, the outfit Tailor has on them; for an NPC with an override or situation outfits, the
* * outfit Tailor last put on them (during a fight, their Adventuring outfit if they have one); for an NPC
* * with only a regular outfit, or one Tailor hasn't put an outfit on yet (one not loaded), their override if
* * they can wear it, or else their regular outfit if they have no situation outfits and can wear it
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* *
* * @return: the outfit's name; "" when akActor is None or a child, when they wear their own gear or
* *          anything Tailor didn't put on, and for an NPC Tailor hasn't put an outfit on yet who has
* *          situation outfits (or no regular outfit they can wear) and no override they can wear
*/;
String Function GetOutfit(Actor akActor) Global Native

;/* GetOutfitGender
* * who the outfit is for, as set in Tailor; an NPC is only ever dressed in outfits that fit their sex
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* *
* * @return: -1 unisex, 0 male, 1 female; -2 when there is no such outfit
*/;
Int Function GetOutfitGender(String asOutfit) Global Native

;/* GetOutfitArmors
* * the outfit's pieces as Armor forms, in the outfit's order
* * a piece whose plugin is not loaded is left out
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* *
* * @return: the pieces; an empty array when there is no such outfit or none of its pieces is loaded
*/;
Armor[] Function GetOutfitArmors(String asOutfit) Global Native

;/* OutfitHasKeyword
* * checks if any piece of the outfit carries the keyword
* * pass your own Keyword property: any keyword from any plugin works (ArmorClothing, ArmorHelmet, a DLC
* * or mod keyword), and your plugin needs no master for Tailor
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* * @param: akKeyword, the keyword to look for
* *
* * @return: true if a loaded piece has the keyword; false if none has it, there is no such outfit, or
* *          akKeyword is None
*/;
Bool Function OutfitHasKeyword(String asOutfit, Keyword akKeyword) Global Native

;/* OutfitUsesSlot
* * checks if any piece of the outfit covers the body slot
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* * @param: aiSlot, the body slot number, 30-61 (30 head, 32 body, 33 hands, 37 feet, 52 for many mods)
* *
* * @return: true if a loaded piece covers the slot; false if none does, there is no such outfit, or
* *          aiSlot is outside 30-61
*/;
Bool Function OutfitUsesSlot(String asOutfit, Int aiSlot) Global Native

;/* IsOutfitInCategory
* * checks if the outfit is in the category
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* * @param: asCategory, the category's name as Tailor shows it ("Clothing", "Town", a custom one)
* *
* * @return: true if both exist and the outfit is in the category; false otherwise
*/;
Bool Function IsOutfitInCategory(String asOutfit, String asCategory) Global Native


; ██╗     ██╗███████╗████████╗███████╗
; ██║     ██║██╔════╝╚══██╔══╝██╔════╝
; ██║     ██║███████╗   ██║   ███████╗
; ██║     ██║╚════██║   ██║   ╚════██║
; ███████╗██║███████║   ██║   ███████║
; ╚══════╝╚═╝╚══════╝   ╚═╝   ╚══════╝

;/* DoesCategoryExist
* * checks if Tailor shows a category under this name
* *
* * required API version: 1.0
* *
* * @param: asCategory, the category's name as Tailor shows it
* *
* * @return: true if the category exists, false if not (an empty name is never a category)
*/;
Bool Function DoesCategoryExist(String asCategory) Global Native

;/* GetOutfitsByCategory
* * the names of the outfits in a category, optionally only those a man or a woman can wear
* *
* * required API version: 1.0
* *
* * @param: asCategory, the category's name as Tailor shows it
* * @param: aiGender, -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1
* *         include the unisex outfits)
* *
* * @return: the outfit names; an empty array when the category doesn't exist, has no outfit that passes
* *          the filter, or aiGender is not -1, 0 or 1
*/;
String[] Function GetOutfitsByCategory(String asCategory, Int aiGender = -1) Global Native

;/* GetOutfitCount
* * how many outfits in a category pass the gender filter: the length GetOutfitsByCategory would return
* *
* * required API version: 1.0
* *
* * @param: asCategory, the category's name as Tailor shows it
* * @param: aiGender, -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1
* *         include the unisex outfits)
* *
* * @return: the count; 0 when the category doesn't exist, has no outfit that passes the filter, or
* *          aiGender is not -1, 0 or 1
*/;
Int Function GetOutfitCount(String asCategory, Int aiGender = -1) Global Native

;/* GetAdventuringOutfits
* * the outfits in the Adventuring pool, optionally only those also in an armor-type category
* * with an armor type this is exactly the outfits in both the Adventuring pool and that type's category
* * (Clothing, Light Armor or Heavy Armor); when none is in both, the array is empty (Tailor's own
* * dressing falls back to the whole pool then, but this list does not)
* *
* * required API version: 1.0
* *
* * @param: asArmorType, "clothing", "light" or "heavy" (any capitals), or "" for the whole pool (default)
* * @param: aiGender, -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1
* *         include the unisex outfits)
* *
* * @return: the outfit names; an empty array when none passes, asArmorType is not one of the three or "",
* *          or aiGender is not -1, 0 or 1
*/;
String[] Function GetAdventuringOutfits(String asArmorType = "", Int aiGender = -1) Global Native

;/* GetOutfitsByArmorKeyword
* * every outfit in the library with a piece that carries the keyword
* * pass your own Keyword property: any keyword from any plugin works
* *
* * required API version: 1.0
* *
* * @param: akKeyword, the keyword to look for (ArmorHeavy, ArmorClothing, a DLC or mod keyword)
* * @param: aiGender, -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1
* *         include the unisex outfits)
* *
* * @return: the outfit names; an empty array when no outfit passes, akKeyword is None, or aiGender is
* *          not -1, 0 or 1
*/;
String[] Function GetOutfitsByArmorKeyword(Keyword akKeyword, Int aiGender = -1) Global Native


; ██████╗ ██╗   ██╗██╗██╗     ██████╗ ██╗███╗   ██╗ ██████╗
; ██╔══██╗██║   ██║██║██║     ██╔══██╗██║████╗  ██║██╔════╝
; ██████╔╝██║   ██║██║██║     ██║  ██║██║██╔██╗ ██║██║  ███╗
; ██╔══██╗██║   ██║██║██║     ██║  ██║██║██║╚██╗██║██║   ██║
; ██████╔╝╚██████╔╝██║███████╗██████╔╝██║██║ ╚████║╚██████╔╝
; ╚═════╝  ╚═════╝ ╚═╝╚══════╝╚═════╝ ╚═╝╚═╝  ╚═══╝ ╚═════╝

;/* CreateOutfit
* * adds a new outfit with no pieces to the player's Tailor library; add its pieces with AddArmorToOutfit
* * it belongs to the player like any other outfit: they can wear it, edit it, rename it or delete it
* * the library is shared by every save and every character (Tailor's JSON files), so the outfit is
* * there in every save once created: check DoesOutfitExist before creating it
* * add the pieces before you put it in a category: an outfit with no pieces in a situation pool can be
* * picked at random and dress someone in nothing, so for a pool create it with no category, add the
* * pieces, then call AddCategory
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the new outfit's name; surrounding spaces are dropped, and no other outfit may have
* *         it (capitals ignored)
* * @param: asCategory, a category to put it in, which must exist; "" or only spaces for none (default)
* * @param: aiGender, who it is for: -1 unisex (default), 0 male, 1 female
* *
* * @return: true when it was created; false when the name is empty or taken, the category doesn't
* *          exist, aiGender is not -1, 0 or 1, Tailor can't change its outfit files this session, or
* *          the new outfit couldn't be saved (then Tailor takes it back for this session; Tailor.log
* *          says if a file still lists it)
*/;
Bool Function CreateOutfit(String asOutfit, String asCategory = "", Int aiGender = -1) Global Native

;/* AddArmorToOutfit
* * adds a piece to the outfit, and dresses everyone wearing it again so they show it
* * the armor must come from a plugin: one the game created while running (an armor the player
* * enchanted, for one) has no plugin, and is refused
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* * @param: akArmor, the piece to add
* *
* * @return: true when it was added, or the outfit already has it (then nothing changes); false when there
* *          is no such outfit, akArmor is None or was created while the game ran, or Tailor
* *          can't change its outfit files this session
*/;
Bool Function AddArmorToOutfit(String asOutfit, Armor akArmor) Global Native

;/* RemoveArmorFromOutfit
* * removes a piece from the outfit, and dresses everyone wearing it again
* * removing the last piece leaves the outfit empty; it is not deleted, and in a situation pool it can
* * still be picked at random and dress someone in nothing
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* * @param: akArmor, the piece to remove
* *
* * @return: true when it was removed; false when there is no such outfit, the outfit doesn't have the
* *          piece, akArmor is None or was created while the game ran, or Tailor
* *          can't change its outfit files this session
*/;
Bool Function RemoveArmorFromOutfit(String asOutfit, Armor akArmor) Global Native

;/* CreateCustomCategory
* * adds a custom category to the player's Tailor library, which the player can rename or delete
* *
* * required API version: 1.0
* *
* * @param: asCategory, the new category's name; surrounding spaces are dropped, and no other category
* *         may have it (capitals ignored; the built-in names such as "Warm" are taken)
* *
* * @return: true when it was created; false when the name is empty or taken, Tailor
* *          can't change its outfit files this session, or the new category couldn't be saved
* *          (then Tailor takes it back for this session; Tailor.log says if a file still lists it)
*/;
Bool Function CreateCustomCategory(String asCategory) Global Native

;/* AddCategory
* * puts the outfit in a category, which can be any category, a situation pool included: an outfit in
* * the Town pool is one of the outfits Tailor picks from for anyone set to a random Town outfit
* * add the outfit's pieces first: an outfit with no pieces in a situation pool can be picked at random
* * and dress someone in nothing
* *
* * required API version: 1.0
* *
* * @param: asOutfit, the outfit's name
* * @param: asCategory, the category's name as Tailor shows it
* *
* * @return: true when the outfit is now in the category, or already was; false when the outfit or the
* *          category doesn't exist, or Tailor can't change its outfit files this session
*/;
Bool Function AddCategory(String asOutfit, String asCategory) Global Native


; ███████╗██╗████████╗██╗   ██╗ █████╗ ████████╗██╗ ██████╗ ███╗   ██╗███████╗
; ██╔════╝██║╚══██╔══╝██║   ██║██╔══██╗╚══██╔══╝██║██╔═══██╗████╗  ██║██╔════╝
; ███████╗██║   ██║   ██║   ██║███████║   ██║   ██║██║   ██║██╔██╗ ██║███████╗
; ╚════██║██║   ██║   ██║   ██║██╔══██║   ██║   ██║██║   ██║██║╚██╗██║╚════██║
; ███████║██║   ██║   ╚██████╔╝██║  ██║   ██║   ██║╚██████╔╝██║ ╚████║███████║
; ╚══════╝╚═╝   ╚═╝    ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚═╝ ╚═════╝ ╚═╝  ╚═══╝╚══════╝

;/* GetSituation
* * the situation Tailor judges this person to be in right now, for anyone, whether Tailor dresses them
* * or not
* * in this order: "swimming" in water, "sleep" in bed, "warm" outdoors in cold weather or a snowy
* * region, "home" in a house (for the player, only their own houses), "town" in a city, town,
* * settlement, inn or other dwelling, and "adventuring" anywhere else
* * a fight is not a situation: it switches some people to their Adventuring outfit (see GetOutfit)
* * for the player this can differ from the situation dressing them: their outfit counts wading
* * knee-deep as water until they are fully out, and keeps their Sleep outfit on after they wake until
* * they walk away
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* *
* * @return: "adventuring", "town", "home", "sleep", "swimming" or "warm"; "" when akActor is None or a
* *          child
*/;
String Function GetSituation(Actor akActor) Global Native

;/* HasSituation
* * whether the person has their own Tailor outfit for a situation, one Tailor would put on them: a fixed
* * outfit they can wear, or Random with at least one outfit they can wear in that situation's pool (for
* * adventuring, one their armor type allows); that is when OverrideWithSituation dresses them in their
* * own choice for it, rather than falling back to a random outfit from the situation's pool
* * check it before dressing someone for a situation, so someone the player gave no outfit for it is left
* * as the player wants:
* *
* *     If Tailor.HasSituation(akBather, "swimming")
* *         Tailor.OverrideWithSituation(akBather, "swimming")
* *     EndIf
* *
* * where they are now doesn't matter (see GetSituation), and neither do a mod's override or situation
* * wigs; an outfit Tailor falls back to when a situation has none of its own doesn't count (at home it
* * dresses them in their town outfit, but that is not a home choice of their own); asking never picks
* * today's random outfit
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* * @param: asSituation, "adventuring", "town", "home", "sleep", "swimming" or "warm" (any capitals)
* *
* * @return: true when they have such an outfit; false when they have none for that situation (including
* *          when the outfit set for it no longer fits their sex, or for adventuring their armor type, or
* *          it is set to Random with no outfit they can wear in that situation's pool), asSituation is
* *          not a situation, or akActor is None or a child
*/;
Bool Function HasSituation(Actor akActor, String asSituation) Global Native


;  ██████╗ ██╗   ██╗███████╗██████╗ ██████╗ ██╗██████╗ ███████╗███████╗
; ██╔═══██╗██║   ██║██╔════╝██╔══██╗██╔══██╗██║██╔══██╗██╔════╝██╔════╝
; ██║   ██║██║   ██║█████╗  ██████╔╝██████╔╝██║██║  ██║█████╗  ███████╗
; ██║   ██║╚██╗ ██╔╝██╔══╝  ██╔══██╗██╔══██╗██║██║  ██║██╔══╝  ╚════██║
; ╚██████╔╝ ╚████╔╝ ███████╗██║  ██║██║  ██║██║██████╔╝███████╗███████║
;  ╚═════╝   ╚═══╝  ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝╚═════╝ ╚══════╝╚══════╝

;/* How an override works
* * an override dresses someone in a Tailor outfit until it is cleared
* * - anyone can have one: the player, or any NPC who is alive, not a child and not a creature, set up in
* *   Tailor or not
* * - one per person: the last override set wins, whichever mod set it
* * - it is kept in the save, and lasts through saving and loading until it is cleared
* * - while it is on, sleeping, swimming, the cold and changing location never replace it; a fight can:
* *   an NPC with an Adventuring outfit fights in it, and an NPC without one keeps the override; the
* *   player with situation outfits fights in their Adventuring outfit, or in their own gear without one,
* *   and the player without situation outfits keeps the override; in every case the override comes
* *   back when the fight ends
* * - it is cleared by ClearOutfitOverride, by the player confirming an outfit for that person in
* *   Tailor's screens or resetting them there, and by deleting the outfit it uses
* * - an outfit the person can't wear any more (the player changed its gender) is skipped, not cleared,
* *   until it fits them again
* * - a random pick (OverrideWithSituation, OverrideWithCategory) is made once, when it is set, and kept
* * - wigs don't change, and Tailor's Hide Weapons and Hide Helmets keep following the situation
* * - one set by OverrideWithSituation for "swimming" or "sleep" is that look while they wear it,
* *   wherever they are: their weapons, shields, quivers and torches are hidden as in Tailor's own
* *   Swimming and Sleep looks, and show in a fight or when drawn; one set for any other situation, or
* *   by OverrideWithOutfit or OverrideWithCategory, hides none itself, even in bed (Tailor's Hide
* *   Weapons setting still applies)
* * - it goes on at once, also on someone already in water: an NPC swimming in their own Tailor Swimming
* *   outfit changes into the override there, and keeps it when they leave the water
* * - setting one returns true when Tailor accepts it, and they are dressed from the next frame on,
* *   sometimes a little later; an NPC who isn't loaded is dressed when they load, and someone open in
* *   Tailor's outfit editor when it closes
*/;

;/* OverrideWithOutfit
* * dresses the person in this outfit until the override is cleared (see How an override works)
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* * @param: asOutfit, the outfit's name; it must fit the person's sex (GetOutfitGender)
* *
* * @return: true when accepted; false when akActor is None, dead, a child or a creature, there is no
* *          such outfit, or it doesn't fit them
*/;
Bool Function OverrideWithOutfit(Actor akActor, String asOutfit) Global Native

;/* OverrideWithSituation
* * dresses the person for a situation, wherever they are, until the override is cleared (see How an
* * override works): in their own Tailor choice for that situation (a fixed outfit, or today's random pick
* * when they are set to Random), or without one in a random outfit they can wear from that situation's
* * pool; the outfit is picked now and kept
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* * @param: asSituation, "adventuring", "town", "home", "sleep", "swimming" or "warm" (any capitals)
* *
* * @return: true when accepted; false when akActor is None, dead, a child or a creature, asSituation is
* *          not a situation, or there is no outfit for it they can wear
*/;
Bool Function OverrideWithSituation(Actor akActor, String asSituation) Global Native

;/* OverrideWithCategory
* * dresses the person in a random outfit they can wear from a category until the override is cleared
* * (see How an override works); the outfit is picked now and kept
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* * @param: asCategory, the category's name as Tailor shows it: an armor type, a situation pool or a
* *         custom category
* *
* * @return: true when accepted; false when akActor is None, dead, a child or a creature, the category
* *          doesn't exist, or it has no outfit they can wear
*/;
Bool Function OverrideWithCategory(Actor akActor, String asCategory) Global Native

;/* HasOutfitOverride
* * checks if the person has an override, set by any mod
* * true from the moment it is set, before the outfit is on, and while it is skipped because they can't
* * wear it
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* *
* * @return: true if they have an override; false if not, or akActor is None or a child
*/;
Bool Function HasOutfitOverride(Actor akActor) Global Native

;/* ClearOutfitOverride
* * ends the person's override, whichever mod set it, and dresses them as Tailor normally would
* * the player wears their situation or regular outfit, or their own gear when they have neither; an
* * NPC with an outfit assignment (a regular outfit or situation outfits) wears it; any other NPC, one
* * with only wigs in Tailor included, gets their own outfit back and keeps their wig
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* *
* * @return: true when they had an override and it is cleared (they are dressed from the next frame on,
* *          sometimes a little later); false when they had none, or akActor is None or a child
*/;
Bool Function ClearOutfitOverride(Actor akActor) Global Native


; ██╗   ██╗████████╗██╗██╗     ██╗████████╗██╗   ██╗
; ██║   ██║╚══██╔══╝██║██║     ██║╚══██╔══╝╚██╗ ██╔╝
; ██║   ██║   ██║   ██║██║     ██║   ██║    ╚████╔╝
; ██║   ██║   ██║   ██║██║     ██║   ██║     ╚██╔╝
; ╚██████╔╝   ██║   ██║███████╗██║   ██║      ██║
;  ╚═════╝    ╚═╝   ╚═╝╚══════╝╚═╝   ╚═╝      ╚═╝

;/* EvaluateOutfit
* * makes Tailor decide the person's outfit again and dress them, from the next frame on, as it does
* * when their situation changes; use it after changing something Tailor's choice depends on
* * works for the player, and for an NPC Tailor dresses (an outfit assignment or situation wigs) or one
* * with an override; an NPC who isn't loaded is dressed when they load
* *
* * required API version: 1.0
* *
* * @param: akActor, the person, the player or an NPC
* *
* * @return: true when accepted; false when akActor is None or a child, or the NPC has no override, no
* *          outfit assignment and no situation wigs (a Hair Dresser wig or hair color alone doesn't
* *          count)
*/;
Bool Function EvaluateOutfit(Actor akActor) Global Native
