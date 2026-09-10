# Tailor

Tailor is an SKSE plugin for managing NPC outfits, wigs, hair colors, and
situation-based appearances in Skyrim Special Edition, Anniversary Edition,
and VR.

Current source version: **2.4.3**

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
- `Tailor.esp` enabled when using the Tailor Lesser Power

OBody NG and SmoothCam are optional integrations. Tailor continues without
them.

The isolated live NPC preview is available on SE/AE; it is disabled on VR.
The 2.4.1 sleep-state correction has automated coverage for six runtime layouts.
Automated and browser checks do not replace gameplay verification of the new
2.4.3 workflows on each supported runtime.

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
