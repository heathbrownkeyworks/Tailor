# Third-party notices

Tailor's original work is licensed under the MIT License. The complete license
text is in `LICENSE`. Third-party material retains its applicable upstream
license as described below.

## Menu Studio-derived preview-backdrop material

The following Tailor preview assets are adapted from Menu Studio by
maartenharms and contributors:

- `assets/meshes/Tailor/Preview/tailor_background_plane.nif`
- `assets/meshes/Tailor/Preview/tailor_brown_plane.nif`
- `assets/textures/Tailor/Preview/stage_white.dds`
- `assets/textures/Tailor/Preview/void_d.dds`
- `assets/textures/Tailor/Preview/void_n.dds`

Source: <https://github.com/maartenharms/menu-studio> at commit
`fa70b79e67d1331916d2cec9b41f514d060c9920` (Menu Studio 0.7.2).
Menu Studio is distributed under GPL-3.0.
The complete GPL-3.0 license text for this material is in
`LICENSES/Menu-Studio-GPL-3.0.txt`.

The plane's four-vertex, two-triangle geometry is original Tailor geometry.
Its opaque, self-emissive lighting material and flat diffuse/normal textures
are adapted from Menu Studio's `voidshell.nif`; the texture paths are renamed
for Tailor. The emissive material uses `#2a2118` as Skyrim/ENB compensation
for Tailor's intended warm-obsidian stage color.
The active brown plane has a plain `NiNode` root and is used as a camera-facing
wall, without a floor platform. Tailor preserves its engine-initialized shader
property and required fade-node link.

## Menu Studio-informed live scene isolation

`src/preview/PreviewScene.cpp` adapts the recursive visibility/restore strategy,
world-feeder coverage, exact player skeleton ownership, always-draw flags and
dynamic light setup from Menu Studio by maartenharms and contributors, GPL-3.0,
commit `8b64be319916223f7bc42700a48ec01ce190b9aa` (`src/Declutter.cpp` and
`src/StudioRig.cpp`). This adapted material retains GPL-3.0; the complete text
is in `LICENSES/Menu-Studio-GPL-3.0.txt`. Tailor supplies its own lifecycle and
guarded free camera. It does not modify cell records or the portal graph,
clone an NPC, or require Menu Studio at runtime.

## SmoothCam public API declarations

`src/compat/SmoothCamAPI.h` contains the minimal V1 ABI declarations needed
to request and release camera ownership from SmoothCam. They are adapted from
SmoothCam's published
<https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/include/SmoothCamAPI.h>
at commit
`66f3960ec4de2b28af5e863c794a3924e6a2dfdd`. The upstream header explicitly
permits mod authors to copy it into their projects.
