// nx2android HarmonyOS NEXT host page: XComponent (native surface, libdmhost.so) + boot information overlay.
//
// P0: the NAPI host prepares the sandbox directories, sets DM_ROOT/DM_ROMFS/DM_SAVE/DM_TEMP and logs the
// nx2a-ohos boot lines.
// C0: if tools/stage_c0_package.sh staged a core package (rawfile c0mods.*), the rewritten modules are unpacked
// into <filesDir>/dm/mods once per package version before the native host starts the shared core.
// P1: a data pack (mods/, RomFS/, aoc/, sysdata/, shaders.dmsv, env.txt, settings.txt, nx2a-data.json, fonts) is
// imported from a folder the player picks with the system file picker, with a space check and progress. The
// HarmonyOS sandbox refuses symlinks and hard links, so the pack is tree-copied. It can also come as a single
// .zip (tools/make_pack.py --zip): the archive is read in place from the picker file descriptor (native unzip,
// no copy into the sandbox) and the parts are moved into place, so the player only has to send one file to the
// phone. Native validatePack() then checks the manifest, mods, shaders and RomFS with stable error codes.
//
// Scaffolded from the DevEco Studio stage-model template (as ohos-feasibility/probe); page structure follows
// d2r-mobile-main/dm/harmony/app/entry/src/main/ets/pages/Index.ets (MIT, Copyright (c) 2026 jhaizhou).
import dmhost from 'libdmhost.so';
import { i18n } from '@kit.LocalizationKit';
import { common } from '@kit.AbilityKit';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { TouchableOverlay } from '../components/TouchableOverlay';
import { vibrator } from '@kit.SensorServiceKit';

const DOMAIN: number = 0xD2E1;
const TAG: string = 'nx2a-ohos';
// OHOS fileIo: the picked folder has no such entry. 13900002 is the directory-copy form; 13900020 is what
// fileIo.copy returns for a missing file behind a picker URI. Both mean "this optional part is absent".
const ERR_NO_SUCH_ENTRY: number = 13900002;
const ERR_NO_SUCH_FILE: number = 13900020;
// Data pack parts, in copy order; the first four are required, the rest are copied when present.
const PACK_DIRS: string[] = ['mods', 'RomFS', 'aoc', 'sysdata'];
const PACK_FILES: string[] = ['shaders.dmsv', 'env.txt', 'settings.txt', 'nx2a-data.json', 'standard.ttf', 'nintendo_ext.ttf', 'fpslock.tsv'];
const PACK_REQUIRED: string[] = ['mods', 'RomFS', 'shaders.dmsv', 'nx2a-data.json'];
// ZIP import staging inside the sandbox: only the unpacked tree (the archive itself is read in place).
const IMPORT_TMP_NAME: string = 'import-tmp';

@Entry
@Component
struct Index {
  @State rc: number = 0;
  @State boot: string = 'boot: waiting';
  @State c0: string = 'c0: checking staged package';
  @State pack: string = 'pack: checking';
  @State surface: string = 'surface: waiting for XComponent';
  @State importing: string = '';
  @State importError: string = '';
  @State showUi: boolean = false;   // game build: hide the P1 panel   // game first: the P0/P1 overlay can hide the Vulkan surface
  @State stickLX: number = 0;        // left stick knob offset from its ring's centre (vp)
  @State stickLY: number = 0;
  @State touchControls: boolean = true;  // Touchable-style touch areas (default) instead of the full button pad
  @State hapticsEnabled: boolean = true;  // left-bottom vibration switch

  private lang(): string {
    let value: string = 'en-US';
    try {
      value = i18n.System.getSystemLanguage();
    } catch (e) {
      return 'en-US';
    }
    // The Switch language codes: Simplified and Traditional Chinese have their own tags.
    if (value === 'zh' || value.startsWith('zh-Hans') || value === 'zh-CN' || value === 'zh-SG') {
      return 'zh-Hans';
    }
    if (value.startsWith('zh-Hant') || value === 'zh-TW' || value === 'zh-HK') {
      return 'zh-Hant';
    }
    return value;
  }

  private mb(bytes: number): string {
    return (bytes / 1048576).toFixed(1) + ' MB';
  }

  private rawText(data: Uint8Array): string {
    return new util.TextDecoder('utf-8').decodeToString(data);
  }

