#!/usr/bin/env python3
"""Stage a prebuilt host core and a full data pack into an app project for development builds.

Both outputs are git-ignored: no game data is committed to the repository.

Usage:
  stage_data_pack.py <libdmcore.so> <pack dir> [app dir]

  <pack dir> has the porter's data pack layout: mods/ RomFS/ aoc/ sysdata/ shaders.dmsv env.txt
  settings.txt nx2a-data.json (fonts, fpslock.tsv optional).
  [app dir] defaults to <this repo>/app; tools/build_from_image.py passes its temporary app copy.

What it does:
  1. copies libdmcore.so into <app dir>/entry/libs/arm64-v8a/ (dlopen("libdmcore.so") finds app
     native libs by soname);
  2. copies the pack tree into <app dir>/entry/src/main/resources/rawfile/devpack/, with a manifest of
     relative paths (forward slashes) and a version token; the ArkTS page unpacks RomFS/ to
     <filesDir>/RomFS and everything else to <filesDir>/dm once per version (the sandbox forbids
     symlinks and hard links, so files are copied).

Cross-platform Python rewrite of nx2android's nxnative/dm/harmony/tools/stage_data_pack.sh (MIT), so
the from-image pipeline works on Windows as well as macOS/Linux without a bash shell.
"""
import hashlib
import shutil
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    core_lib = Path(sys.argv[1]).resolve()
    pack_dir = Path(sys.argv[2]).resolve()
    project = Path(__file__).resolve().parent.parent
    app_dir = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else project / 'app'

    if not core_lib.is_file():
        print(f'missing core library: {core_lib}', file=sys.stderr)
        return 1
    if not pack_dir.is_dir():
        print(f'missing pack directory: {pack_dir}', file=sys.stderr)
        return 1
    if not (pack_dir / 'nx2a-data.json').is_file():
        print(f'warning: {pack_dir} has no nx2a-data.json (the app will reject it)', file=sys.stderr)

    libs_dir = app_dir / 'entry' / 'libs' / 'arm64-v8a'
    raw_dir = app_dir / 'entry' / 'src' / 'main' / 'resources' / 'rawfile'
    pack_raw = raw_dir / 'devpack'

    libs_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(core_lib, libs_dir / 'libdmcore.so')

    if pack_raw.exists():
        shutil.rmtree(pack_raw)
    pack_raw.mkdir(parents=True)

    files = sorted(
        (p for p in pack_dir.rglob('*') if p.is_file() and p.name != '.DS_Store'),
        key=lambda p: p.relative_to(pack_dir).as_posix(),
    )
    manifest_lines = []
    for src in files:
        rel = src.relative_to(pack_dir).as_posix()
        dst = pack_raw / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        manifest_lines.append(rel)

    raw_dir.mkdir(parents=True, exist_ok=True)
    (raw_dir / 'devpack.manifest').write_text('\n'.join(manifest_lines) + '\n', encoding='utf-8', newline='\n')

    # Version token: a hash over the sorted (file hash, relative path) lines. Only used as a marker for
    # "already unpacked this pack", so the exact formula is not a compatibility surface.
    digest = hashlib.sha256()
    for src, rel in zip(files, manifest_lines):
        digest.update(f'{sha256_file(src)}  {rel}\n'.encode('utf-8'))
    version = 'devpack-' + digest.hexdigest()[:16]
    (raw_dir / 'devpack.version').write_text(version + '\n', encoding='utf-8', newline='\n')

    total = sum((pack_raw / rel).stat().st_size for rel in manifest_lines)

    def human(n: int) -> str:
        return f'{n / 1048576:.1f} MB' if n >= 1048576 else f'{n / 1024:.0f} KB'

    print(f'staged {libs_dir / "libdmcore.so"} ({human(core_lib.stat().st_size)})')
    print(f'staged {len(manifest_lines)} files from {pack_dir} into {pack_raw} ({human(total)}), version {version}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
