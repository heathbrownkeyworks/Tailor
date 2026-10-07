# Third-Party Notices

Tailor is licensed as described in [LICENSING.md](LICENSING.md). The following third-party components retain their own licenses.

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) is included as a Git submodule and is licensed under GPL-3.0-or-later with its Modding Exception and GPL-3.0 Linking Exception. Tailor statically links it. The exact revision and complete license texts are recorded by the submodule.
- The optional compatibility headers under `src/compat/` retain the notices and terms supplied in those files or by their upstream projects.

## Native interface

- Dear ImGui 1.92.6, including its Win32/DX11 backends, is embedded in `Tailor.dll`. Copyright (c) 2014-2026 Omar Cornut. Its MIT license is included in `licenses/Dear-ImGui-MIT.txt`.
- This software uses the FreeType font engine, version 2.14.1. The FreeType Project license is included in `licenses/FreeType-FTL.txt`.
- The desktop screenshot tool uses `stb_image_write` under the MIT/public-domain dual license retained in the dependency header. It is not included in Tailor's runtime plugin.
- The Poppins and Montserrat fonts in `assets/fonts/` are used under the SIL Open Font License, included in `licenses/Poppins-OFL.txt` and `licenses/Montserrat-OFL.txt`. They are installed under `SKSE/Plugins/Tailor/fonts`.
- zlib (static) decompresses plugin records for `src/outfit/NpcRecordReader.cpp`. Its license is the zlib License, retained in the dependency's `zlib.h`.

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

The plane's four-vertex, two-triangle geometry is original Tailor geometry.
Its opaque, self-emissive lighting material and flat diffuse/normal textures
are adapted from Menu Studio's `voidshell.nif`; the texture paths are renamed
for Tailor. The emissive material uses `#2a2118` as Skyrim/ENB compensation
for Tailor's intended warm-obsidian stage color.
Tailor clones the plane as a camera-facing wall and floor without modifying
shared material state at runtime. No Menu Studio runtime code is included in
the backdrop-loading path.

## Menu Studio-informed live scene isolation

`src/preview/PreviewScene.cpp` adapts the recursive visibility/restore strategy,
world-feeder coverage, exact player skeleton ownership, always-draw flags and
dynamic light setup from Menu Studio by maartenharms and contributors, GPL-3.0,
commit `8b64be319916223f7bc42700a48ec01ce190b9aa` (`src/Declutter.cpp` and
`src/StudioRig.cpp`). Tailor supplies its own lifecycle and guarded free camera;
it does not modify cell records or the portal graph, clone an NPC, or require
Menu Studio at runtime.

`assets/meshes/Tailor/Preview/tailor_brown_plane.nif` is the same plane with
opaque, non-specular self-emission set to Tailor's `#2a2118` theme color.
Its root is a plain `NiNode`; the runtime preserves the plane's
engine-initialized shader property and fade-node link. The current preview
uses a camera-facing wall and has no floor platform.

## SmoothCam public API declarations

`src/compat/SmoothCamAPI.h` contains the minimal V1 ABI declarations needed
to request and release camera ownership from SmoothCam. They are adapted from
SmoothCam's published
<https://github.com/mwilsnd/SkyrimSE-SmoothCam/blob/master/SmoothCam/include/SmoothCamAPI.h>
at commit
`66f3960ec4de2b28af5e863c794a3924e6a2dfdd`. The upstream header explicitly
permits mod authors to copy it into their projects.
