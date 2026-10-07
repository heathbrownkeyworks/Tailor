# Tailor API

Optional runtime integration for other SKSE plugins. Built for menu managers and
hotkey frameworks that want to own the binding and drive Tailor's UI themselves.

**Requires Tailor 2.3.1 or newer.** Earlier versions export nothing.

The four UI exports below are the original API. Tailor 3.0 adds a mod API for
Papyrus scripts and SKSE plugins, to read and build outfits and to dress people:
see [Tailor 3.0: the mod API](#tailor-30-the-mod-api).

## What Tailor exports

```cpp
void OpenTailor();
void CloseTailor();
void SetTailorHotkeyEnabled(bool enabled);
bool IsTailorOpen();
```

Plain `extern "C"`, no name mangling, no calling-convention decoration. Resolve
them with `GetModuleHandleA("Tailor.dll")` and `GetProcAddress`.

## Quick start

Copy [`TailorAPI.h`](TailorAPI.h) into your project. There is nothing to link
and no build dependency on Tailor. Everything resolves at runtime, and every
call is a safe no-op when Tailor is not installed.

```cpp
#include "TailorAPI.h"

// Once, after plugins have loaded (kPostLoad or kDataLoaded both work):
if (TailorAPI::Load()) {
    TailorAPI::SetHotkeyEnabled(false);   // take over the binding
}

// From your own hotkey:
if (TailorAPI::IsOpen()) {
    TailorAPI::Close();
} else {
    TailorAPI::Open();
}
```

If you would rather not use the header, the raw form is:

```cpp
auto* tailor = GetModuleHandleA("Tailor.dll");
if (tailor) {
    auto open = reinterpret_cast<void(*)()>(GetProcAddress(tailor, "OpenTailor"));
    if (open) { open(); }
}
```

## Behaviour

**`OpenTailor()`** opens the UI on whatever NPC is under the player's crosshair,
exactly as the native hotkey does. If no NPC is targeted the UI opens on the
player. With a child under the crosshair it does not open: Tailor never handles
children, and shows the HUD message "Tailor can't be used on children." instead.

**`CloseTailor()`** closes the UI, reverts any in-progress outfit or wig preview,
and unfreezes the target NPC. Same teardown the native hotkey performs.

Both are **idempotent**. Calling `OpenTailor()` when it is already open is a
no-op, not a toggle. You never have to guard them.

Both are **asynchronous**. They are marshalled onto the game thread through the
SKSE task interface, so they return immediately and the UI changes on the next
frame. Do not expect `IsTailorOpen()` to flip the instant you call them.

Both are **thread safe**. Call them from your input handler, a worker thread,
wherever. Same for the other two.

**`SetTailorHotkeyEnabled(bool)`** turns Tailor's own configurable keyboard
hotkey (Shift+Z by default) on and off.

**`IsTailorOpen()`** reports whether the UI is currently open.

## Things worth knowing

**The hotkey flag is runtime only.** It is not written to `Tailor.ini` and it
resets to enabled every time the game starts. Call `SetHotkeyEnabled(false)`
on each load, not once.

**It does not disable the Tailor Lesser Power.** That is a separate opener the
user casts from their Favorites menu, and silently removing it would surprise
people who favorited it deliberately. Users who want it gone can set
`GrantPower=0` in `Data/SKSE/Plugins/Tailor.ini`.

**That is why `IsTailorOpen()` exists.** Because the Lesser Power can open Tailor
without going through this API, a flag you track yourself will drift out of sync.
Ask Tailor instead of remembering.

**`OpenTailor()` declines when Tailor cannot open.** The call is ignored while the
game is paused (any pausing menu, such as Horde's, is open), while the main or
loading menu is up, or before the player is loaded, rather than fighting over the
screen. With a child under the crosshair Tailor doesn't open either, and its HUD
message shows. Check `IsTailorOpen()` afterwards if you need to know whether it took.

**Everything degrades quietly.** If Tailor is missing, `Load()` returns false,
`IsOpen()` returns false, and the rest do nothing. No crash, no hard dependency,
nothing to declare in your requirements.

## Stability

These four names and signatures are fixed as of 2.3.1 and will not change.
Anything new will be added as additional functions rather than by altering
these, so integrating against them is safe.

Questions or requests for more surface area: open an issue or message ColdSun on
Nexus.

## Tailor 3.0: the mod API

Mods can read the player's Tailor outfits and categories, build outfits in
their library, ask which outfit someone wears and which situation they are in,
and dress anyone in a Tailor outfit until the mod lets go (an override). The
work happens in Tailor's DLL, and there are two ways in, over the same code:

- **Papyrus:** the script `Tailor`, 26 `Global Native` functions. No ESP, no
  master and nothing to attach.
- **C++:** `ITailorInterface1` for SKSE plugins, the same functions through
  `RequestInterface1()`, plus a listener for the change events.

**Requires Tailor 3.0 or newer.** The API version is **1.0**, and each function
names the API version it needs. The Papyrus reference below says what the
documentation inside [`Tailor.psc`](../../papyrus/Source/Scripts/Tailor.psc)
says, and the second half of [`TailorAPI.h`](TailorAPI.h) documents the C++
functions in the same words.

### Using it from Papyrus

**Install.** Tailor ships `Scripts/Tailor.pex`, which the game runs, and the
source your scripts compile against in both places compilers look:
`Source/Scripts/Tailor.psc` (the Skyrim SE Creation Kit's `Data\Source\Scripts`)
and `Scripts/Source/Tailor.psc` (the Skyrim LE layout, which SKSE64 uses too).
Put either folder on the compiler's import path. Don't ship a `Tailor.pex` of
your own. There is nothing to attach or fill: call each function on the script
by name, `Tailor.FunctionName(...)`.

Without Tailor installed a call fails and Papyrus logs it, so check the version
before anything else:

```papyrus
If Tailor.GetApiVersion() >= 1.0
    ; Tailor is there
EndIf
```

**Try it.** [`tools/api-test/TailorApiTest.psc`](../../tools/api-test/TailorApiTest.psc)
is a quest script that calls every function on the player and one NPC and logs
each result; its header says how to set it up. It leaves the outfit
`TailorApiTest Outfit` and the category `TailorApiTest Category` in the player's
shared library (the API cannot delete them, so delete them in Tailor's screens
afterwards), and each run clears any override on the player and the test NPC.

### Rules every function shares

- **Names.** Outfits and categories go by the names Tailor's screens show,
  ignoring capitals and surrounding spaces: `" steel plate"` finds
  `"Steel Plate"` (Papyrus compares strings ignoring capitals too).
- **Names are UTF-8.** In an outfit or category name you pass, a byte that
  isn't valid UTF-8 becomes "?", for creating and for finding alike, so save
  your scripts as UTF-8.
- **Unique names.** Outfit names are unique, ignoring capitals; so are the
  names of new categories. Outfits that shared a name before Tailor 3.0 are
  numbered once, the first time Tailor reads the whole file: the first keeps
  its name and the others become "Name (2)", "Name (3)".
- **Categories.** The armor types (Clothing, Light Armor, Heavy Armor), the
  situation pools (Adventuring, Town, Home, Sleep, Swimming, Warm) and the
  player's custom categories. A custom category with the same name as another
  shows in Tailor as "Warm (2)", and that is its name here as well.
- **Gender.** An outfit is unisex (-1), male (0) or female (1). The `aiGender`
  filter of the list functions takes -1 for every outfit, 0 for what a man can
  wear and 1 for what a woman can wear (both with the unisex outfits); any other
  value matches no outfit.
- **Situations** are named `"adventuring"`, `"town"`, `"home"`, `"sleep"`,
  `"swimming"` and `"warm"` (any capitals).
- **Slots** are the body slot numbers 30-61, as the Creation Kit shows them
  (32 body, 33 hands, 37 feet).
- **Failures.** A missing outfit, category or form, or a None actor or a child
  (Tailor never handles children), makes a function return `""`, -2, 0,
  false or an empty array (each function says which), and Tailor's log
  (`SKSE\Tailor.log`) says why in one line.
- **The library is shared.** The outfit library (the outfits and categories,
  and everything the building functions change) is shared by every save and
  every character, in Tailor's JSON files. Overrides are kept per save.
- **A file Tailor couldn't read.** When Tailor couldn't fully read its outfit
  files at startup (`outfits.json` or `library.json`: a broken line, say), it
  changes neither until it is restarted with the file fixed: the building
  functions return false and change nothing, and Tailor's log says why.
- **A file Tailor couldn't write.** A change Tailor accepted but couldn't write
  to its files (another program holding one, a full disk) holds for this
  session and is written with the next save that succeeds, and Tailor's log
  says so; `CreateOutfit` and `CreateCustomCategory` instead take back what
  they made and return false.
- **Who Tailor dresses.** Tailor dresses an NPC who has an outfit assignment in
  Tailor (a regular outfit or situation outfits) or situation wigs; a Hair
  Dresser wig or hair color alone doesn't count.
- **Dressing is asynchronous.** The functions that dress someone (the
  overrides, `ClearOutfitOverride`, `EvaluateOutfit`, and adding or removing a
  piece of an outfit someone wears) return true when Tailor accepts the
  request. The outfit goes on from the next frame on, sometimes a little later,
  and `GetOutfit` can name the previous outfit until then.

### The functions at a glance

| Group | Function | What it does |
|---|---|---|
| Version | `GetApiVersion` | The API version, 1.0 in Tailor 3.0. |
| Outfits | `DoesOutfitExist` | Whether the player's library has an outfit of this name. |
| | `GetOutfit` | The Tailor outfit someone wears now. |
| | `GetOutfitGender` | Who an outfit is for: unisex, male or female. |
| | `GetOutfitArmors` | An outfit's pieces as Armor forms. |
| | `OutfitHasKeyword` | Whether any piece of an outfit carries a keyword. |
| | `OutfitUsesSlot` | Whether any piece of an outfit covers a body slot. |
| | `IsOutfitInCategory` | Whether an outfit is in a category. |
| Lists | `DoesCategoryExist` | Whether Tailor shows a category under this name. |
| | `GetOutfitsByCategory` | The outfit names in a category, optionally by gender. |
| | `GetOutfitCount` | How many outfits a category has, optionally by gender. |
| | `GetAdventuringOutfits` | The Adventuring pool, optionally by armor type and gender. |
| | `GetOutfitsByArmorKeyword` | Every outfit with a piece that carries a keyword. |
| Building | `CreateOutfit` | Adds an empty outfit to the player's library. |
| | `AddArmorToOutfit` | Adds a piece to an outfit, and re-dresses its wearers. |
| | `RemoveArmorFromOutfit` | Removes a piece from an outfit, and re-dresses its wearers. |
| | `CreateCustomCategory` | Adds a custom category to the player's library. |
| | `AddCategory` | Puts an outfit in a category. |
| Situations | `GetSituation` | The situation Tailor judges someone to be in now. |
| | `HasSituation` | Whether someone has their own Tailor outfit for a situation. |
| Overrides | `OverrideWithOutfit` | Dresses someone in an outfit until the override is cleared. |
| | `OverrideWithSituation` | The same, in the outfit for a situation (for Swimming or Sleep, weapons hidden too). |
| | `OverrideWithCategory` | The same, in a random outfit from a category. |
| | `HasOutfitOverride` | Whether someone has an override. |
| | `ClearOutfitOverride` | Ends someone's override. |
| Utility | `EvaluateOutfit` | Makes Tailor decide someone's outfit again and dress them. |

### Version

#### GetApiVersion

```papyrus
Float Function GetApiVersion() Global Native
```

The version of this API, 1.0 in Tailor 3.0; each function below names the version it needs. Check it once before using the API: without Tailor installed the call fails and Papyrus logs it.

- **Needs:** API version 1.0
- **Returns:** the API version, 1.0

### Outfits

#### DoesOutfitExist

```papyrus
Bool Function DoesOutfitExist(String asOutfit) Global Native
```

Checks if the player's Tailor library has an outfit of this name. The player can rename or delete any outfit, your mod's own included: check before you use one, and create it again when it is gone.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- **Returns:** true if the outfit exists, false if not (an empty name is never an outfit)

#### GetOutfit

```papyrus
String Function GetOutfit(Actor akActor) Global Native
```

The Tailor outfit this person wears now. For the player, the outfit Tailor has on them; for an NPC with an override or situation outfits, the outfit Tailor last put on them (during a fight, their Adventuring outfit if they have one); for an NPC with only a regular outfit, or one Tailor hasn't put an outfit on yet (one not loaded), their override if they can wear it, or else their regular outfit if they have no situation outfits and can wear it.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- **Returns:** the outfit's name; "" when akActor is None or a child, when they wear their own gear or anything Tailor didn't put on, and for an NPC Tailor hasn't put an outfit on yet who has situation outfits (or no regular outfit they can wear) and no override they can wear

#### GetOutfitGender

```papyrus
Int Function GetOutfitGender(String asOutfit) Global Native
```

Who the outfit is for, as set in Tailor; an NPC is only ever dressed in outfits that fit their sex.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- **Returns:** -1 unisex, 0 male, 1 female; -2 when there is no such outfit

#### GetOutfitArmors

```papyrus
Armor[] Function GetOutfitArmors(String asOutfit) Global Native
```

The outfit's pieces as Armor forms, in the outfit's order. A piece whose plugin is not loaded is left out.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- **Returns:** the pieces; an empty array when there is no such outfit or none of its pieces is loaded

#### OutfitHasKeyword

```papyrus
Bool Function OutfitHasKeyword(String asOutfit, Keyword akKeyword) Global Native
```

Checks if any piece of the outfit carries the keyword. Pass your own Keyword property: any keyword from any plugin works (ArmorClothing, ArmorHelmet, a DLC or mod keyword), and your plugin needs no master for Tailor.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- `akKeyword`: the keyword to look for
- **Returns:** true if a loaded piece has the keyword; false if none has it, there is no such outfit, or akKeyword is None

#### OutfitUsesSlot

```papyrus
Bool Function OutfitUsesSlot(String asOutfit, Int aiSlot) Global Native
```

Checks if any piece of the outfit covers the body slot.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- `aiSlot`: the body slot number, 30-61 (30 head, 32 body, 33 hands, 37 feet, 52 for many mods)
- **Returns:** true if a loaded piece covers the slot; false if none does, there is no such outfit, or aiSlot is outside 30-61

#### IsOutfitInCategory

```papyrus
Bool Function IsOutfitInCategory(String asOutfit, String asCategory) Global Native
```

Checks if the outfit is in the category.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- `asCategory`: the category's name as Tailor shows it ("Clothing", "Town", a custom one)
- **Returns:** true if both exist and the outfit is in the category; false otherwise

### Lists

#### DoesCategoryExist

```papyrus
Bool Function DoesCategoryExist(String asCategory) Global Native
```

Checks if Tailor shows a category under this name.

- **Needs:** API version 1.0
- `asCategory`: the category's name as Tailor shows it
- **Returns:** true if the category exists, false if not (an empty name is never a category)

#### GetOutfitsByCategory

```papyrus
String[] Function GetOutfitsByCategory(String asCategory, Int aiGender = -1) Global Native
```

The names of the outfits in a category, optionally only those a man or a woman can wear.

- **Needs:** API version 1.0
- `asCategory`: the category's name as Tailor shows it
- `aiGender`: -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex outfits)
- **Returns:** the outfit names; an empty array when the category doesn't exist, has no outfit that passes the filter, or aiGender is not -1, 0 or 1

#### GetOutfitCount

```papyrus
Int Function GetOutfitCount(String asCategory, Int aiGender = -1) Global Native
```

How many outfits in a category pass the gender filter: the length GetOutfitsByCategory would return.

- **Needs:** API version 1.0
- `asCategory`: the category's name as Tailor shows it
- `aiGender`: -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex outfits)
- **Returns:** the count; 0 when the category doesn't exist, has no outfit that passes the filter, or aiGender is not -1, 0 or 1

#### GetAdventuringOutfits

```papyrus
String[] Function GetAdventuringOutfits(String asArmorType = "", Int aiGender = -1) Global Native
```

The outfits in the Adventuring pool, optionally only those also in an armor-type category. With an armor type this is exactly the outfits in both the Adventuring pool and that type's category (Clothing, Light Armor or Heavy Armor); when none is in both, the array is empty (Tailor's own dressing falls back to the whole pool then, but this list does not).

