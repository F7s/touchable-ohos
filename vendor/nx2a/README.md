# Vendored nx2android toolchain

`tools/build_from_image.py` turns a game image (NSP/XCI) + `prod.keys` into a data pack and an
unsigned HAP. The extraction, module rewriting and shader translation stages come from the
[nx2android](https://github.com/seedds/nx2android) port; they are vendored here so that
touchable-ohos builds from a game image **without any other checkout**.

## Source

Vendored from an nx2android checkout (nxnative, porter and `ohos-feasibility/tools`) on
2026-11-12. Upstream is MIT licensed (see `LICENSE` in this folder, copied from
`nxnative/LICENSE`, Copyright (c) 2026 seedds).

| Vendored path | Upstream path | Notes |
|---|---|---|
| `porter/` | `porter/{__init__,paths,upstream}.py` | only the modules `extract_mods.py` imports |
| `nxnative/scripts/build_ipa_from_nsp.py` | same | NSP scan, `nx_extract` build, version helpers |
| `nxnative/dm/games/` | same | generic readers + the `0100D9F01D474000` game profile only |
| `nxnative/dm/tools/` | same | NSO rewriter, symbol/patch tables, `nx_extract` (.NET) source |
| `nxnative/dm/android/tools/` | same | Vulkan shader bundle writer |
| `nxnative/dm/gfx/tools/` | same, minus caches | shader split + `dmsh_xlate_bin` (.NET) |
| `nxnative/dm/gfx/nvn/dmsh_chain.h` | same | shader chain version header |
| `nxnative/dm/ios/tools/ios_metal.py` | same | read by `dmsh_chain.py` tool fingerprinting |
| `tools/extract_mods.py` | `ohos-feasibility/tools/extract_mods.py` | `REPO` repointed at `vendor/nx2a` |

The `.git`/`__pycache__` folders, the other ~100 game profiles, and the unsupported extraction
paths (iOS host, Unity/Oodle captures) are not vendored.

## Changes from upstream

- `tools/extract_mods.py`: `REPO = Path(__file__).resolve().parents[1]` (was
  `parents[2] / 'nx2android-main'`), so the vendored tree is the repository root.
- `nxnative/dm/harmony/tools/stage_data_pack.sh` is not vendored: `tools/stage_data_pack.py` is a
  cross-platform Python rewrite (so Windows needs no bash) that also accepts an optional app
  directory, letting `build_from_image.py` stage into its temporary app copy.

Python requirements are in `../../requirements.txt`; the .NET SDK 10 is needed to build
`nx_extract` and to run `dmsh_xlate`. The first run restores the `nx_extract` NuGet packages
(network, or a warm `~/.nuget/packages` cache); `nx_extract/obj` is a generated artifact and is
git-ignored.

## Verified

`tools/build_from_image.py` was run against a real Rhythm Heaven dump
(`0100D9F01D474000`, XCI, prod.keys v22.5.0): extraction, module rewrite, shader split,
shader translation and `make_pack.py` produced a 3.21 GiB pack whose `shaders.dmsv` is
byte-identical to the previously built one. The final HAP step was verified by building the
same `app/ + native/` layout with `hvigor` directly (BUILD SUCCESSFUL, HAP emitted).

## Re-vendoring

Copy the paths above from an nx2android checkout, re-apply the two path edits, and keep
`nxnative/LICENSE` as this folder's `LICENSE`.
