# Tailor

Tailor is an SKSE plugin for managing NPC and player outfits, wigs, hair colors, and
situation-based appearances in Skyrim Special Edition, Anniversary Edition,
and VR.

Current source version: **2.5.0**

## Changes in 2.5.0

- Added gamepad navigation, contextual button prompts, cursor mode and preview rotation through Meridian Input.
- Added a configurable LB + Menu/Start opener that preserves Horde's LB + Y and Romantasy's RB + View/Back shortcuts.
- Added player outfit and wig support, with player assignments stored in the SKSE co-save.
- Outfits take priority over assigned wigs when a helmet or hood is worn. Wig screens temporarily hide conflicting headgear and restore it on exit.
- Added equipment diagnostics for outfit pieces that fail to equip or attach after settling.

The release passed native and browser checks. Skyrim testing of the new controller
and headwear behavior is still pending. The Bonemold and H2135 armor display reports
remain open for gameplay verification; this release does not modify those armor assets.

## Changes from 2.4.1 to 2.4.3

- Added outfit import and export with name, armor type and category filters.
- Imports preserve existing libraries and assignments and skip matching duplicates.
- Added per-NPC Adventuring armor preferences for Heavy Armor, Light Armor, Clothing or Any.
- Added outfit previews by clicking rows in Manage Outfits.
- Outfits and wigs now appear alphabetically in Dressing previews.
- Improved preview cleanup and outfit restoration, including support for armors with mismatched mesh slots such as Obi Bodysuit.
- Improved outfit fallback when no suitable Adventuring outfit is available.
- Fixed sidebar tooltips appearing behind page content.
- Added centered completion notifications for outfit exports and imports.

## Changes in 2.4.1

- Fixed incorrect sleep detection that could leave awake NPCs wearing sleep outfits.
- Preserves early sleep outfit changes and existing libraries and assignments.

## Changes from 2.3.7 to 2.4.0

- Outfits can belong to multiple categories.
- Outfit categories work with both male and female NPCs.
- Duplicate legacy default categories are combined while preserving existing outfits and assignments.
- Improved wig retention when followers are dismissed or equipment is reset.
- Faster wig recovery after magic effects and stripping events.
- Fixed hair color tiles sometimes requiring a second click.
- NPCs return to their regular outfit after situational outfits end.
- Sleep outfits apply earlier when NPCs enter bed and support alternate sleep animations.
- Slows in-game time while Tailor is open and restores the original timescale on exit.
- Improved preview rotation, camera restoration and NPC movement handling.

## Features

- Create shared outfit categories and place outfits in multiple categories.
- Build outfits from armor records already loaded in the game.
- Preview outfits and wigs on the selected NPC before saving an assignment.
- Inspect hair in a closer preview that uses the NPC's existing in-game appearance.
- Keep assigned outfit items in Skyrim's hidden outfit inventory instead of
  the NPC's normal trade inventory.
- Restore the NPC's plugin-defined default outfit with Reset Outfit, including
  assignments created by older Tailor versions.
- Assign fixed or randomized outfits and wigs for adventuring, town, home, and
  sleep situations.
- Choose an Adventuring armor preference for each NPC's fixed and random outfits.
- Share selected outfits through portable JSON files with built-in categories.
- Save persistent outfit, wig, and hair-color assignments.
- Coordinate outfit changes with OBody NG so the NPC keeps the assigned body.
- Use SmoothCam camera ownership when SmoothCam is installed.
- Open the interface with the configurable hotkey or the Tailor Lesser Power.
- Build one Address Library based DLL for Skyrim SE, AE, and VR.

## Runtime requirements

- Skyrim Special Edition, Anniversary Edition, or VR
- SKSE matching the installed Skyrim runtime
- Address Library for SKSE Plugins matching the installed Skyrim runtime
- Meridian UI with the `Meridian.View/1` interface for the in-game browser UI
- Meridian UI 1.5.0 or another runtime providing `Meridian.Input/1` for controller support
- `Tailor.esp` enabled when using the Tailor Lesser Power

OBody NG and SmoothCam are optional integrations. Tailor continues without
them.

The isolated live NPC preview is available on SE/AE; it is disabled on VR.
The 2.4.1 sleep-state correction has automated coverage for six runtime layouts.
Automated and browser checks do not replace gameplay verification of the new
2.5.0 workflows on each supported runtime.

