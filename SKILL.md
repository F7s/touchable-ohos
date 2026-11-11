---
name: touchable-ohos
description: How to build, package, install, publish and troubleshoot this standalone Rhythm Heaven Groove (title 0100D9F01D474000) HarmonyOS port. Use when the task is to build the code-only HAP, generate a data pack from the user's own NSP/XCI + prod.keys, sign/install/launch the HAP on a HarmonyOS device, import a pack (folder or single .zip) in the app, cut a GitHub release, or diagnose startup crashes and import failures — even when the user only says "build the hap", "generate the data pack", "why does it crash on launch", "import the game", or "publish this".
---

# touchable-ohos

A standalone HarmonyOS port of Rhythm Heaven Groove. It publishes the **app shell + prebuilt core**
and lets each user turn their own game files into a data pack. The repository never contains game data.

- Title ID: `0100D9F01D474000` — the only supported game.
- Bundle: `com.nx2a.rhythm` (legacy pack package `com.nx2a.dm` is also accepted by the core).
- Reads: [README.md](README.md) for the project, [docs/TUTORIAL.md](docs/TUTORIAL.md) for the human tutorial.

## Golden rules

- **Never commit or publish game data**: `out/`, `dist/`, `*.hap`, RomFS, mods, `shaders.dmsv`,
  `prod.keys`. They are git-ignored; verify with `git status --ignored` before a release.
- **`DM_TARGET=ohos` is mandatory** whenever `patch_nso.py` runs. Its default is `apple`; a pack built
  without it aborts the core at startup with `rewritten for Apple (TARGET 0x0)`. `build_from_image.py`
  sets it and verifies `# TARGET 0x2` — never bypass that check.
- **Ask before installing on the user's device**, and never uninstall their own install without being told.
  Test builds are the same bundle id, so an install replaces the existing app and its imported pack.
- **Signing material stays local**: `app/build-profile.json5` is git-ignored; each machine runs
  `devecocli signature generate` once. Never commit certificates or passwords.
- **No keys or game files in the chat**: print key names, never values.

## Repository map

| Path | What it is |
|---|---|
| `app/` | HarmonyOS stage-model project (ArkTS UI + NAPI host). `app/entry/libs/arm64-v8a/libdmcore.so` is the prebuilt game core and ships with the repo. |
| `core/libdmcore.so` | Same core, canonical copy. `build_from_image.py` stages it into the build. |
| `native/` | C sources of `libdmhost.so`: NAPI host (`napi_ohos.c`), Vulkan surface, and the reference-based ZIP extractor (`unzip_fd.c`). |
| `tools/build_from_image.py` | NSP/XCI + prod.keys → data pack (+ optional `.zip`) → unsigned HAP. |
| `tools/make_pack.py` | Already-extracted RomFS/mods/shaders → pack (stdlib only). `--zip` writes the single-file pack. |
| `tools/stage_data_pack.py` | Stage a pack + core into an app project (used by `build_from_image.py`). |
| `vendor/nx2a/` | Vendored nx2android toolchain (MIT): extraction, NSO rewriter, shader split/translate. |
| `docs/TUTORIAL.md` | End-user tutorial (Chinese): setup, signing, build, import, troubleshooting. |

## Standard flows

### A. Code-only HAP (no game data, what releases ship)

```bash
cd app
cp build-profile.template.json5 build-profile.json5   # first time only
devecocli signature generate --type debug             # first time only
devecocli build --build-mode debug --assemble-type assembleHap
# output: app/entry/build/default/outputs/default/entry-default-{signed,unsigned}.hap
```

The HAP must contain only ~16 entries (`ets/`, `libs/`, `resources/`), no `RomFS/`, `mods/`,
`shaders.dmsv`, `devpack` or `c0mods`. Verify with `unzip -l`.

### B. Data pack + HAP from a game image

```bash
python3 -m pip install -r requirements.txt   # lz4, capstone (+ optional pycryptodome, backports.zstd)
python3 tools/build_from_image.py \
  --keys /path/to/prod.keys \
  --game /path/to/folder-with-nsp-or-xci \
  --no-embed-pack \
  --archive
# out/game-pack/                                 3.21 GiB
# out/touchable-ohos-0100D9F01D474000-gamepack.zip   ~2.6 GiB
# out/touchable-ohos-0100D9F01D474000-unsigned.hap   code-only HAP
```

