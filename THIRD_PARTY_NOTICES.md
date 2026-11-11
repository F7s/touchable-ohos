# Third-party notices

## Touchable

This project ports the HarmonyOS front-end and host integration from Touchable's
iOS Rhythm Heaven implementation.

- Project: Touchable
- License: MIT

## bcn_layer / Granite BC7 shader

`app/entry/src/main/cpp` and the prebuilt core include the BC7 compute shader
from leegao/bcn_layer `shader-v3`, originally from Themaister/Granite.

- Project: https://github.com/leegao/bcn_layer
- Original: https://github.com/Themaister/Granite
- License: MIT

## nx2android / nxnative toolchain (vendored)

`vendor/nx2a/` contains the game-image build toolchain from nx2android (Python
sources and the prebuilt `dmsh_xlate` .NET translator), vendored so that
`tools/build_from_image.py` needs no external checkout.

- Project: nx2android / nxnative
- License: MIT (Copyright (c) 2026 seedds), `vendor/nx2a/LICENSE`
- Shader translator: Ryujinx (`Ryujinx.Graphics.Shader`, `Ryujinx.Common`,
  `Spv.Generator`), MIT
- Helper libraries in `dmsh_xlate_bin/`: Humanizer, Gommon,
  Microsoft.IO.RecyclableMemoryStream, MsgPack — MIT

Upstream third-party details: nx2android `nxnative/dm/third_party/README.md`.

## Other components

Additional third-party components and their licenses are listed in the
nx2android repository.