- **Needs:** API version 1.0
- `asArmorType`: "clothing", "light" or "heavy" (any capitals), or "" for the whole pool (default)
- `aiGender`: -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex outfits)
- **Returns:** the outfit names; an empty array when none passes, asArmorType is not one of the three or "", or aiGender is not -1, 0 or 1

#### GetOutfitsByArmorKeyword

```papyrus
String[] Function GetOutfitsByArmorKeyword(Keyword akKeyword, Int aiGender = -1) Global Native
```

Every outfit in the library with a piece that carries the keyword. Pass your own Keyword property: any keyword from any plugin works.

- **Needs:** API version 1.0
- `akKeyword`: the keyword to look for (ArmorHeavy, ArmorClothing, a DLC or mod keyword)
- `aiGender`: -1 every outfit (default); 0 what a man can wear; 1 what a woman can wear (0 and 1 include the unisex outfits)
- **Returns:** the outfit names; an empty array when no outfit passes, akKeyword is None, or aiGender is not -1, 0 or 1

### Building

Outfits and categories a mod builds go into the player's library like any other. The player can wear, edit, rename or delete them.

#### CreateOutfit

```papyrus
Bool Function CreateOutfit(String asOutfit, String asCategory = "", Int aiGender = -1) Global Native
```

Adds a new outfit with no pieces to the player's Tailor library; add its pieces with AddArmorToOutfit. It belongs to the player like any other outfit: they can wear it, edit it, rename it or delete it. The library is shared by every save and every character (Tailor's JSON files), so the outfit is there in every save once created: check DoesOutfitExist before creating it. Add the pieces before you put it in a category: an outfit with no pieces in a situation pool can be picked at random and dress someone in nothing, so for a pool create it with no category, add the pieces, then call AddCategory.

