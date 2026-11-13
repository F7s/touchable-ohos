#!/usr/bin/env python3
"""Turn a Rhythm Heaven game image + prod.keys into a data pack (and optionally an unsigned HAP).

Pipeline:
  keys + NSP/XCI
    -> nx_extract (ExeFS + full RomFS)
    -> patch_nso.py rewrite of main/sdk/subsdk* (DM_TARGET=ohos)
    -> shader_split.py + shader_vk.py -> shaders.dmsv
    -> make_pack.py -> game-pack/ (+ optional .zip)
    -> [without --pack-only] stage pack + core into the app -> hvigor -> unsigned HAP

This project is standalone: the extraction, rewriting and shader-translation toolchain is vendored under
vendor/nx2a/ (from nx2android, MIT; see vendor/nx2a/README.md). Only Python 3.11+, the .NET SDK and the
Python packages in requirements.txt are needed.

Usage:
  # data pack only: no DevEco Studio / devecocli required (for players who install the release HAP).
  # Writes out/touchable-ohos-<title>-gamepack.zip and removes the unpacked game-pack/ afterwards.
  python3 tools/build_from_image.py --keys prod.keys --game ./game --pack-only

  # data pack + unsigned HAP: needs DevEco Studio (devecocli) and a local signing config
  python3 tools/build_from_image.py --keys prod.keys --game ./game --no-embed-pack --archive

`--game` is a folder containing the base NSP/XCI (and optional update/DLC).
`--out` defaults to <project>/out (git-ignored, so generated game data is never committed or published).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
CORE = PROJECT / 'core' / 'libdmcore.so'
APP = PROJECT / 'app'
VENDOR = PROJECT / 'vendor' / 'nx2a'
GAME_ID = '0100D9F01D474000'


def run(cmd, cwd=None, env=None):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)


def require(path: Path, what: str):
    if not path.exists():
        raise SystemExit(f'missing {what}: {path}')
    return path


def check_python_deps():
    # Imported by the vendored stages; fail early with one readable message instead of a traceback.
    missing = []
    for mod in ('capstone', 'lz4'):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    try:
        import Crypto  # noqa: F401  (pycryptodome; only needed for dumps without a [title ID] tag or ticket)
    except ImportError:
        try:
            import Cryptodome  # noqa: F401
        except ImportError:
            pass  # optional for tagged NSP/XCI dumps
    if missing:
        raise SystemExit(f'missing Python packages: {", ".join(missing)}. Run: pip install -r {PROJECT / "requirements.txt"}')


def check_dotnet():
    if shutil.which('dotnet') is None:
        raise SystemExit('dotnet not found. The .NET SDK 10 is needed for nx_extract and the shader translator.')


def main():
    ap = argparse.ArgumentParser(description='Build a Rhythm Heaven data pack (and optionally an unsigned HAP) from a game image')
    ap.add_argument('--keys', required=True, type=Path, help='prod.keys')
    ap.add_argument('--game', required=True, type=Path, help='folder with the base NSP/XCI')
    ap.add_argument('--out', type=Path, default=None,
                    help='output directory (default: <project>/out, git-ignored)')
    ap.add_argument('--title-id', default=GAME_ID)
    ap.add_argument('--language', default='en-US')
    ap.add_argument('--version', default='1.0.0')
    ap.add_argument('--pack-only', action='store_true',
                    help='write only the .zip pack (game-pack/ is removed afterwards): no DevEco '
                         'Studio, devecocli or signing config needed; the player installs the release HAP')
    ap.add_argument('--no-embed-pack', action='store_true',
                    help='do not put game data into the HAP; emit game-pack/ beside it')
    ap.add_argument('--archive', action='store_true',
                    help='HAP builds only: also write <out>/touchable-ohos-<title>-gamepack.zip '
                         '(pack-only builds always write the zip)')
    ap.add_argument('--keep-pack-dir', action='store_true',
                    help='pack-only: keep the unpacked out/game-pack (default: only the .zip is kept)')
    ap.add_argument('--keep-work', action='store_true')
    args = ap.parse_args()

    require(args.keys, 'prod.keys')
    require(args.game, 'game folder')
    check_python_deps()
    check_dotnet()
    extract_mods = require(VENDOR / 'tools' / 'extract_mods.py', 'vendored extract_mods.py')
    patch_nso = require(VENDOR / 'nxnative' / 'dm' / 'tools' / 'patch_nso.py', 'vendored patch_nso.py')
    shader_split = require(VENDOR / 'nxnative' / 'dm' / 'gfx' / 'tools' / 'shader_split.py', 'vendored shader_split.py')
    shader_vk = require(VENDOR / 'nxnative' / 'dm' / 'android' / 'tools' / 'shader_vk.py', 'vendored shader_vk.py')
    if args.pack_only:
        stage = None
    else:
        require(CORE, 'core/libdmcore.so')
        if not (APP / 'build-profile.json5').exists():
            raise SystemExit(
                'app/build-profile.json5 is missing. It is machine-local and git-ignored (the published '
                'repository ships only build-profile.template.json5), so create it once with your own '
                'signing material:\n'
                '  cd app\n'
                '  cp build-profile.template.json5 build-profile.json5\n'
                '  devecocli signature generate --type debug')
        stage = require(HERE / 'stage_data_pack.py', 'stage_data_pack.py')

    total = 5 if args.pack_only else 6
    out = args.out.resolve() if args.out else (PROJECT / 'out').resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = out / 'work'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    # DM_TARGET=ohos is mandatory for the module rewrite: patch_nso.py defaults to 'apple', and the HarmonyOS
    # core aborts at startup with "the module was rewritten for Apple (TARGET 0x0)" if the fixups say 0x0.
    env = dict(os.environ, DM_GAME=args.title_id, DM_TARGET='ohos', DOTNET_CLI_TELEMETRY_OPTOUT='1')

    # 1. Extract ExeFS + full RomFS.
    print(f'[{1}/{total}] extracting game image', flush=True)
    run([sys.executable, extract_mods, args.game, work, args.keys, '--full-romfs'], env=env)
    extract = work / 'extract'
    versions = sorted([p for p in extract.iterdir() if p.is_dir() and not p.name.endswith('.partial')])
    if not versions:
        raise SystemExit(f'no extracted version under {extract}')
    game = versions[-1]
    exefs = game / 'ExeFS'
    romfs = game / 'RomFS'
    require(romfs, 'extracted RomFS')
    require(exefs / 'main', 'extracted ExeFS/main')

    # 2. Rewrite the executable modules.
    print(f'[{2}/{total}] rewriting modules', flush=True)
    mods = out / 'mods'
    if mods.exists():
        shutil.rmtree(mods)
    mods.mkdir(parents=True)
    patched = {'main', 'sdk', 'subsdk0'}
    for name in sorted(p.name for p in exefs.iterdir() if p.is_file()):
        if name not in patched:
            continue
        run([sys.executable, patch_nso, exefs / name, mods, name], cwd=VENDOR / 'nxnative' / 'dm', env=env)

    # patch_nso.py writes "# TARGET 0x0" for Apple, 0x1 Android, 0x2 HarmonyOS. Fail here instead of shipping a
    # pack the HarmonyOS core aborts on at startup.
    for fix in sorted(mods.glob('*.fixups.tsv')):
        target = ''
        for line in fix.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('# TARGET'):
                target = line.split()[-1]
                break
        if target != '0x2':
            raise SystemExit(f'{fix.name}: rewritten for TARGET {target or "(missing)"}, expected 0x2 (HarmonyOS)')

    # 3. Split and translate shaders.
    print(f'[{3}/{total}] splitting shaders', flush=True)
    pkg = work / 'shader-pkg'
    shader_map = work / 'shader_map.tsv'
    shader_facts = work / 'shader_facts.tsv'
    shader_pairs = work / 'shader_pairs.tsv'
    run([sys.executable, shader_split, romfs, exefs / 'main', pkg,
         shader_map, shader_facts, shader_pairs], env=env)
    print(f'[{4}/{total}] translating shaders', flush=True)
    shaders = work / 'shaders.dmsv'
    run([sys.executable, shader_vk, pkg, shaders, '--jobs', str(os.cpu_count() or 8)], env=env)

    # 4. Build the data pack with the standalone pack tool.
    print(f'[{5}/{total}] building data pack', flush=True)
    pack = out / 'game-pack'
    archive = out / f'touchable-ohos-{args.title_id}-gamepack.zip'
    want_zip = args.archive or args.pack_only
    pack_cmd = [sys.executable, HERE / 'make_pack.py',
                '--romfs', romfs, '--mods', mods, '--shaders', shaders,
                '--out', pack, '--version', args.version, '--lang', args.language]
    if want_zip:
        pack_cmd += ['--zip', str(archive)]
    run(pack_cmd)

    if args.pack_only:
        print(f'\nzip archive:  {archive}')
        if args.keep_pack_dir:
            print(f'data pack:    {pack} (kept with --keep-pack-dir)')
        else:
            # Players only need the single zip; dropping the tree saves ~3.2 GB.
            shutil.rmtree(pack, ignore_errors=True)
            print('data pack:    not kept (use --keep-pack-dir to keep out/game-pack)')
        print('Pack only: no HAP was built. Install the release HAP, then import the zip in the app '
              '(UI -> 导入数据包（ZIP）).')
        if not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)
        return

    # 6. Embed the pack into a copy of the app and build.
    print(f'[{6}/{total}] building HAP', flush=True)
    # The app's CMakeLists.txt adds ../native relative to the project root (app/entry/src/main/cpp -> ../../../../../native),
    # so the build copy needs the same app/ + native/ layout as the repository.
    build_root = work / 'project'
    build_app = build_root / 'app'
    # ignore_patterns matches entry names, not relative paths: 'build' / '.cxx' exclude entry/build and
    # entry/.cxx (a copied CMakeCache would point at the previous project directory).
    shutil.copytree(APP, build_app, ignore=shutil.ignore_patterns(
        'build', '.cxx', '.hvigor', '.preview', 'oh_modules', '__pycache__'))
    shutil.copytree(PROJECT / 'native', build_root / 'native')
    # app project needs its modules; reuse the original's oh_modules if present
    if (APP / 'oh_modules').exists():
        shutil.copytree(APP / 'oh_modules', build_app / 'oh_modules')
    if not args.no_embed_pack:
        run([sys.executable, stage, CORE, pack, build_app], env=env)
    else:
        shutil.copyfile(CORE, build_app / 'entry/libs/arm64-v8a/libdmcore.so')

    # shutil.which honours PATHEXT, so this finds devecocli.cmd on Windows as well as the shim on macOS/Linux.
    devecocli = shutil.which('devecocli')
    if not devecocli:
        raise SystemExit('devecocli not found. Install DevEco Studio and add its devecocli to PATH.')
    run([devecocli, 'build', '--build-mode', 'release', '--assemble-type', 'assembleHap'], cwd=build_app, env=env)
    hap = build_app / 'entry/build/default/outputs/default/entry-default-unsigned.hap'
    require(hap, 'unsigned HAP')
    final = out / f'touchable-ohos-{args.title_id}-unsigned.hap'
    shutil.copyfile(hap, final)
    print(f'\nunsigned HAP: {final}')
    print(f'data pack:    {pack}')
    if want_zip:
        print(f'zip archive:  {archive}')
    if args.no_embed_pack:
        print('HAP has no game data; import game-pack/ in the app.')
    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
