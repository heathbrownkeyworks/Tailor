# Tailor

Tailor is an SKSE plugin for managing outfits, wigs, hair colors and situation-based looks in Skyrim Special
Edition and Anniversary Edition, for NPCs and for the player.

Current source version: **3.0.1**

## Changes in 3.0.1

- Names in Russian, Greek, Chinese, Japanese, Korean and other languages show their own letters instead of question
  marks. Letters Tailor's fonts don't have are drawn with Windows' own fonts.
- Lists sort alphabetically by your Windows language, ignoring capitals in any alphabet.
- Search ignores capitals in any alphabet.
- Outfit and category names are unique ignoring capitals in any alphabet. Two names that differ only in capitals, such
  as "Мантия" and "мантия", are renamed once when Tailor loads: the second becomes "мантия (2)".

## Changes in 3.0.0

- New native interface drawn inside the game with Dear ImGui. Tailor no longer needs a UI framework mod.
- The player can be dressed too: outfits, wigs, hair color and situation outfits. Tailor targets the NPC under
  the crosshair, or the player when there is none. The switch at the bottom of the screen changes between them.
- New Swimming and Warm outfit situations. Warm applies outdoors in cloudy, rainy or snowy weather and in snowy
  regions. Neither one changes the wig.
- Home now applies to NPCs in any house, not only player homes. Without a Home outfit or wig, Home uses Town's.
- Outfits are tagged Male, Female or Unisex. An NPC is only given outfits that fit them.
- A Settings page with three switches: Disable Tailor Favorite, Hide Weapons and Hide Helmets.
- Weapons, shields, quivers and torches are hidden while someone wears their Sleep or Swimming look.
- A mod API for other mods, from Papyrus and from C++. See [docs/api/README.md](docs/api/README.md).
- Outfit names and category names are unique, ignoring capitals.
- 300 hair colors in 14 families, plus your own custom colors.
- Deleting an outfit gives every NPC who wore it their own outfit back.
- Tailor never dresses children and does not open on one.
- Tailor's files are saved through a temporary file, and a file Tailor can't read in full is never overwritten.
- Lists sort by name ignoring capitals.
- No controller button combination opens Tailor anymore. On a controller, use the Tailor power from Favorites.
- Tailor is now licensed under GPL-3.0-or-later. See [License](#license).

## Features

- Create outfits from armor records already loaded in the game, and sort them into categories. An outfit can be in
  several categories.
- Preview outfits and wigs on the actual NPC, live in the game world, before you assign them.
- Cycle through a category on an NPC and assign the outfit you like.
- Assign fixed or randomized outfits for Adventuring, Town, Home, Sleep, Swimming and Warm, and wigs for
  Adventuring, Town, Home and Sleep.
- Choose an armor type (clothing, light or heavy) for each NPC's Adventuring outfits.
- Keep assigned outfit pieces in Skyrim's hidden outfit inventory instead of the NPC's trade inventory.
- Reset Outfit gives an NPC back the default outfit from their own plugin.
- Add wigs from installed mods, assign them, and color hair.
- Share outfits with other players through JSON files.
- Hide plugins you don't want in the outfit and wig lists.
- Coordinates outfit changes with OBody NG so NPCs keep their bodies.
- Borrows the camera through SmoothCam's API when SmoothCam is installed.
- Open Tailor with a hotkey (Shift+Z by default) or with the Tailor Lesser Power.

## Runtime requirements

- Skyrim Special Edition or Anniversary Edition
- SKSE matching the installed Skyrim runtime
- Address Library for SKSE Plugins matching the installed Skyrim runtime
- `Tailor.esp` enabled, for the Tailor Lesser Power

OBody NG and SmoothCam are optional. Tailor works without them.

The same DLL also builds for VR, where the live preview is turned off.

## Opening Tailor

Press **Shift+Z**, or cast the **Tailor** Lesser Power. Tailor gives the power to the player on every load and adds
it to Favorites, unless Disable Tailor Favorite is on in Tailor's Settings. On a controller, the power is the way
in. The game keeps running while Tailor is open, so the preview NPC stays animated.

The hotkey is set in `Data/SKSE/Plugins/Tailor.ini`, as DirectX scan codes:

```ini
[Hotkey]
ModifierKey=42
ActivateKey=44
```

`ModifierKey=0` uses the activate key alone. Set `GrantPower=0` under `[Power]` if you only want the hotkey.

## Controller

| Default Xbox control | Action |
| --- | --- |
| D-pad / left stick | Move between controls |
| A | Select or edit |
| B | Back, then close Tailor |
| LB / RB | Change pages, or the previous and next outfit or wig while dressing |
| X while dressing | Assign the outfit or wig shown |
| Y | Rotate the preview; press again to face the front |
| Right stick | Rotate the preview while rotating, otherwise scroll the list |
| R3 | Switch between controller navigation and a cursor |

The bottom of the screen shows the buttons for the current page. Names and searches are typed on a keyboard.
Controller settings live in `Tailor.ini`:

```ini
[Controller]
Enabled=1
;GlyphFamily=Xbox
Accept=South
Cancel=East
Secondary=West
Tertiary=North
PreviousTab=LeftShoulder
NextTab=RightShoulder
ToggleCursor=RightThumb
```

`GlyphFamily` is `Xbox`, `PlayStation` or `Generic`. The action names take `South`, `East`, `West`, `North`,
`LeftShoulder`, `RightShoulder`, `LeftThumb`, `RightThumb`, `Start` and `Back`. The D-pad is kept for navigation.
A bad or repeated binding puts every action back to its default. Restart the game after changing the file.

## Settings

The Settings page holds three switches, all off by default. They apply to every save and are kept in
`Data/SKSE/Plugins/Tailor/settings.json`.

- Disable Tailor Favorite: keep the Tailor power out of Favorites.
- Hide Weapons: hide weapons, shields and quivers outside Adventuring and combat. Torches stay.
- Hide Helmets: take helmets and hoods off outside Adventuring and combat, and put the same ones back on after.

## Sharing outfits

Open Export, pick outfits, and name the file. Tailor adds `.json` and saves it in `Data/SKSE/Plugins/Tailor`. It
never overwrites an existing file or one of its own files.

To import, put the file in the same folder and choose it on the Import page. With MO2, place it in a mod's
`SKSE/Plugins/Tailor` folder. Import adds new outfits and never replaces outfits or assignments you already have.
An outfit with the same name and the same pieces as one of yours is a duplicate. One with the same name but
different pieces is skipped, since names are unique. Missing armor and unknown categories are reported and skipped.

Only built-in categories travel with an outfit, by these keys: `heavy`, `light`, `clothing`, `adventuring`,
`town`, `home`, `sleep`, `swimming` and `warm`. `sex` is -1 for Unisex, 0 for Male and 1 for Female.

```json
{
  "format": "TailorOutfitExport",
  "version": 1,
  "outfits": [
    {
      "name": "Example Outfit",
      "sex": -1,
      "categories": ["light", "adventuring"],
      "items": [
        {"formId": 2048, "name": "Example Armor", "plugin": "ExampleArmor.esp"}
      ]
    }
  ]
}
```

`formId` is the armor record's plugin-local FormID, and `plugin` its plugin file. Item order and item names don't
matter when matching duplicates.

## Mod API

Other mods can read the player's outfits and categories, build outfits, ask which situation someone is in, dress
anyone in a Tailor outfit, and hear when outfits or situations change. Papyrus scripts call the `Tailor` script
(`papyrus/Source/Scripts/Tailor.psc`). SKSE plugins include [`docs/api/TailorAPI.h`](docs/api/TailorAPI.h). The
reference is [docs/api/README.md](docs/api/README.md), and `tools/api-test/` has a test quest script that calls
every function.

## Building

You need:

- Windows 10 or later
- Visual Studio 2022 or the Visual Studio 2022 Build Tools with MSVC
- [xmake](https://xmake.io/) 3.0.1 or later
- Git

Clone the repository with its CommonLibSSE-NG submodule:

```powershell
git clone --recurse-submodules https://github.com/heathbrownkeyworks/Tailor.git
Set-Location Tailor
```

Configure and build the release DLL:

```powershell
xmake f -p windows -a x64 -m release --skyrim_se=y --skyrim_ae=y --skyrim_vr=y -c
xmake build
```

Always pass `-p windows`; without it xmake picks MinGW. The DLL is written to
`build/windows/x64/release/Tailor.dll`.

This repository holds source, not a ready-to-install mod. A packaged mod also needs:

- `Tailor.esp`, built from `plugin/spriggit/` with Spriggit
- `Scripts/Tailor.pex`, compiled from `papyrus/Source/Scripts/Tailor.psc`, with the source in
  `Source/Scripts/` and `Scripts/Source/`
- the fonts from `assets/fonts/` in `SKSE/Plugins/Tailor/fonts/`
- the preview mesh and textures from `assets/meshes/` and `assets/textures/`

## Source layout

- `src/` is the SKSE plugin.
- `papyrus/` is the `Tailor` script for the mod API.
- `plugin/spriggit/` is the Spriggit source for `Tailor.esp`.
- `assets/` holds the fonts and the preview mesh and textures.
- `docs/api/` is the mod API reference and its C++ header.
- `tools/api-test/` is a test quest for the mod API.
- `lib/commonlibsse-ng/` is the CommonLibSSE-NG submodule.

## License

Tailor is licensed under GPL-3.0-or-later, with the additional permissions in [EXCEPTIONS.md](EXCEPTIONS.md). See
[LICENSING.md](LICENSING.md) and [LICENSE](LICENSE). The API header `docs/api/TailorAPI.h` is also available
under the MIT License ([licenses/TailorAPI-MIT.txt](licenses/TailorAPI-MIT.txt)). Third-party material keeps its
own license; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