- **Needs:** API version 1.0
- `asOutfit`: the new outfit's name; surrounding spaces are dropped, and no other outfit may have it (capitals ignored)
- `asCategory`: a category to put it in, which must exist; "" or only spaces for none (default)
- `aiGender`: who it is for: -1 unisex (default), 0 male, 1 female
- **Returns:** true when it was created; false when the name is empty or taken, the category doesn't exist, aiGender is not -1, 0 or 1, Tailor can't change its outfit files this session, or the new outfit couldn't be saved (then Tailor takes it back for this session; Tailor.log says if a file still lists it)

#### AddArmorToOutfit

```papyrus
Bool Function AddArmorToOutfit(String asOutfit, Armor akArmor) Global Native
```

Adds a piece to the outfit, and dresses everyone wearing it again so they show it. The armor must come from a plugin: one the game created while running (an armor the player enchanted, for one) has no plugin, and is refused.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- `akArmor`: the piece to add
- **Returns:** true when it was added, or the outfit already has it (then nothing changes); false when there is no such outfit, akArmor is None or was created while the game ran, or Tailor can't change its outfit files this session

#### RemoveArmorFromOutfit

```papyrus
Bool Function RemoveArmorFromOutfit(String asOutfit, Armor akArmor) Global Native
```

Removes a piece from the outfit, and dresses everyone wearing it again. Removing the last piece leaves the outfit empty; it is not deleted, and in a situation pool it can still be picked at random and dress someone in nothing.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- `akArmor`: the piece to remove
- **Returns:** true when it was removed; false when there is no such outfit, the outfit doesn't have the piece, akArmor is None or was created while the game ran, or Tailor can't change its outfit files this session

