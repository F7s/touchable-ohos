# Third-party notices

This project builds on the following open-source projects. Each keeps its own license.

## nx2android / nxnative (upstream project)

This project is a standalone HarmonyOS fork of the **nx2android** port of Rhythm Heaven Groove:
the app shell (ArkTS UI + NAPI host), the native platform layer (`native/`) and the prebuilt game
core (`core/libdmcore.so`, `app/entry/libs/arm64-v8a/libdmcore.so`) all derive from nx2android's
nxnative/DM engine and HarmonyOS packaging. The from-image toolchain is additionally vendored
under `vendor/nx2a/`.

- Project: https://github.com/seedds/nx2android (nxnative / DM engine)
- Copyright (c) 2026 seedds
- License: MIT
- Upstream third-party details: nx2android `nxnative/dm/third_party/README.md`

## Touchable

The HarmonyOS front end ports the UI/host integration and the touch layer of Touchable's iOS
Rhythm Heaven implementation (`internal/platform/ios/App.m` and the menu/touch behaviour).

- Project: Touchable (`Touchable-main`)
- Copyright (c) mzhao
- License: MIT

## d2r-mobile

The HarmonyOS app shell follows d2r-mobile: the stage-model `EntryAbility` / `Index` page layout
and the native CMake project structure (`dm/harmony/native/CMakeLists.txt`) are adapted from it,
as marked in the file headers.

- Project: https://github.com/jhaizhou-ops/d2r-mobile
- Copyright (c) 2026 jhaizhou
- License: MIT

## nx2android toolchain (vendored under `vendor/nx2a/`)

`vendor/nx2a/` contains the game-image build toolchain from nx2android (Python sources and the
prebuilt `dmsh_xlate` .NET translator), vendored so that `tools/build_from_image.py` needs no
external checkout. Its provenance and the exact list of vendored paths are in
`vendor/nx2a/README.md`; the license is copied to `vendor/nx2a/LICENSE` (MIT, Copyright (c) 2026
seedds).

Bundled inside the vendored translator (`dmsh_xlate_bin/`):

| Component | License |
|---|---|
| Ryujinx (`Ryujinx.Graphics.Shader`, `Ryujinx.Common`) | MIT |
| Spv.Generator | MIT |
| Humanizer | MIT |
| Gommon | MIT |
| Microsoft.IO.RecyclableMemoryStream | MIT |
| MsgPack | MIT |

Build-time NuGet dependency of `nx_extract` (not vendored): **LibHac** (MIT).

Upstream third-party details: nx2android `nxnative/dm/third_party/README.md`.

## bcn_layer / Granite BC7 shader

The prebuilt core includes the BC7 compute shader from leegao/bcn_layer `shader-v3`, originally
from Themaister/Granite.

- Project: https://github.com/leegao/bcn_layer
- Original: https://github.com/Themaister/Granite
- License: MIT

## System libraries

The app links HarmonyOS system libraries (ArkUI/N-API, Vulkan, native window/image, hilog, zlib).
They are provided by the platform and are not redistributed here.

## Other components

Additional third-party components of the vendored porting core are listed in the nx2android
repository (`nxnative/dm/third_party/README.md`).
