# Tailor

Tailor is an SKSE plugin for managing NPC outfits, wigs, hair colors, and
situation-based appearances in Skyrim Special Edition, Anniversary Edition,
and VR.

Current source version: **2.3.8**

## Changes in 2.3.8

- Recover assigned wigs after follower dismissal and external equipment removal.
- Slow the game timescale to 1 while Tailor is open and restore it on closing.
- Place an outfit in multiple categories while preserving existing libraries and assignments.
- Use any outfit with either NPC sex; wig categories keep their existing behavior.
- Return to the generic assigned outfit when a situational outfit no longer applies.
- Detect sleep entry and waking through the game's sleep state, including alternate sleep idles.
- Rotate the preview camera by holding the controls or dragging, and reset it to the front.
- Keep NPC movement held during preview while preserving behavior and animation processing.

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
The 2.3.8 source has passed native builds and automated checks. Gameplay testing
of these changes is pending; runtimes and renderer configurations need separate testing.

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