Steps: extract → rewrite `main/sdk/subsdk0` (`DM_TARGET=ohos`) → split shaders → translate to
Vulkan → `make_pack.py` → build HAP. Needs Python 3.11+, .NET SDK 10, `devecocli`; macOS/Windows/Linux
(all Python + .NET, no bash).

### C. Install and run

```bash
hdc list targets                       # or: hdc tconn <ip>:<port>
hdc -t <serial> install -r out/touchable-ohos-0100D9F01D474000-unsigned.hap
hdc -t <serial> shell aa start -a EntryAbility -b com.nx2a.rhythm
```

Launching while the screen is locked fails with `10106102`. Unlock and keep the screen on.

### D. Import the pack in the app

UI (bottom-left) → **导入数据包（ZIP）** or **导入数据包（文件夹）**, then fully close and reopen.

- ZIP import is **reference-based**: the app opens the picked file and passes its fd to native
  `unzipStart`, which extracts with `pread`; the zip is never copied into the sandbox. Peak space =
  unpacked pack (+ the old pack during a re-import). Supports store/deflate, per-entry ZIP64 offsets;
  **no encrypted zips, no ZIP64 containers** (>4 GB / >65535 entries).
- Folder import tree-copies the parts (peak ≈ 2× the pack).
- Transfer the zip over USB; hdc over Wi-Fi measured ~3 MB/s (~14 min for 2.6 GB).

### E. Release

1. Build the code-only HAP (flow A), confirm `unzip -l` has no game data.
2. `git status --ignored` — nothing under `out/`, `dist/`, `*.hap`, game files staged.
3. Tag + GitHub release, attach the **unsigned** code-only HAP and its sha256. Do not attach signed
   HAPs (they carry the builder's certificate) or any pack.
4. Release notes: supported title ID, HarmonyOS 7.0/API 26/arm64, "bring your own game files", link to
   `docs/TUTORIAL.md`.

## Verification checklist

```bash
# data pack target and structure
grep TARGET out/game-pack/mods/*.fixups.tsv          # must be 0x2
unzip -p out/touchable-ohos-0100D9F01D474000-gamepack.zip nx2a-data.json
unzip -l out/touchable-ohos-0100D9F01D474000-gamepack.zip | head

# code-only HAP must not contain game data
unzip -Z1 out/touchable-ohos-0100D9F01D474000-unsigned.hap | grep -Ei 'RomFS|mods/|shaders|devpack|c0mods' || echo clean

# device logs during import / boot
hdc -t <serial> shell hilog -x -T nx2a-ohos
hdc -t <serial> shell hilog -x -P <pid>
```

## Failure signatures (exact log strings)

| Log | Cause | Fix |
|---|---|---|
| `the module was rewritten for Apple (TARGET 0x0)` | pack built without `DM_TARGET=ohos` | regenerate with `build_from_image.py`; re-import the new pack |
| `musl thread slot layout mismatch` | core's TSD offset no longer matches the system libc | core fix (`OHOS_TSD_OFF` in `patch_nso.py`), not a pack issue |
| `10106102 ... screen is locked` | device locked | unlock, retry |
| `P1-32 空间不足` | not enough free space for unpacked pack (+ old pack) | free space |
| `P1-33 ZIP 里没有 nx2a-data.json` | picked the wrong archive / nested folder | pick the archive whose root holds `mods/ RomFS/ shaders.dmsv nx2a-data.json` |
| `P1-37 解压失败（-3）` | encrypted or ZIP64 container | rebuild with `make_pack.py --zip` |
| `spawn EPERM` during `devecocli build` | restricted sandbox can't write `~/.hvigor` / `~/.npm` | run in a normal user shell |

## Constraints to respect when changing code

- The data pack contract is enforced twice: `native/napi_ohos.c` (`validatePack`, package/target/mods/
  shaders/RomFS checks) and the ArkTS import UI. Keep the two in sync.
- `NX2A_OHOS_BUNDLE` in `native/napi_ohos.c`, `PACKAGE` in `tools/make_pack.py` and
  `AppScope/app.json5` must stay identical (`com.nx2a.rhythm`).
- Native ZIP extraction lives in `native/unzip_fd.c`; it can be host-tested without a device:
  `cc -O2 -DUF_HOST_TEST native/unzip_fd.c -lz -o /tmp/uf_test && /tmp/uf_test pack.zip /tmp/out`
  (compare file count/sizes/CRC with `unzip -t` or Python `zipfile`).
- `vendor/nx2a/` is vendored upstream code: don't edit it beyond the path edits recorded in
  `vendor/nx2a/README.md`; re-vendor instead.
