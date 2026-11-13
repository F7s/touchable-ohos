# touchable-ohos

Touchable 风格节奏天国（Rhythm Heaven Groove）HarmonyOS 独立项目。

- 游戏：Rhythm Heaven Groove（Title ID `0100D9F01D474000`）
- 应用名：节奏天国 奇迹之星
- Bundle：`com.nx2a.rhythm`
- 目标：HarmonyOS 7.0 / API 26 / arm64
- 本仓库不包含任何游戏数据

## 文档

- 完整使用教程：[docs/TUTORIAL.md](docs/TUTORIAL.md)（环境、签名、构建、导入、故障排查）
- 给 AI agent 的操作手册：[SKILL.md](SKILL.md)

## 目录

```text
touchable-ohos/
  app/                 HarmonyOS 应用工程（ArkTS + NAPI + 预编译 core）
  core/libdmcore.so    节奏天国用 native core（与 app 内同一份）
  native/              HarmonyOS 平台层（libdmhost.so 的 C 源码）
  tools/
    build_from_image.py  从游戏镜像一键生成数据包 + 未签名 HAP
    make_pack.py         从已解包文件组装数据包（纯标准库）
    stage_data_pack.py   把数据包打进临时 app 工程（跨平台）
  vendor/nx2a/         从 nx2android vendor 的移植工具链（MIT，见其 README）
  requirements.txt     从镜像构建时的 Python 依赖
  out/                 本地生成的 game-pack 和 HAP（git-ignored，不随仓库发布）
  README.md
```

`app/entry/libs/arm64-v8a/libdmcore.so` 是预编译的 Rhythm Heaven core，
由 nx2android 的 HarmonyOS 构建链生成，随仓库发布（没有它 app 无法构建）。
这个仓库只发布应用外壳和 core，不包含游戏 RomFS、mods、shaders 或存档。
`out/` 和 `dist/` 已在 `.gitignore` 中，生成的数据和 HAP 不会被提交或发布。

## 环境

- DevEco Studio（含 HarmonyOS SDK API 26）
- 命令行工具：`devecocli` / `hdc`
- 真机：HarmonyOS 7.0，arm64
- 只有“从游戏镜像生成”才额外需要：Python 3.11+（`pip install -r requirements.txt`）、.NET SDK 10
- 构建与应用安装需要对应平台的 DevEco Studio；**从镜像生成数据包本身在 macOS 和
  Windows 上都能跑**（全部为 Python + .NET，不依赖 bash）

## 构建

首次构建先准备本机签名配置（`app/build-profile.json5` 是机器本地文件，不随仓库发布）：

```bash
cd app
cp build-profile.template.json5 build-profile.json5
devecocli signature generate --type debug   # 生成自己的签名材料并写回配置
```

然后构建：

```bash
cd app
devecocli build --build-mode debug --assemble-type assembleHap
```

产物：

```text
app/entry/build/default/outputs/default/entry-default-signed.hap
```

release 构建（或分发给别人）时重新生成 release 签名：

```bash
cd app
devecocli signature generate --type release
devecocli build --build-mode release --assemble-type assembleHap
```

签名配置在 `app/build-profile.json5`，不要把个人证书提交到公开仓库。

### Windows 说明

- 用 DevEco Studio 的 Windows 版（自带 `devecocli` / `hdc`），命令相同；
- Python 一般用 `python` / `py -3` 代替文档里的 `python3`；
- 不需要 Git Bash / WSL：数据包生成链路已全部是 Python + .NET；
- 首次构建会通过 NuGet 还原 `nx_extract` 的依赖（需要网络或已有的 NuGet 缓存）。

## 安装与启动

```bash
hdc -t <serial> install -r entry-default-signed.hap
hdc -t <serial> shell aa start -a EntryAbility -b com.nx2a.rhythm
```

## 导入游戏数据

本仓库不带游戏数据。应用启动时如果没有有效数据包，会停在导入界面，
不会启动 core，也不会闪退。

1. 把数据包放到手机的 `Documents/`，例如：

   ```text
   Documents/game-pack/
   ```

2. 打开应用，点左下角 **UI**
3. 点 **导入数据包（文件夹）**，选择这个目录
   （或把 `--zip` 生成的单文件 `.zip` 拷到手机，点 **导入数据包（ZIP）**：应用直接从你选的文件解压，
   **不会把 ZIP 再复制一份到应用沙箱**）
4. 等导入完成（大包解压需要几分钟，界面有进度）
5. 完全关闭应用后重新启动

数据包至少需要：

```text
mods/
RomFS/
shaders.dmsv
nx2a-data.json
```

可选：