#### CreateCustomCategory

```papyrus
Bool Function CreateCustomCategory(String asCategory) Global Native
```

Adds a custom category to the player's Tailor library, which the player can rename or delete.

- **Needs:** API version 1.0
- `asCategory`: the new category's name; surrounding spaces are dropped, and no other category may have it (capitals ignored; the built-in names such as "Warm" are taken)
- **Returns:** true when it was created; false when the name is empty or taken, Tailor can't change its outfit files this session, or the new category couldn't be saved (then Tailor takes it back for this session; Tailor.log says if a file still lists it)

#### AddCategory

```papyrus
Bool Function AddCategory(String asOutfit, String asCategory) Global Native
```

Puts the outfit in a category, which can be any category, a situation pool included: an outfit in the Town pool is one of the outfits Tailor picks from for anyone set to a random Town outfit. Add the outfit's pieces first: an outfit with no pieces in a situation pool can be picked at random and dress someone in nothing.

- **Needs:** API version 1.0
- `asOutfit`: the outfit's name
- `asCategory`: the category's name as Tailor shows it
- **Returns:** true when the outfit is now in the category, or already was; false when the outfit or the category doesn't exist, or Tailor can't change its outfit files this session

### Situations

#### GetSituation

```papyrus
String Function GetSituation(Actor akActor) Global Native
```