## Controller controls

Hold **LB**, then press **Menu/Start** to open Tailor. In the UI:

| Default Xbox control | Action |
| --- | --- |
| D-pad / left stick | Navigate controls |
| A | Select, activate or edit |
| B | Leave editing, close a popup, cancel a preview, go back, then close Tailor |
| LB / RB | Change pages, or cycle outfits/wigs in the dressing screens |
| X while cycling | Assign the displayed outfit or wig |
| Y | Enter preview rotation; press again to reset to front |
| Right stick in rotation mode | Rotate the preview camera |
| Right stick elsewhere | Scroll the selected list |
| Right stick click | Toggle cursor mode |

Prompts follow Meridian's bindings and controller label family. Names and search
text use a physical keyboard. RGB fields use A to start editing, directions to
adjust, and A or B to finish. Empty mod search fields can be browsed with directions.
Keyboard/mouse and the Tailor Lesser Power remain available without Meridian Input.

These defaults also apply to existing `Data/SKSE/Plugins/Tailor.ini` files without
a Controller section. Restart the game after changing them:

```ini
[Controller]
Enabled=1
ShortcutEnabled=1
Modifier=LeftShoulder
Button=Start
```

Set `ShortcutEnabled=0` to disable only the opener or `Enabled=0` to disable
controller support. Digital names include `Start`, `Back`, `LeftShoulder`,
`RightShoulder`, `LeftThumb`, `RightThumb`, `South`, `East`, `West`, `North`, and
`DpadUp/Down/Left/Right`. `Modifier=None` permits a single button. Conflicting or
reserved chords disable Tailor's opener and are reported in its log.

## Sharing outfits

Use Export in the outfit sidebar to select outfits, filter the list and name a
JSON file. Tailor adds `.json` and saves in `Data/SKSE/Plugins/Tailor`. Only
built-in category memberships are shared. Existing files and Tailor's own
library, assignment and blacklist files cannot be overwritten.

Place a shared file in the same folder and choose it from Import. With MO2,
make the file available through a mod's `SKSE/Plugins/Tailor` folder. Import
adds valid outfits without replacing existing outfits or NPC assignments.
An outfit is a duplicate only when its exact name and armor item set match.
Missing armor and unsupported categories are reported and skipped.
Completed exports and imports show the same centered notification as other saves,
with detailed results retained on the page.

Exports use stable category keys: `heavy`, `light`, `clothing`, `adventuring`,
`town`, `home` and `sleep`. Local numeric category IDs are not portable.

```json
{
  "format": "TailorOutfitExport",
  "version": 1,
  "outfits": [
    {
      "name": "Example Outfit",
      "categories": ["light", "adventuring"],
      "items": [
        {"formId": 2048, "name": "Example Armor", "plugin": "ExampleArmor.esp"}
      ]
    }
  ]
}
```

Replace the example item with an installed armor record's plugin-local FormID
and plugin filename. Item order and item display names do not affect duplicate
matching; different outfit names remain separate outfits.

## Building

Requirements:

- Windows 10 or later
- Visual Studio 2022 or the Visual Studio 2022 Build Tools with MSVC
- [xmake](https://xmake.io/) 3.0.1 or later
- Git

Clone the repository and its CommonLibSSE-NG submodule:

```powershell
git clone --recurse-submodules https://github.com/heathbrownkeyworks/Tailor.git
Set-Location Tailor
```

Configure and build the unified release DLL:

```powershell
xmake f -p windows -a x64 -m release --skyrim_se=y --skyrim_ae=y --skyrim_vr=y -c
xmake build
```

The DLL is written to:

```text
build/windows/x64/release/Tailor.dll
```

The repository contains source files rather than a ready-to-install mod
archive. The packaged mod also needs the generated `Tailor.esp`, the Meridian
UI files from `view/`, and the preview assets from `assets/`.

## Source layout

- `src/` contains the native SKSE plugin.
- `view/` contains the Meridian UI web view.
- `plugin/spriggit/` contains the Spriggit source for `Tailor.esp`.
- `assets/` contains the live-preview mesh and textures.
- `lib/commonlibsse-ng/` is the CommonLibSSE-NG submodule.

## License

Tailor's original work is available under the [MIT License](LICENSE).
Third-party material retains its applicable upstream license. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for attribution and license
details.