```text
aoc/  sysdata/  env.txt  settings.txt  standard.ttf  nintendo_ext.ttf
```

ZIP 导入的临时占用：只多出解压后的数据包（约 3.2 GB）；如果设备上已有旧包，解压期间新旧包会同时
存在（约 6.4 GB），导入完成后旧包被替换。

`nx2a-data.json` 的 `title_id` 必须是：

```text
0100D9F01D474000
```

## 生成数据包

工具：

```text
tools/make_pack.py
```

它把已经提取好的游戏文件组装成应用接受的 pack：

```bash
python3 tools/make_pack.py \
  --romfs /path/to/RomFS \
  --mods /path/to/mods \
  --shaders /path/to/shaders.dmsv \
  --out /path/to/game-pack
```

可选参数：

```text
--aoc <dir>             DLC
--sysdata <dir>         系统数据
--standard-font <ttf>   系统字体
--ext-font <ttf>        按键图标字体
--lang en-US
--version 1.0.0
--zip /path/to/game-pack.zip   同时生成单文件压缩包（App 用「导入数据包（ZIP）」直接解压导入）
```

输出目录会生成：

```text
game-pack/
  mods/
  RomFS/
  shaders.dmsv
  env.txt
  nx2a-data.json
```

然后把这个目录（或 `--zip` 生成的压缩包）导入应用。

也可以直接复用 nx2android 已经生成的 pack：

```text
ohos-feasibility/game-pack/
```

## 从游戏镜像生成未签名 HAP

```text
tools/build_from_image.py
```

**不需要任何其他仓库**：解包、改模块、拆/翻译 shader 的工具链已 vendor 在
`vendor/nx2a/`（来自 nx2android，MIT）。需要：

- `prod.keys`
- 包含基础 NSP/XCI 的文件夹
- Python 3.11+ 及 `pip install -r requirements.txt`
- .NET SDK 10（构建 `nx_extract`、运行 `dmsh_xlate`）
- DevEco Studio / `devecocli`（生成数据包不需要，构建 HAP 需要）

macOS / Windows / Linux 均可；不需要 bash、Xcode 或 WSL。

命令：

```bash
python3 tools/build_from_image.py \
  --keys /path/to/prod.keys \
  --game /path/to/game-folder
```

`--out` 可省略，默认输出到 `out/`（已在 .gitignore 中）：

```bash
python3 tools/build_from_image.py ... --out /path/to/output
```

流程：

```text
NSP/XCI + keys
  → ExeFS + full RomFS
  → patch main / sdk / subsdk0
  → shader_split + shader_vk
  → shaders.dmsv
  → game-pack/
  → 嵌入 app → hvigor release 构建
```

输出（默认在 `out/`）：

```text
out/game-pack/
out/touchable-ohos-0100D9F01D474000-unsigned.hap
```

只生成外壳和独立数据包，不把游戏数据放进 HAP：

```bash
python3 tools/build_from_image.py ... --no-embed-pack
```

此时 HAP 是 code-only，启动后通过 **UI → 导入数据包（文件夹）** 导入
`out/game-pack`。

也可以同时生成单文件数据包，方便传到手机：

```bash
python3 tools/build_from_image.py ... --no-embed-pack --archive
```

输出 `out/touchable-ohos-0100D9F01D474000-gamepack.zip`，在 App 里通过
**UI → 导入数据包（ZIP）** 导入：应用拿到 picker 返回的文件描述符后由 native 直接解压到沙箱，
ZIP 本身不会被复制（手机端只需要把单文件传进去）。

## 独立构建说明

本仓库的所有构建路径都只依赖仓库自身：

| 目标 | 命令 | 外部依赖 | 平台 |
|---|---|---|---|
| 无游戏数据 HAP | `cd app && devecocli build ...` | DevEco SDK | macOS / Windows |
| 从已解包文件组装数据包 | `tools/make_pack.py` | 无（标准库） | 全平台 |
| 从游戏镜像生成数据包 + HAP | `tools/build_from_image.py` | Python 包 + .NET SDK + devecocli | macOS / Windows / Linux |

`vendor/nx2a/` 是唯一的外部代码来源，已连同 MIT 许可证一起复制进仓库，
升级方式见 `vendor/nx2a/README.md`。

## 发布与最终用户流程

本仓库面向“发布源码 + 用户用自己的游戏转换”的模式：

**发布方（仓库作者）**

- 发布仓库源码即可（`git archive` / 正常 clone），`out/`、`dist/`、
  `*.hap`、`__pycache__/`、`nx_extract/obj` 都已被 git 忽略；
- 不要用“直接压缩整个工作目录”的方式发布，否则会把本地生成的
  `out/game-pack`（3 GB+ 游戏数据）一起带出去；