The situation Tailor judges this person to be in right now, for anyone, whether Tailor dresses them or not. In this order: "swimming" in water, "sleep" in bed, "warm" outdoors in cold weather or a snowy region, "home" in a house (for the player, only their own houses), "town" in a city, town, settlement, inn or other dwelling, and "adventuring" anywhere else. A fight is not a situation: it switches some people to their Adventuring outfit (see GetOutfit). For the player this can differ from the situation dressing them: their outfit counts wading knee-deep as water until they are fully out, and keeps their Sleep outfit on after they wake until they walk away.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- **Returns:** "adventuring", "town", "home", "sleep", "swimming" or "warm"; "" when akActor is None or a child

#### HasSituation

```papyrus
Bool Function HasSituation(Actor akActor, String asSituation) Global Native
```

Whether the person has their own Tailor outfit for a situation, one Tailor would put on them: a fixed outfit they can wear, or Random with at least one outfit they can wear in that situation's pool (for adventuring, one their armor type allows); that is when OverrideWithSituation dresses them in their own choice for it, rather than falling back to a random outfit from the situation's pool. Check it before dressing someone for a situation, so someone the player gave no outfit for it is left as the player wants:

```papyrus
If Tailor.HasSituation(akBather, "swimming")
    Tailor.OverrideWithSituation(akBather, "swimming")
EndIf
```

Where they are now doesn't matter (see GetSituation), and neither do a mod's override or situation wigs; an outfit Tailor falls back to when a situation has none of its own doesn't count (at home it dresses them in their town outfit, but that is not a home choice of their own); asking never picks today's random outfit.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- `asSituation`: "adventuring", "town", "home", "sleep", "swimming" or "warm" (any capitals)
- **Returns:** true when they have such an outfit; false when they have none for that situation (including when the outfit set for it no longer fits their sex, or for adventuring their armor type, or it is set to Random with no outfit they can wear in that situation's pool), asSituation is not a situation, or akActor is None or a child

### Overrides

#### How an override works

An override dresses someone in a Tailor outfit until it is cleared.

- Anyone can have one: the player, or any NPC who is alive, not a child and not a creature, set up in Tailor or not.
- One per person: the last override set wins, whichever mod set it.
- It is kept in the save, and lasts through saving and loading until it is cleared.
- While it is on, sleeping, swimming, the cold and changing location never replace it; a fight can: an NPC with an Adventuring outfit fights in it, and an NPC without one keeps the override; the player with situation outfits fights in their Adventuring outfit, or in their own gear without one, and the player without situation outfits keeps the override; in every case the override comes back when the fight ends.
- It is cleared by `ClearOutfitOverride`, by the player confirming an outfit for that person in Tailor's screens or resetting them there, and by deleting the outfit it uses.
- An outfit the person can't wear any more (the player changed its gender) is skipped, not cleared, until it fits them again.
- A random pick (`OverrideWithSituation`, `OverrideWithCategory`) is made once, when it is set, and kept.
- Wigs don't change, and Tailor's Hide Weapons and Hide Helmets keep following the situation.
- One set by `OverrideWithSituation` for "swimming" or "sleep" is that look while they wear it, wherever they are: their weapons, shields, quivers and torches are hidden as in Tailor's own Swimming and Sleep looks, and show in a fight or when drawn. One set for any other situation, or by `OverrideWithOutfit` or `OverrideWithCategory`, hides none itself, even in bed (Tailor's Hide Weapons setting still applies).
- It goes on at once, also on someone already in water: an NPC swimming in their own Tailor Swimming outfit changes into the override there, and keeps it when they leave the water.
- Setting one returns true when Tailor accepts it, and they are dressed from the next frame on, sometimes a little later; an NPC who isn't loaded is dressed when they load, and someone open in Tailor's outfit editor when it closes.

