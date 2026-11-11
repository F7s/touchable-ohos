#!/usr/bin/env python3
"""C0 helper: extract a game's code modules (ExeFS) — and only the RomFS manifest, no RomFS files — for the
DM_TARGET=ohos rewrite test. Reuses upstream's dump scanner and nx_extract build from
nxnative/scripts/build_ipa_from_nsp.py; writes nothing into the repository.

Vendored into touchable-ohos as vendor/nx2a/tools/extract_mods.py (from nx2android's
ohos-feasibility/tools/extract_mods.py, MIT); REPO now points at the vendored nx2a tree.

Usage:
  extract_mods.py <game folder with the NSP/XCI> <work dir> <prod.keys> [--full-romfs]

  <work dir> is where upstream keeps <title ID>_build: ExeFS lands in <work dir>/extract/<version>/ExeFS.
"""
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]   # vendor/nx2a
sys.path.insert(0, str(REPO))
from porter.upstream import game, nsp  # noqa: E402  (nxnative/scripts/build_ipa_from_nsp.py)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    folder, work, keys = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    full = '--full-romfs' in sys.argv[4:]
    work.mkdir(parents=True, exist_ok=True)
    logs = work / 'logs'
    logs.mkdir(exist_ok=True)
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1')
    app, fam = nsp.scan_nsps(folder, keys=keys)
    g = game.load(f'{app:016X}')
    have = nsp.nsp_versions(fam, g.builds())
    ver = max(have, key=nsp.vnum)
    nx = nsp.build_nx_extract(REPO / 'nxnative', work, logs, env)
    ex = work / 'extract'
    tmp = ex / (ver + '.partial')
    nsp.rmtree(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    argv = [str(nx), str(keys), str(fam['base']), str(have[ver] or '-'), str(tmp)]
    if not full:
        argv.append('--manifest')
    print('run', ' '.join(argv), flush=True)
    nsp.run(argv, logs / f'extract_{ver}.log')
    nsp.swap_in(tmp, ex / ver)
    print('extracted ->', ex / ver)
    print('modules:', ', '.join(sorted(p.name for p in (ex / ver / 'ExeFS').iterdir())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