- `app/entry/libs/arm64-v8a/libdmcore.so` 是必须随仓库发布的预编译 core。

**最终用户**

1. `python3 -m pip install -r requirements.txt`
2. 安装 DevEco Studio / HarmonyOS SDK / `devecocli`、.NET SDK 10
3. 准备自己的 `prod.keys` 和 NSP/XCI 文件夹
4. 转换：

   ```bash
   python3 tools/build_from_image.py \
     --keys ~/prod.keys \
     --game ~/my-game-folder \
     --no-embed-pack
   ```

5. 得到 `out/touchable-ohos-0100D9F01D474000-unsigned.hap` 和
   `out/game-pack/`；加 `--archive` 还会得到单文件
   `out/touchable-ohos-0100D9F01D474000-gamepack.zip`；
6. HAP 用 `devecocli signature generate` 签自己的证书后安装；
   数据包通过 **UI → 导入数据包（文件夹）** 或 **（ZIP）** 导入，
   ZIP 只需要把那个单文件传到手机。

游戏数据、字体和 shader 包的版权归原权利人所有。仓库不分发数据包，
数据包只存在于用户本地被 git 忽略的 `out/` 中。

## 功能

- Touchable 风格 direct menu / 触屏布局（ONE / TWO / DPAD / PAD）
- 混奏关卡的动态 D-pad（按场景方向并集切换）
- Touchable 音频 renderer + mixer（OHAudio FAST）
- HarmonyOS 震动后端，左下角可开关
- Ambient bars 黑边渐变
- Vulkan GPU deswizzle + GPU BC7 解码（Maleoon 920C）
- CPU BC7 / deswizzle 多线程 fallback
- 无数据包时显示导入界面，不崩溃
- 数据包支持文件夹导入，或单个 `.zip` 导入（`tools/make_pack.py --zip`）；ZIP 走引用式解压，
  不复制压缩包本身（支持 store/deflate，不支持加密 ZIP 和 ZIP64 容器）

## 故障排查

**打开应用闪退，日志里 core 报 `FATAL ... the module was rewritten for Apple (TARGET 0x0)`**

数据包里的 `mods/*.fixups.tsv` 是用 `DM_TARGET=apple`（默认值）重写的，HarmonyOS 的 core 只接受
`# TARGET 0x2`。用 `tools/build_from_image.py` 重新生成数据包即可（它会设置 `DM_TARGET=ohos`
并校验 `TARGET 0x2`）。手工跑 `patch_nso.py` 时必须带这个环境变量：

```bash
DM_GAME=0100D9F01D474000 DM_TARGET=ohos python3 vendor/nx2a/nxnative/dm/tools/patch_nso.py <main> <out> main
```

**打开应用闪退，日志里 core 报 `FATAL ... musl thread slot layout mismatch`**

core 的线程槽位偏移是按某版 musl 测的，系统 libc 变了。需要重新测量并更新 `DM_TARGET=ohos`
的偏移（nx2android 侧修改 `patch_nso.py` 的 `OHOS_TSD_OFF`），不是数据包问题。

## 已知问题

- 混奏饵食关卡抛物线时仍有帧节奏卡顿，未完全解决
- 大纹理首次上传仍有 `pkg_hash` / BC7 源数据拷贝开销
- `TWO` / `PAD` 布局未逐关验证
- 震动延迟高于 Touchable，pattern restart 可能有断感

详细移植记录见 nx2android 仓库的
`nxnative/dm/harmony/PORTING_RETROSPECTIVE.md`。

## 第三方

本项目参考/移植了以下开源项目（完整说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）：

- **Touchable**（菜单/音频/震动/触屏层参考），MIT，Copyright (c) mzhao
- **d2r-mobile**（HarmonyOS 工程结构、EntryAbility/Index 页面布局、native CMake 结构），MIT，
  Copyright (c) 2026 jhaizhou，https://github.com/jhaizhou-ops/d2r-mobile
- **nx2android / nxnative**（vendor 的转链工具链），MIT，Copyright (c) 2026 seedds，
  https://github.com/seedds/nx2android
- **Ryujinx**（`Ryujinx.Graphics.Shader` / `Ryujinx.Common`，随 vendor 的 shader 翻译器），MIT
- **bcn_layer `shader-v3` / Granite** 的 `bc7.comp`，MIT
- 构建期依赖：nx_extract 使用 LibHac（MIT）

## 许可证

- 本项目：[MIT](LICENSE)，Copyright (c) 2026 lizhenghao
- 第三方组件（Touchable、nx2android / nxnative、Ryujinx 等）：见
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## 版本

- 1.0.0 / versionCode 1000000