  // Unpack the staged C0 modules (rawfile/c0mods) into <filesDir>/dm/mods, once per package version.
  private async unpackC0Mods(files: string): Promise<string> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const rm = ctx.resourceManager;
    let want: string = '';
    try {
      want = this.rawText(await rm.getRawFileContent('c0mods.version')).trim();
    } catch (e) {
      return 'c0: no staged package (P0 build)';
    }
    if (!want) {
      return 'c0: no staged package (P0 build)';
    }
    const modsDir: string = files + '/dm/mods';
    const marker: string = files + '/dm/c0mods.version';
    let have: string = '';
    try {
      have = fileIo.readTextSync(marker).trim();
    } catch (e) {
      have = '';
    }
    if (have === want) {
      return 'c0: mods already unpacked (' + want + ')';
    }
    const manifest: string = this.rawText(await rm.getRawFileContent('c0mods.manifest'));
    const names: string[] = manifest.split('\n').map((s: string) => s.trim()).filter((s: string) => s.length > 0);
    if (!fileIo.accessSync(modsDir)) {
      fileIo.mkdirSync(modsDir, true);
    }
    let count: number = 0;
    for (const name of names) {
      const data: Uint8Array = await rm.getRawFileContent('c0mods/' + name);
      const dst: string = modsDir + '/' + name;
      try {
        fileIo.unlinkSync(dst);
      } catch (e) {
        // absent: fine
      }
      const f = fileIo.openSync(dst, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
      try {
        fileIo.writeSync(f.fd, data.buffer as ArrayBuffer);
      } finally {
        fileIo.closeSync(f.fd);
      }
      count++;
    }
    const mf = fileIo.openSync(marker, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    try {
      fileIo.writeSync(mf.fd, want + '\n');
    } finally {
      fileIo.closeSync(mf.fd);
    }
    return 'c0: unpacked ' + count + ' module files (' + want + ')';
  }

  // Unpack a full development data pack staged by tools/stage_data_pack.sh (rawfile/devpack): RomFS/ goes to
  // <filesDir>/RomFS, every other entry to <filesDir>/dm. Once per package version.
  private async unpackDevPack(files: string): Promise<string> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const rm = ctx.resourceManager;
    let want: string = '';
    try {
      want = this.rawText(await rm.getRawFileContent('devpack.version')).trim();
    } catch (e) {
      hilog.info(DOMAIN, TAG, 'devpack: no devpack.version in the HAP (%{public}s)', JSON.stringify(e));
      return 'devpack: not staged';
    }
    if (!want) {
      return 'devpack: not staged';
    }
    hilog.info(DOMAIN, TAG, 'devpack: staged version %{public}s', want);
    const marker: string = files + '/dm/devpack.version';
    let have: string = '';
    try {
      have = fileIo.readTextSync(marker).trim();
    } catch (e) {
      have = '';
    }
    if (have === want) {
      return 'devpack: already unpacked (' + want + ')';
    }
    const manifest: string = this.rawText(await rm.getRawFileContent('devpack.manifest'));
    const names: string[] = manifest.split('\n').map((s: string) => s.trim()).filter((s: string) => s.length > 0);
    hilog.info(DOMAIN, TAG, 'devpack: unpacking %{public}d files', names.length);
    let count: number = 0;
    let failed: number = 0;
    for (const name of names) {
      const dst: string = name.startsWith('RomFS/') ? files + '/' + name : files + '/dm/' + name;
      try {
        const data: Uint8Array = await rm.getRawFileContent('devpack/' + name);
        const slash: number = dst.lastIndexOf('/');
        this.ensureDir(dst.substring(0, slash));
        try {
          fileIo.unlinkSync(dst);
        } catch (e) {
          // absent: fine
        }
        const f = fileIo.openSync(dst, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
        try {
          fileIo.writeSync(f.fd, data.buffer as ArrayBuffer);
        } finally {
          fileIo.closeSync(f.fd);
        }
        count++;
      } catch (e) {
        // hvigor's resource packaging drops dot-files and the like; keep going and say so
        failed++;
        hilog.error(DOMAIN, TAG, 'devpack: %{public}s failed: %{public}s', name, JSON.stringify(e));
      }
    }
    this.ensureDir(files + '/dm');
    const mf = fileIo.openSync(marker, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    try {
      fileIo.writeSync(mf.fd, want + '\n');
    } finally {
      fileIo.closeSync(mf.fd);
    }
    hilog.info(DOMAIN, TAG, 'devpack: unpacked %{public}d files (%{public}d failed) into %{public}s', count, failed, files);
    return 'devpack: unpacked ' + count + ' files (' + failed + ' failed, ' + want + ')';
  }

  // Recursive size of a picked folder entry. Picker URIs may refuse stat: then 0 is returned and the space
  // check falls back to the copy error (ENOSPC).
  private treeSize(uri: string): number {
    let total: number = 0;
    try {
      const st = fileIo.statSync(uri);
      if (st.isFile()) {
        return st.size;
      }
      if (st.isDirectory()) {
        for (const name of fileIo.listFileSync(uri)) {
          total += this.treeSize(uri + '/' + name);
        }
      }
    } catch (e) {
      return 0;
    }
    return total;
  }

  private ensureDir(dir: string): void {
    if (dir.length === 0 || fileIo.accessSync(dir)) {
      return;
    }
    const slash: number = dir.lastIndexOf('/');
    if (slash > 0) {
      this.ensureDir(dir.substring(0, slash));
    }
    fileIo.mkdirSync(dir);
  }

  // P1 layout: RomFS goes to <filesDir>/RomFS, every other part to <filesDir>/dm/<part> (the host's DM_ROOT).
  private packDest(files: string, part: string): string {
    if (part === 'RomFS') {
      return files + '/RomFS';
    }
    return files + '/dm/' + part;
  }

  // Copy one file from a picker URI into the sandbox. fileIo.copy(uri, uri) works for directories but returns
  // 13900020 for a file behind a picker URI, so the file is opened and copied in 1 MB blocks through its fd.
  private async copyFileFromUri(src: string, dst: string, label: string): Promise<void> {
    const s = fileIo.openSync(src, fileIo.OpenMode.READ_ONLY);
    try {
      const size: number = fileIo.statSync(s.fd).size;
      try {
        fileIo.unlinkSync(dst);
      } catch (e) {
        // absent: fine
      }
      const d = fileIo.openSync(dst, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
      try {
        const block: number = 1048576;
        let off: number = 0;
        while (off < size) {
          const want: number = Math.min(block, size - off);
          const buf = new ArrayBuffer(want);
          const n: number = fileIo.readSync(s.fd, buf, { offset: off, length: want });
          if (n <= 0) {
            break;
          }
          fileIo.writeSync(d.fd, buf.slice(0, n));
          off += n;
          this.importing = '正在导入 ' + label + '：' + Math.floor(off / 1048576) + ' / ' +
            Math.floor(size / 1048576) + ' MB';
        }
      } finally {
        fileIo.closeSync(d.fd);
      }
    } finally {
      fileIo.closeSync(s.fd);
    }
  }

  // Copy one pack part from the picked folder into the app sandbox. Returns false when the part is absent.
  private async copyPart(root: string, part: string, files: string, keepExisting: boolean): Promise<boolean> {
    const src: string = root + '/' + part;
    const dst: string = this.packDest(files, part);
    this.ensureDir(files + '/dm');
    if (keepExisting && fileIo.accessSync(dst)) {
      return true;  // the player's own settings.txt wins over the pack's defaults (as on Android)
    }
    const isFilePart: boolean = part.indexOf('.') >= 0;
    try {
      if (isFilePart) {
        await this.copyFileFromUri(src, dst, part);
      } else {
        try {
          fileIo.rmdirSync(dst);
        } catch (e) {
          // absent: fine
        }
        if (!fileIo.accessSync(dst)) {
          fileIo.mkdirSync(dst, true);
        }
        await fileIo.copy(src, fileUri.getUriFromPath(dst), {
          progressListener: (p: fileIo.Progress) => {
            this.importing = '正在导入 ' + part + '：' + Math.floor(p.processedSize / 1048576) +
              ' / ' + Math.floor(p.totalSize / 1048576) + ' MB';
          }
        });
        // Some systems copy the source directory as a subdirectory of the target: flatten it (as d2r does).
        const inner: string = dst + '/' + part;
        if (fileIo.accessSync(inner)) {
          for (const name of fileIo.listFileSync(inner)) {
            fileIo.renameSync(inner + '/' + name, dst + '/' + name);
          }
          fileIo.rmdirSync(inner);
        }
      }
      return true;
    } catch (e) {
      const code: number = (e as BusinessError).code;
      if (code === ERR_NO_SUCH_ENTRY || code === ERR_NO_SUCH_FILE) {
        return false;
      }
      throw new Error(part + ' (' + code + ')');
    }
  }

  private async importPack(): Promise<void> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const files: string = ctx.filesDir;
    this.importError = '';
    this.importing = '正在选择文件夹…';
    try {
      const opts = new picker.DocumentSelectOptions();
      opts.selectMode = picker.DocumentSelectMode.FOLDER;
      opts.maxSelectNumber = 1;
      const uris: string[] = await new picker.DocumentViewPicker(ctx).select(opts);
      if (!uris || uris.length === 0) {
        this.importing = '';
        return;
      }
      const root: string = uris[0];
      this.importing = '正在检查数据包…';
      // Space check before copying: the parts on the device plus 256 MB of headroom must fit.
      let need: number = 0;
      for (const dir of PACK_DIRS) {
        need += this.treeSize(root + '/' + dir);
      }
      for (const name of PACK_FILES) {
        need += this.treeSize(root + '/' + name);
      }
      const free: number = dmhost.freeSpace(files);
      if (free >= 0 && need > 0 && free < need + 256 * 1024 * 1024) {
        this.importError = 'P1-28 空间不足：需要 ' + this.mb(need) + '，可用 ' + this.mb(free) +
          '（至少还要留 256 MB 余量）';
        this.importing = '';
        return;
      }
      hilog.info(DOMAIN, TAG, 'import: need %{public}s, free %{public}s', this.mb(need), this.mb(free));
      let copied: number = 0;
      for (const part of PACK_DIRS.concat(PACK_FILES)) {
        this.importing = '正在导入 ' + part + '…';
        let ok: boolean = false;
        try {
          ok = await this.copyPart(root, part, files, part === 'settings.txt');
        } catch (e) {
          this.importError = 'P1-29 导入失败：' + (e as Error).message;
          this.importing = '';
          return;
        }
        if (ok) {
          copied++;
        } else if (PACK_REQUIRED.indexOf(part) >= 0) {
          this.importError = 'P1-30 数据包缺少必需项：' + part;
          this.importing = '';
          return;
        }
      }
      const verdict: string = dmhost.validatePack(files);
      const code: number = parseInt(verdict.split(':')[0], 10);
      this.pack = 'pack: ' + verdict;
      if (code !== 0) {
        this.importError = 'P1 校验失败（' + verdict + '）';
        this.importing = '';
        return;
      }
      this.importing = '导入完成（' + copied + ' 项）。关闭应用后重新打开即可载入新数据包';
      hilog.info(DOMAIN, TAG, 'import: done, %{public}s', verdict);
    } catch (e) {
      this.importError = 'P1-31 导入异常：' + JSON.stringify(e);
      this.importing = '';
    }
  }

  // Delete a sandbox file or directory (best effort: a failed import leaves no staging files behind).
  private removePath(p: string): void {
    try {
      if (!fileIo.accessSync(p)) {
        return;
      }
    } catch (e) {
      return;
    }
    try {
      fileIo.rmdirSync(p);
      return;
    } catch (e) {
      // not a directory
    }
    try {
      fileIo.unlinkSync(p);
    } catch (e) {
      // leave it; the next import overwrites the same names
    }
  }

  // Poll the native extraction job (started by dmhost.unzipStart) until it finishes. The archive is read
  // straight from the picker file descriptor, so there is no copy of the .zip in the sandbox.
  private pollUnzip(unpacked: number): Promise<number> {
    return new Promise<number>((resolve) => {
      const timer: number = setInterval(() => {
        try {
          const s: string = dmhost.unzipPoll();
          const p: string[] = s.split(':');
          const state: number = parseInt(p[0], 10);
          if (state === 1) {
            const done: number = parseInt(p[2], 10);
            this.importing = '正在解压 ' + this.mb(done) + ' / ' + this.mb(unpacked);
            return;
          }
          clearInterval(timer);
          if (state === 2) {
            resolve(0);
            return;
          }
          const rc: number = parseInt(p[1], 10);
          this.importError = 'P1-37 解压失败（' + rc + '）：' + p.slice(4).join(':');
          resolve(rc !== 0 ? rc : -1);
        } catch (e) {
          clearInterval(timer);
          this.importError = 'P1-38 解压异常：' + JSON.stringify(e);
          resolve(-1);
        }
      }, 300);
    });
  }

  // Install a data pack from a single .zip without copying the archive into the sandbox: the picked file's fd
  // is handed to native code, which reads entries in place (pread) and writes them to import-tmp. The pack may
  // be at the archive root or inside one top-level folder; the parts are then renamed into the host layout
  // (<filesDir>/RomFS and <filesDir>/dm/<part>), so no second full copy is paid.
  private async importPackZip(): Promise<void> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const files: string = ctx.filesDir;
    const tmp: string = files + '/' + IMPORT_TMP_NAME;
    this.importError = '';
    this.importing = '正在选择 ZIP 数据包…';
    try {
      const opts = new picker.DocumentSelectOptions();
      opts.selectMode = picker.DocumentSelectMode.FILE;
      opts.maxSelectNumber = 1;
      opts.fileSuffixFilters = ['.zip'];
      const uris: string[] = await new picker.DocumentViewPicker(ctx).select(opts);
      if (!uris || uris.length === 0) {
        this.importing = '';
        return;
      }

      this.importing = '正在读取 ZIP 信息…';
      const file = fileIo.openSync(uris[0], fileIo.OpenMode.READ_ONLY);
      let rc: number = 0;
      let unpacked: number = 0;
      try {
        const info: string = dmhost.zipInfo(file.fd);
        const ip: string[] = info.split(':');
        if (parseInt(ip[0], 10) !== 0) {
          this.importError = 'P1-35 无法读取 ZIP（' + info + '）';
          this.importing = '';
          return;
        }
        unpacked = parseInt(ip[1], 10);
        const entries: number = parseInt(ip[2], 10);
        hilog.info(DOMAIN, TAG, 'import zip: %{public}d entries, unpacked %{public}s', entries, this.mb(unpacked));

        // Unpacked pack + the copy of the old pack that is replaced part by part + headroom.
        const free: number = dmhost.freeSpace(files);
        const hasPack: boolean = parseInt(dmhost.validatePack(files).split(':')[0], 10) === 0;
        const need: number = unpacked * (hasPack ? 2 : 1) + 256 * 1024 * 1024;
        if (free >= 0 && free < need) {
          this.importError = 'P1-32 空间不足：解压后 ' + this.mb(unpacked) + '，至少需要 ' +
            this.mb(need) + '，可用 ' + this.mb(free);
          this.importing = '';
          return;
        }

        this.removePath(tmp);
        fileIo.mkdirSync(tmp, true);
        this.importing = '正在解压数据包（大文件需要几分钟）…';
        const started: string = dmhost.unzipStart(file.fd, tmp);
        if (started !== '0') {
          this.importError = 'P1-36 无法开始解压：' + started;
          this.importing = '';
          this.removePath(tmp);
          return;
        }
        rc = await this.pollUnzip(unpacked);
      } finally {
        fileIo.closeSync(file.fd);
      }
      if (rc !== 0) {
        this.removePath(tmp);
        return;
      }

      // The pack tree is at the archive root, or inside one top-level folder (some zip tools add one).
      let root: string = tmp;
      if (!fileIo.accessSync(root + '/nx2a-data.json')) {
        let found: string = '';
        for (const name of fileIo.listFileSync(tmp)) {
          const d: string = tmp + '/' + name;
          if (fileIo.statSync(d).isDirectory() && fileIo.accessSync(d + '/nx2a-data.json')) {
            if (found.length > 0) {
              found = '';
              break;
            }
            found = d;
          }
        }
        if (found.length > 0) {
          root = found;
        }
      }
      if (!fileIo.accessSync(root + '/nx2a-data.json')) {
        this.importError = 'P1-33 ZIP 里没有 nx2a-data.json，不是有效的数据包';
        this.importing = '';
        this.removePath(tmp);
        return;
      }

      this.importing = '正在安装数据包…';
      this.ensureDir(files + '/dm');
      let moved: number = 0;
      for (const part of PACK_DIRS.concat(PACK_FILES)) {
        const src: string = root + '/' + part;
        if (!fileIo.accessSync(src)) {
          if (PACK_REQUIRED.indexOf(part) >= 0) {
            this.importError = 'P1-30 数据包缺少必需项：' + part;
            this.importing = '';
            this.removePath(tmp);
            return;
          }
          continue;
        }
        const dst: string = this.packDest(files, part);
        this.removePath(dst);
        try {
          fileIo.renameSync(src, dst);
        } catch (e) {
          // Renaming across mounts fails; fall back to a copy inside the sandbox.
          if (part.indexOf('.') >= 0) {
            fileIo.copyFileSync(src, dst);
          } else {
            fileIo.mkdirSync(dst, true);
            await fileIo.copy(src, fileUri.getUriFromPath(dst));
          }
        }
        moved++;
      }
      this.removePath(tmp);

      const verdict: string = dmhost.validatePack(files);
      const code: number = parseInt(verdict.split(':')[0], 10);
      this.pack = 'pack: ' + verdict;
      if (code !== 0) {
        this.importError = 'P1 校验失败（' + verdict + '）';
        this.importing = '';
        return;
      }
      this.importing = 'ZIP 导入完成（' + moved + ' 项）。关闭应用后重新打开即可载入新数据包';
      hilog.info(DOMAIN, TAG, 'import zip: done, %{public}s', verdict);
    } catch (e) {
      const be = e as BusinessError;
      this.importError = 'P1-34 ZIP 导入失败（' + be.code + '）：' + be.message;
      this.importing = '';
      this.removePath(tmp);
      hilog.error(DOMAIN, TAG, 'import zip: %{public}s', JSON.stringify(e));
    }
  }

  // Nintendo extension font (button glyphs U+E0E0..): a stand-in generated by dm/tools/make_ext_font.py. Without it
  // the shared-font type 5 falls back to Arial/Roboto, which has no circle-A glyph, so "Press A" draws as a tofu square.
  // It lives in the HAP rawfile and is copied once into the writable sandbox before dmhost.start().
  private async installExtFont(ctx: common.UIAbilityContext): Promise<void> {
    const dir: string = ctx.filesDir + '/dm';
    const dst: string = dir + '/nintendo_ext.ttf';
    let step: string = 'mkdir';
    try {
      if (!fileIo.accessSync(dir)) fileIo.mkdirSync(dir, true);
      step = 'check';
      if (fileIo.accessSync(dst)) return;
      step = 'rawfile';
      const data = await ctx.resourceManager.getRawFileContent('nintendo_ext.ttf');
      step = 'open';
      const f = fileIo.openSync(dst, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
      try {
        step = 'write';
        fileIo.writeSync(f.fd, data.buffer as ArrayBuffer);
      } finally {
        fileIo.closeSync(f.fd);
      }
      hilog.info(DOMAIN, TAG, 'ext font installed: %{public}s', dst);
    } catch (e) {
      const be = e as BusinessError;
      hilog.warn(DOMAIN, TAG, 'ext font install failed at %{public}s: %{public}s', step, be.message ?? JSON.stringify(e));
    }
  }

  private async init(): Promise<void> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const files: string = ctx?.filesDir ?? '';
    try {
      await this.installExtFont(ctx);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'ext font: %{public}s', JSON.stringify(e));
    }
    try {
      this.c0 = await this.unpackC0Mods(files);
      hilog.info(DOMAIN, TAG, 'init: %{public}s', this.c0);
    } catch (e) {
      this.c0 = 'c0: unpack failed: ' + JSON.stringify(e);
      hilog.error(DOMAIN, TAG, 'init: %{public}s', this.c0);
    }
    try {
      const dev: string = await this.unpackDevPack(files);
      hilog.info(DOMAIN, TAG, 'init: %{public}s', dev);
      if (dev.indexOf('not staged') < 0) {
        this.c0 = this.c0 + ' | ' + dev;
      }
    } catch (e) {
      this.c0 = this.c0 + ' | devpack: unpack failed ' + JSON.stringify(e);
    }
    let packOk: boolean = false;
    try {
      const verdict: string = dmhost.validatePack(files);
      this.pack = 'pack: ' + verdict;
      packOk = parseInt(verdict.split(':')[0], 10) === 0;
    } catch (e) {
      this.pack = 'pack: validate failed ' + JSON.stringify(e);
    }
    if (!packOk) {
      this.rc = -1;
      this.showUi = true;
      this.boot = '没有可用的游戏数据包。请点「导入数据包」选择数据包目录，导入完成后重启应用。';
      return;
    }
    try {
      try { dmhost.clearCaptures(); } catch (e) {}
      this.rc = dmhost.start(files, this.lang());
      this.boot = dmhost.bootInfo();
      try {
        const h: number = dmhost.hapticsStart((intensity: number, sharpness: number, epoch: number) => {
          this.applyHaptic(intensity, sharpness, epoch);
        });
        hilog.info(DOMAIN, TAG, 'haptics: native sink %{public}d, hd=%{public}s', h, String(vibrator.isHdHapticSupported()));
        try {
          const infos = vibrator.getVibratorInfoSync();
          hilog.info(DOMAIN, TAG, 'haptics: vibrators %{public}s', JSON.stringify(infos));
        } catch (e) {
          hilog.warn(DOMAIN, TAG, 'haptics: vibrator query failed %{public}s', JSON.stringify(e));
        }
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'haptics: start bridge failed %{public}s', JSON.stringify(e));
      }
      // Debug hook: after the first screen, export runtime-captured shaders to hilog (sandbox is not readable by hdc).
      setTimeout(() => { try { dmhost.dumpCaptures(); } catch (e) {} }, 25000);
    } catch (e) {
      this.rc = -1;
      this.boot = 'dmhost.start threw: ' + JSON.stringify(e);
    }
  }

  // ---- haptics: native intensity/sharpness -> one OHOS continuous pattern ----
  private hapLastI: number = -1;
  private hapLastF: number = -1;
  private hapLastAt: number = 0;
  private hapSupported: boolean = true;

  private stopHaptic(): void {
    try { vibrator.stopVibration(); } catch (e) { }
  }

  private applyHaptic(intensity: number, sharpness: number, epoch: number): void {
    if (!this.hapSupported || !this.hapticsEnabled) return;
    if (!(intensity > 0.01)) {
      if (this.hapLastI !== 0) { this.hapLastI = 0; this.hapLastF = -1; this.stopHaptic(); }
      return;
    }
    const i: number = Math.max(1, Math.min(100, Math.round(intensity * 100)));
    // OHOS frequency is 0..100 with 55 at resonance; Touchable sharpness is 0..1 around 0.5 neutral.
    const f: number = Math.max(5, Math.min(95, Math.round(55 + (sharpness - 0.5) * 80)));
    const now: number = Date.now();
    // Only coalesce the exact same level. The 16 ms floor allows a 60 Hz update, matching the game's HID
    // rate; every change still restarts the one-shot OHOS pattern because the API has no live player.
    if (i === this.hapLastI && f === this.hapLastF) return;
    if (now - this.hapLastAt < 16) return;
    if (this.hapLastI <= 0) hilog.info(DOMAIN, TAG, 'haptics: first output i=%{public}d f=%{public}d', i, f);
    this.hapLastI = i; this.hapLastF = f; this.hapLastAt = now;
    try {
      const builder: vibrator.VibratorPatternBuilder = new vibrator.VibratorPatternBuilder();
      builder.addContinuousEvent(0, 5000, { intensity: i, frequency: f, index: 0 });
      const pattern: vibrator.VibratorPattern = builder.build();
      // Replace the previous pattern: a second start at the same priority otherwise waits for the old one.
      this.stopHaptic();
      vibrator.startVibration({ type: 'pattern', pattern: pattern }, { id: 0, usage: 'physicalFeedback' },
        (err: BusinessError) => {
          if (err) {
            hilog.warn(DOMAIN, TAG, 'haptics: start failed %{public}s', JSON.stringify(err));
          }
        });
    } catch (e) {
      this.hapSupported = false;
      hilog.warn(DOMAIN, TAG, 'haptics: pattern unavailable %{public}s', JSON.stringify(e));
    }
  }

  aboutToAppear(): void {
    this.init();
  }

  aboutToDisappear(): void {
    try { dmhost.hapticsStop(); } catch (e) { }
    this.stopHaptic();
  }

  // One on-screen button: press on touch down, release on up/cancel. The NAPI side keeps an OR of all held buttons,
  // so diagonals and combinations (A+B, L+R, +/−) work.
  @Builder
  padButton(label: string, mask: number) {
    Button(label)
      .fontSize(14)
      .fontWeight(FontWeight.Medium)
      .fontColor('#F2FFFFFF')
      .backgroundColor('#73000000')
      .border({ width: 1, color: '#33FFFFFF' })
      .borderRadius(24)
      .width(46)
      .height(46)
      .padding(0)
      .shadow({ radius: 8, color: '#66000000', offsetY: 2 })
      .stateEffect(true)
      .onTouch((event: TouchEvent) => {
        try {
          if (event.type === TouchType.Down) dmhost.press(mask, true);
          else if (event.type === TouchType.Up || event.type === TouchType.Cancel) dmhost.press(mask, false);
        } catch (e) {
        }
      })
  }

  // Shoulder keys (L/R/ZL/ZR) benefit from a capsule: two characters do not fit a 46 vp circle without ellipsis.
  @Builder
  padPill(label: string, mask: number) {
    Button(label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor('#F2FFFFFF')
      .backgroundColor('#73000000')
      .border({ width: 1, color: '#33FFFFFF' })
      .borderRadius(21)
      .width(66)
      .height(42)
      .padding(0)
      .shadow({ radius: 8, color: '#66000000', offsetY: 2 })
      .stateEffect(true)
      .onTouch((event: TouchEvent) => {
        try {
          if (event.type === TouchType.Down) dmhost.press(mask, true);
          else if (event.type === TouchType.Up || event.type === TouchType.Cancel) dmhost.press(mask, false);
        } catch (e) {
        }
      })
  }

  // Analog input: lx/ly in -32767..32767, screen up/right positive.
  private sendStick(): void {
    const lx: number = Math.round(this.stickLX / 44 * 32767);
    const ly: number = Math.round(-this.stickLY / 44 * 32767);
    try {
      dmhost.stick(lx, ly, 0, 0);
    } catch (e) {
    }
  }

  // A left analog stick in place of the D-pad: the knob follows the finger inside the ring.
  @Builder
  virtualStick() {
    Stack() {
      Circle()
        .width(110)
        .height(110)
        .fill('#26000000')
        .stroke('#40FFFFFF')
        .strokeWidth(1)
      Circle()
        .width(46)
        .height(46)
        .fill('#B3FFFFFF')
        .position({ x: 32 + this.stickLX, y: 32 + this.stickLY })
    }
    .width(110)
    .height(110)
    .onTouch((event: TouchEvent) => {
      const t = event.touches[0];
      if (!t) return;
      let dx: number = t.x - 55;
      let dy: number = t.y - 55;
      const len: number = Math.sqrt(dx * dx + dy * dy);
      if (len > 44) {
        dx = dx * 44 / len;
        dy = dy * 44 / len;
      }
      if (event.type === TouchType.Down || event.type === TouchType.Move) {
        this.stickLX = dx;
        this.stickLY = dy;
        this.sendStick();
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.stickLX = 0;
        this.stickLY = 0;
        this.sendStick();
      }
    })
  }

  build() {
    Stack() {
      XComponent({ id: 'dmhost', type: XComponentType.SURFACE, libraryname: 'dmhost' })
        .width('100%')
        .height('100%')
        .onLoad(() => {
          try {
            this.surface = dmhost.surfaceInfo();
          } catch (e) {
            this.surface = 'surface: ' + JSON.stringify(e);
          }
        })
      Column({ space: 6 }) {
        Text('nx2android · HarmonyOS NEXT · P0/P1')
          .fontColor('#ff66dd66')
          .fontSize(14)
          .fontWeight(FontWeight.Bold)
        Text(`start rc = ${this.rc}`)
          .fontColor(this.rc === 0 ? '#ff8fd48f' : '#ffff8080')
          .fontSize(12)
        Text(this.c0)
          .fontColor('#ffd8c07a')
          .fontSize(12)
        Text(this.pack)
          .fontColor('#ffd8c07a')
          .fontSize(12)
        Text(this.surface)
          .fontColor('#ff9fbfdf')
          .fontSize(12)
        Row({ space: 12 }) {
          Button('导入数据包（文件夹）')
            .fontSize(14)
            .onClick(() => {
              this.importPack();
            })
          Button('导入数据包（ZIP）')
            .fontSize(14)
            .onClick(() => {
              this.importPackZip();
            })
        }
        if (this.importing) {
          Text(this.importing)
            .fontColor('#ffd8d0bc')
            .fontSize(12)
        }
        if (this.importError) {
          Text(this.importError)
            .fontColor('#ffff8080')
            .fontSize(12)
        }
        Scroll() {
          Text(this.boot)
            .fontColor('#ffd8d0bc')
            .fontSize(11)
            .fontFamily('monospace')
            .textAlign(TextAlign.Start)
            .width('100%')
        }
        .scrollBar(BarState.Auto)
        .align(Alignment.TopStart)
        .width('100%')
        .layoutWeight(1)
      }
      .alignItems(HorizontalAlign.Start)
      .padding(12)
      .width('100%')
      .height('100%')
      .visibility(this.showUi ? Visibility.Visible : Visibility.Hidden)
      // Invisible full-window touch catcher above the XComponent: SURFACE XComponent.onTouch does not fire on this
      // device, so the first input bridge maps any screen touch to the A button here.
      Column() {
      }
      .width('100%')
      .height('100%')
      .backgroundColor('#00000000')
      .hitTestBehavior(HitTestMode.Transparent)
      // Touchable-style touch controls (the default) or the full button pad. Both sit above the transparent
      // XComponent catcher; the XComponent surface itself has no usable touch events on this device.
      if (this.touchControls) {
        TouchableOverlay()
          .visibility(this.showUi ? Visibility.Hidden : Visibility.Visible)
      } else {
        Column() {
          Row() {
            this.padPill('L', 1 << 6)
            this.padPill('ZL', 1 << 8)
            Blank()
            this.padButton('−', 1 << 11)
            this.padButton('+', 1 << 10)
            Blank()
            this.padPill('ZR', 1 << 9)
            this.padPill('R', 1 << 7)
          }
          .width('100%')
          .justifyContent(FlexAlign.SpaceBetween)
          Blank()
          Row() {
            // D-pad
            Column({ space: 4 }) {
              this.padButton('↑', 1 << 13)
              Row({ space: 4 }) {
                this.padButton('←', 1 << 12)
                this.padButton('↓', 1 << 15)
                this.padButton('→', 1 << 14)
              }
            }
            Blank()
            // A/B/X/Y
            Column({ space: 4 }) {
              this.padButton('X', 1 << 2)
              Row({ space: 4 }) {
                this.padButton('Y', 1 << 3)
                this.padButton('A', 1 << 0)
                this.padButton('B', 1 << 1)
              }
            }
          }
          .width('100%')
          .justifyContent(FlexAlign.SpaceBetween)
        }
        .width('100%')
        .height('100%')
        // Inset to the 16:9 picture area (2860x1272 screen -> ~300 px black bars on each side), so the pad sits over
        // the game rather than in the letterbox.
        .padding({ left: 86, right: 86, top: 14, bottom: 14 })
        .hitTestBehavior(HitTestMode.Transparent)
        .visibility(this.showUi ? Visibility.Hidden : Visibility.Visible)
      }
      // Small always-available toggles in the left letterbox bar, away from the game picture:
      // UI = import/boot panel, touch/pad = control style, 菜单 = menu touch mode.
      // Keep the debug toggles in the bottom of the left letterbox bar: the rhythm layout's B now sits at
      // the top of that bar, so both must not overlap.
      Column({ space: 4 }) {
        Button('UI')
          .fontSize(8)
          .fontColor('#B3FFFFFF')
          .backgroundColor('#33000000')
          .borderRadius(10)
          .width(50)
          .height(22)
          .padding(0)
          .onClick(() => { this.showUi = !this.showUi; })
        Button(this.touchControls ? '触屏' : '手柄')
          .fontSize(8)
          .fontColor(this.touchControls ? '#FF9EE6A0' : '#B3FFFFFF')
          .backgroundColor('#33000000')
          .borderRadius(10)
          .width(50)
          .height(22)
          .padding(0)
          .onClick(() => { this.touchControls = !this.touchControls; })
        Button(this.hapticsEnabled ? '震动' : '震动关')
          .fontSize(8)
          .fontColor(this.hapticsEnabled ? '#FF9EE6A0' : '#B3FFFFFF')
          .backgroundColor('#33000000')
          .borderRadius(10)
          .width(50)
          .height(22)
          .padding(0)
          .onClick(() => {
            this.hapticsEnabled = !this.hapticsEnabled;
            if (!this.hapticsEnabled) {
              this.hapLastI = -1;
              this.hapLastF = -1;
              this.stopHaptic();
            }
          })
      }
      .width('100%')
      .height('100%')
      .alignItems(HorizontalAlign.Start)
      .justifyContent(FlexAlign.End)
      .padding({ left: 4, bottom: 8 })
      .hitTestBehavior(HitTestMode.Transparent)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#ff080808')
  }
}