#### OverrideWithOutfit

```papyrus
Bool Function OverrideWithOutfit(Actor akActor, String asOutfit) Global Native
```

Dresses the person in this outfit until the override is cleared (see How an override works).

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- `asOutfit`: the outfit's name; it must fit the person's sex (GetOutfitGender)
- **Returns:** true when accepted; false when akActor is None, dead, a child or a creature, there is no such outfit, or it doesn't fit them

#### OverrideWithSituation

```papyrus
Bool Function OverrideWithSituation(Actor akActor, String asSituation) Global Native
```

Dresses the person for a situation, wherever they are, until the override is cleared (see How an override works): in their own Tailor choice for that situation (a fixed outfit, or today's random pick when they are set to Random), or without one in a random outfit they can wear from that situation's pool; the outfit is picked now and kept.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- `asSituation`: "adventuring", "town", "home", "sleep", "swimming" or "warm" (any capitals)
- **Returns:** true when accepted; false when akActor is None, dead, a child or a creature, asSituation is not a situation, or there is no outfit for it they can wear

#### OverrideWithCategory

```papyrus
Bool Function OverrideWithCategory(Actor akActor, String asCategory) Global Native
```

Dresses the person in a random outfit they can wear from a category until the override is cleared (see How an override works); the outfit is picked now and kept.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- `asCategory`: the category's name as Tailor shows it: an armor type, a situation pool or a custom category
- **Returns:** true when accepted; false when akActor is None, dead, a child or a creature, the category doesn't exist, or it has no outfit they can wear

#### HasOutfitOverride

```papyrus
Bool Function HasOutfitOverride(Actor akActor) Global Native
```

Checks if the person has an override, set by any mod. True from the moment it is set, before the outfit is on, and while it is skipped because they can't wear it.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- **Returns:** true if they have an override; false if not, or akActor is None or a child

#### ClearOutfitOverride

```papyrus
Bool Function ClearOutfitOverride(Actor akActor) Global Native
```

Ends the person's override, whichever mod set it, and dresses them as Tailor normally would. The player wears their situation or regular outfit, or their own gear when they have neither; an NPC with an outfit assignment (a regular outfit or situation outfits) wears it; any other NPC, one with only wigs in Tailor included, gets their own outfit back and keeps their wig.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- **Returns:** true when they had an override and it is cleared (they are dressed from the next frame on, sometimes a little later); false when they had none, or akActor is None or a child

### Utility

#### EvaluateOutfit

```papyrus
Bool Function EvaluateOutfit(Actor akActor) Global Native
```

Makes Tailor decide the person's outfit again and dress them, from the next frame on, as it does when their situation changes; use it after changing something Tailor's choice depends on. Works for the player, and for an NPC Tailor dresses (an outfit assignment or situation wigs) or one with an override; an NPC who isn't loaded is dressed when they load.

- **Needs:** API version 1.0
- `akActor`: the person, the player or an NPC
- **Returns:** true when accepted; false when akActor is None or a child, or the NPC has no override, no outfit assignment and no situation wigs (a Hair Dresser wig or hair color alone doesn't count)

### Events

Tailor sends two SKSE mod events. Register for them with `RegisterForModEvent`
on any form, alias or magic effect script, and register again after every game
load, since SKSE forgets registrations (for example from `OnInit` and from a
player alias's `OnPlayerLoadGame`):

```papyrus
RegisterForModEvent("Tailor_OnOutfitChanged", "OnTailorOutfitChanged")
RegisterForModEvent("Tailor_OnSituationChanged", "OnTailorSituationChanged")

Event OnTailorOutfitChanged(String asEventName, String asOutfit, Float afNumArg, Form akSender)
    Actor akWearer = akSender as Actor
EndEvent

Event OnTailorSituationChanged(String asEventName, String asSituation, Float afNumArg, Form akSender)
    Actor akPerson = akSender as Actor
EndEvent
```

| Event | String argument | Sent when |
|---|---|---|
| `Tailor_OnOutfitChanged` | the Tailor outfit the person wears now, as `GetOutfit` names it, or "" when they wear none (their own gear) | the Tailor outfit someone wears changes |
| `Tailor_OnSituationChanged` | the person's new situation, as `GetSituation` names it (for the player that can differ from the situation dressing them; see `GetSituation`) | someone's situation changes |

- `akSender` is the actor the event is about (cast it with `as Actor`); `afNumArg` is always 0.
- Tailor checks for changes about every 250 ms, and an event is sent by the first check after the change. No check runs while Tailor's menu is open, the game is paused, or the main menu or a loading screen is up, and a change made then is sent by the first check after, if it still holds.
- No event is sent for the first sighting of someone after a load or a new game. An outfit Tailor is still putting back on (an NPC's situation outfit after a load, for one) is not known until it is on, and then sends an event only if it differs from the last outfit known since the load, so after a load it counts as a first sighting too.
- An override set on someone Tailor isn't watching yet (an NPC it doesn't dress, say) sends `Tailor_OnOutfitChanged` at the first check after it is set, naming the override as `GetOutfit` does: the outfit they wore when it was set counts as the last one known, unless Tailor was still putting that one back on.
- Tailor watches the player, every NPC it dresses or has an override for, and anyone it has watched since the last load or new game (until the game drops them from memory), while they are loaded and alive, and never a child; so an event is still sent when Tailor stops dressing someone or their override ends.

### Papyrus examples

A trigger box around a bath house dresses whoever walks in for swimming, and
gives them their usual outfit back when they leave. It may cover the water
itself: an override goes on at once, in water too.

```papyrus
Scriptname MyBathTrigger extends ObjectReference

Event OnTriggerEnter(ObjectReference akActionRef)
    Actor akBather = akActionRef as Actor
    If akBather
        Tailor.OverrideWithSituation(akBather, "swimming")
    EndIf
EndEvent

Event OnTriggerLeave(ObjectReference akActionRef)
    Actor akBather = akActionRef as Actor
    If akBather
        Tailor.ClearOutfitOverride(akBather)
    EndIf
EndEvent
```

Building an outfit once. The library is shared by every save and the player can
delete your outfit, so check for it each time a game loads and create it again
when it is gone:

```papyrus
Scriptname MyOutfitBuilder extends ReferenceAlias   ; on the player alias of your quest

Armor Property MyCuirass Auto
Armor Property MyBoots Auto

Event OnInit()
    BuildOutfit()
EndEvent

Event OnPlayerLoadGame()
    BuildOutfit()
EndEvent

Function BuildOutfit()
    If Tailor.GetApiVersion() < 1.0
        Return
    EndIf
    If !Tailor.DoesOutfitExist("Rogue Disguise")
        ; a female outfit for the Town pool: the pieces first, then the pool
        If Tailor.CreateOutfit("Rogue Disguise", "", 1)
            Tailor.AddArmorToOutfit("Rogue Disguise", MyCuirass)
            Tailor.AddArmorToOutfit("Rogue Disguise", MyBoots)
            Tailor.AddCategory("Rogue Disguise", "Town")
        EndIf
    EndIf
EndFunction
```

Reacting to an outfit change:

```papyrus
Scriptname MyOutfitWatcher extends ReferenceAlias   ; on the player alias of your quest

Event OnInit()
    RegisterForTailor()
EndEvent

Event OnPlayerLoadGame()
    RegisterForTailor()
EndEvent

Function RegisterForTailor()
    RegisterForModEvent("Tailor_OnOutfitChanged", "OnTailorOutfitChanged")
EndFunction

Event OnTailorOutfitChanged(String asEventName, String asOutfit, Float afNumArg, Form akSender)
    Actor akWearer = akSender as Actor
    If akWearer == Game.GetPlayer() && asOutfit == "Rogue Disguise"
        Debug.Notification("You are in disguise.")
    EndIf
EndEvent
```

### Using it from C++

For SKSE plugins, Tailor exports `RequestTailorInterface(std::uint32_t version)`,
which gives `ITailorInterface1*` for version 1 and nullptr for any other. The
header wraps it:

```cpp
#include "TailorAPI.h"

class MyListener final : public TailorAPI::ITailorListener1
{
public:
    void OnOutfitChanged(RE::Actor* actor, const char* outfit) override
    {
        // outfit is valid until this call returns: copy it to keep it
    }

    void OnSituationChanged(RE::Actor* actor, const char* situation) override {}
};

MyListener g_listener;
TailorAPI::ITailorInterface1* g_tailor = nullptr;

void OnSKSEMessage(SKSE::MessagingInterface::Message* message)
{
    switch (message->type) {
    case SKSE::MessagingInterface::kPostLoad:
        // nullptr when Tailor is missing or older than 3.0
        g_tailor = TailorAPI::RequestInterface1();
        if (g_tailor) {
            g_tailor->AddListener(&g_listener);
        }
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        // Tailor has loaded its library by now, so the building functions work; add the pieces
        // (AddArmorToOutfit) before putting the outfit in a pool (AddCategory)
        if (g_tailor && !g_tailor->DoesOutfitExist("Rogue Disguise")) {
            g_tailor->CreateOutfit("Rogue Disguise", "", 1);
        }
        break;
    }
}

void UseIt(RE::Actor* actor)
{
    // A list comes back through a callback, once for each item, before the function returns
    std::vector<std::string> townOutfits;
    g_tailor->GetOutfitsByCategory("Town", -1,
        [](const char* name, void* context) { static_cast<std::vector<std::string>*>(context)->emplace_back(name); },
        &townOutfits);

    // A function that takes an actor runs on the game thread, in an SKSE task for example
    SKSE::GetTaskInterface()->AddTask([handle = actor->GetHandle()] {
        if (const auto person = handle.get()) {
            g_tailor->OverrideWithSituation(person.get(), "swimming");
        }
    });
}
```

- **The functions are the Papyrus ones**, with the same names, the same rules
  and the same return values; `TailorAPI.h` documents each in the same words.
  Strings are UTF-8 `const char*` (nullptr counts as ""), forms are
  `RE::Actor*`, `RE::TESObjectARMO*` and `RE::BGSKeyword*`, and a parameter that
  has a default in Papyrus has none in C++: pass -1 for a gender and `""` for
  no category or armor type.
- **Getting the interface.** Call `RequestInterface1()` once every plugin is
  loaded (SKSE's `kPostLoad` message or later) and keep the pointer for the
  session. `GetApiVersion`, `AddListener` and `RemoveListener` work from then
  on. Tailor reads its library while it handles `kDataLoaded`, so call the other
  functions after that: from `kPostLoadGame` or `kNewGame` on, or in an SKSE task
  queued from your `kDataLoaded` handler. Before then the queries see an empty
  library, and the building functions (`CreateOutfit`, `AddArmorToOutfit`,
  `RemoveArmorFromOutfit`, `CreateCustomCategory`, `AddCategory`) return false
  and change nothing. Only an SKSE plugin can call that early.
- **Threads.** The functions that take an `RE::Actor*` (`GetOutfit`,
  `GetSituation`, `HasSituation`, the three `OverrideWith` functions,
  `HasOutfitOverride`, `ClearOutfitOverride` and `EvaluateOutfit`), the
  building functions (`CreateOutfit`, `AddArmorToOutfit`,
  `RemoveArmorFromOutfit`, `CreateCustomCategory` and `AddCategory`),
  `AddListener` and `RemoveListener` must be called on the game thread, in an
  SKSE task
  (`SKSE::GetTaskInterface()->AddTask`) for example: the building functions
  change the library, which Tailor's own dressing uses on the game thread. The
  others, the queries, read Tailor's library under its locks and may be called
  from any thread. Papyrus scripts need not care: the VM runs every `Tailor`
  function in step with the game thread.
- **Callbacks.** A string or list comes back through your callback
  (`void (*)(const char* value, void* context)`, or the same with an
  `RE::TESObjectARMO*` for `GetOutfitArmors`), called once for each item before
  the function returns and not at all for an empty list. `context` is any
  pointer of yours, handed back unchanged. A string Tailor passes you is valid
  until the callback returns: copy it to keep it. No STL type crosses between
  the DLLs. `GetOutfit` and `GetSituation` call the callback once and return
  true; for a nullptr actor or a child they return false without calling it.
- **Listeners.** `AddListener` tells your `ITailorListener1` about the same
  changes as the Papyrus events (the rules under Events apply: about every
  250 ms, nothing while Tailor's menu is open or the game is paused, no first
  sighting after a load). Its functions are called on the game thread, so they
  may call any function of the interface. The listener stays registered,
  through loads, until `RemoveListener`, and must outlive its registration:
  call `RemoveListener` before destroying it. One removed from inside a
  listener's function is not called again. Adding one twice, or nullptr, does
  nothing.
- **Version 1 is frozen** once released. Later additions come as a new interface
  version, and `GetApiVersion()` tells which API version the installed Tailor
  has.

## License

`TailorAPI.h` is available under the MIT License in
[`../../licenses/TailorAPI-MIT.txt`](../../licenses/TailorAPI-MIT.txt). This
permissive license applies only to the runtime-loading header. Tailor's native
implementation and `Tailor.dll` remain GPL-3.0-or-later with the exceptions
described in [`../../LICENSING.md`](../../LICENSING.md).
