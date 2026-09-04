# Third-party notices

Tailor's original work is licensed under the MIT License. The complete license
text is in `LICENSE`. Third-party material retains its applicable upstream
license as described below.

## Menu Studio-derived preview-backdrop material

The following Tailor preview assets are adapted from Menu Studio by
maartenharms and contributors:

- `assets/meshes/Tailor/Preview/tailor_background_plane.nif`
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
Tailor clones the plane as a camera-facing wall and floor without modifying
shared material state at runtime. No Menu Studio runtime code is included in
the backdrop-loading path.

## Menu Studio-derived AE-safe isolation paths

`src/preview/TailorPreviewSession.cpp` adapts Menu Studio's safe loaded-cell
reference enumeration strategy from `src/Declutter.cpp` at the same commit.
The Tailor implementation collects reference handles while CommonLib holds a
cell's reference lock, performs scene-graph changes only after that lock is
released, and deliberately omits CommonLib's final sky-cell access on exterior
AE runtimes. Tailor uses this only for a bounded, reversible preview-isolation
bubble; Menu Studio's broader declutter, lighting, imagespace, weather, and
world-feeder mutations are not included.

Tailor also follows Menu Studio's render-only terrain isolation pattern from
the same `Declutter.cpp`: for each relevant attached exterior cell, it claims
only that LAND record's four loaded quadrant mesh roots, plus a standalone
geometry fallback when a quadrant has no mesh root or its geometry is not a
descendant. Tailor retains strong node references, changes only the transient
AppCull bit, leaves terrain collision and records untouched, and restores the
exact nodes during the preview session's existing ownership-aware teardown.

Tailor also adopts Menu Studio's field-tested distinction between placed light
art and renderer illumination. It includes validated `LIGH` reference roots in
the reversible isolation bubble to hide candle/flame/smoke artwork, but does
not self-cull their separately gathered `NiLight` emitters or copy Menu
Studio's broader cell-light/studio-rig behavior. This preserves the existing
follower illumination until Tailor owns a dedicated light rig.

## SmoothCam public API declarations

`src/compat/SmoothCamAPI.h` contains the minimal V1 ABI declarations needed
to request and release camera ownership from SmoothCam. They are adapted from
SmoothCam's published
<https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/include/SmoothCamAPI.h>
at commit
`66f3960ec4de2b28af5e863c794a3924e6a2dfdd`. The upstream header explicitly
permits mod authors to copy it into their projects.
