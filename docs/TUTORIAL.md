# touchable-ohos 使用教程

本教程面向两类人：

- **玩家**：已经拥有《节奏天国 奇迹之星》正版游戏文件，想在自己的 HarmonyOS 手机上玩。
- **发布者**：想把这个项目发布出去，让别人用自己的游戏文件转换。

> 本仓库**不包含任何游戏数据**。`out/`、`dist/`、`*.hap`、游戏 RomFS/mods/shaders 都被 git 忽略，
> 不会随仓库发布。

---

## 0. 快速开始（TL;DR）

假设你已经装好 DevEco Studio、Python 3.11+、.NET SDK 10：

```bash
cd touchable-ohos
python3 -m pip install -r requirements.txt

# 首次构建需要本机签名配置（只需一次）
cd app
cp build-profile.template.json5 build-profile.json5
devecocli signature generate --type debug
cd ..

# 用自己的 prod.keys + NSP/XCI 生成数据包和 HAP
python3 tools/build_from_image.py \
  --keys ~/prod.keys \
  --game ~/我的游戏文件夹 \
  --no-embed-pack \
  --archive

# 安装到手机
hdc -t <serial> install -r out/touchable-ohos-0100D9F01D474000-unsigned.hap
```

把 `out/touchable-ohos-0100D9F01D474000-gamepack.zip` 用 USB 拷到手机 `Documents/`，
打开 App → **UI → 导入数据包（ZIP）** → 导入完成后彻底关闭再打开。

---

## 1. 环境准备

### 1.1 必需组件

| 组件 | 版本 | 用途 |
|---|---|---|
| DevEco Studio | 含 HarmonyOS SDK API 26 | 构建 HAP、提供 `devecocli` / `hdc` |
| `devecocli` | 随 DevEco Studio | 构建、签名 |
| `hdc` | 随 SDK | 安装/调试/传文件 |
| Python | 3.11+ | 生成数据包、组装 pack |
| .NET SDK | 10 | 构建 `nx_extract`、运行 shader 翻译器 |
| 真机 | HarmonyOS 7.0 / arm64 | 运行 |

**平台支持**

- 数据包生成：macOS / Windows / Linux 都可以（纯 Python + .NET，不需要 bash）
- HAP 构建与安装：需要对应平台的 DevEco Studio（macOS / Windows）
- Windows 用户把命令里的 `python3` 换成 `python` 或 `py -3`

> **没有 DevEco Studio 的玩家**：不需要装 DevEco。只要 Python 3.11+ 和 .NET SDK 10，
> 用 `--pack-only` 只生成数据包；HAP 直接下载 GitHub Release，用「小白调试助手」等
> 第三方工具安装。见本文第 3.4 节「只生成数据包」。

### 1.2 安装 Python 依赖

```bash
python3 -m pip install -r requirements.txt
```

内容：`lz4`、`capstone`（重写模块必需），`pycryptodome`、`backports.zstd`（可选）。

### 1.3 验证环境

```bash
devecocli --version
hdc list targets
dotnet --version        # 需要 10.x
python3 --version       # 需要 3.11+
```

### 1.4 连接手机

```bash
# USB 连接时直接能看到
hdc list targets

# 无线调试（手机开发者模式里打开无线调试，拿到 IP:端口）
hdc tconn 192.168.1.104:37031
```

> 无线连接适合装应用和小文件，不适合传几个 GB 的数据包（实测约 3 MB/s）。

---

## 2. 准备你自己的游戏文件

你需要：

1. **prod.keys**（从自己的主机提取，或用社区整理的 key 文件）
2. **基础游戏镜像**：NSP 或 XCI，放在一个文件夹里
3. 文件名最好包含 Title ID，例如
   `节奏天国 奇迹之星 [0100D9F01D474000][v0].nsp`

本项目只支持：

```text
Title ID: 0100D9F01D474000
游戏名:   Rhythm Heaven Groove（节奏天国 奇迹之星）
```

如果文件名里没有 Title ID，脚本会尝试从 NCA 头里读，此时需要安装 `pycryptodome`。

---

## 3. 生成数据包和 HAP

### 3.1 一键生成（推荐）

```bash
python3 tools/build_from_image.py \
  --keys /path/to/prod.keys \
  --game /path/to/game-folder \
  --no-embed-pack \
  --archive
```

| 参数 | 说明 |
|---|---|
| `--keys` | prod.keys 路径 |
| `--game` | 放 NSP/XCI 的文件夹 |
| `--out` | 输出目录，默认 `<项目>/out`（git 忽略） |
| `--no-embed-pack` | 只生成 code-only HAP，不把游戏数据塞进 HAP |
| `--archive` | 额外生成单文件 `gamepack.zip`，方便传到手机 |
| `--keep-work` | 保留中间文件（解包、shader 包），便于排查 |

流程（6 步）：

```text
NSP/XCI + prod.keys
  → 解包 ExeFS + 完整 RomFS（.NET 工具 nx_extract）
  → 重写 main / sdk / subsdk0（patch_nso.py，DM_TARGET=ohos）
  → 拆分 shader（shader_split.py）
  → 翻译成 Vulkan SPIR-V（shader_vk.py）
  → 组装 game-pack/
  → 构建 HAP
```

产物：

```text
out/
  game-pack/                                 数据包目录（3.2 GB）
  touchable-ohos-0100D9F01D474000-gamepack.zip  单文件数据包（约 2.6 GB）
  touchable-ohos-0100D9F01D474000-unsigned.hap  未签名 HAP（无游戏数据）
```

> 注意：`--no-embed-pack` 才是“只发布外壳”的推荐做法。默认模式会把 3.2 GB 游戏数据
> 打进 HAP，只适合自己本地测试。

### 3.2 分步生成（已有解包文件时）

如果已经有 `RomFS/`、`mods/`、`shaders.dmsv`，直接用 `make_pack.py`：

```bash
python3 tools/make_pack.py \
  --romfs /path/to/RomFS \
  --mods /path/to/mods \
  --shaders /path/to/shaders.dmsv \
  --out /path/to/game-pack \
  --zip /path/to/game-pack.zip
```

可选参数：`--aoc`、`--sysdata`、`--standard-font`、`--ext-font`、`--lang`、`--version`。

### 3.3 数据包必须包含

```text
mods/            main.patched.bin + fixups/gates 等（必须是 DM_TARGET=ohos 重写的）
RomFS/           游戏 RomFS
shaders.dmsv     Vulkan shader 包
nx2a-data.json   清单（title_id = 0100D9F01D474000）
```

可选：`aoc/`、`sysdata/`、`env.txt`、`settings.txt`、`standard.ttf`、`nintendo_ext.ttf`。

---

### 3.4 只生成数据包（没有 DevEco Studio）

不需要 DevEco Studio、`devecocli`、签名配置，也不需要编译 HAP。只需要：

- Python 3.11+（`pip install -r requirements.txt`）
- .NET SDK 10
- 自己的 `prod.keys` 和 NSP/XCI

命令：

```bash
git clone https://github.com/F7s/touchable-ohos.git
cd touchable-ohos
python3 -m pip install -r requirements.txt

python3 tools/build_from_image.py \
  --keys /path/to/prod.keys \
  --game /path/to/game-folder \
  --pack-only \
  --archive
```

产物：

```text
out/game-pack/                                    数据包目录
out/touchable-ohos-0100D9F01D474000-gamepack.zip  单文件数据包
```

`--pack-only` 在执行到第 5 步（组装 pack）后停止，**不会调用 devecocli**，
也不要求存在 `app/build-profile.json5`。

接着：

1. 从 [GitHub Releases](https://github.com/F7s/touchable-ohos/releases) 下载
   `touchable-ohos-0100D9F01D474000-unsigned.hap`
2. 用 **小白调试助手** 等第三方 HAP 安装工具把它装到手机
   （按工具说明操作；如果提示未签名，用工具自带的调试签名/一键签名功能）
3. 把 `gamepack.zip` 拷到手机 `Documents/`
4. 打开 App → **UI → 导入数据包（ZIP）** → 导入完成后彻底关闭再打开

## 4. 签名与构建（首次需要）

`app/build-profile.json5` 是**机器本地文件**，被 git 忽略；发布出去的仓库只有模板。
所以第一次构建前：

```bash
cd app
cp build-profile.template.json5 build-profile.json5
devecocli signature generate --type debug   # 生成签名材料并写回配置
```

然后构建：

```bash
devecocli build --build-mode debug --assemble-type assembleHap
```

产物：

```text
app/entry/build/default/outputs/default/entry-default-signed.hap
app/entry/build/default/outputs/default/entry-default-unsigned.hap
```

> 用 `--type release` 可以生成 release 签名；正式分发时用你自己的证书重新签名。
> 不要把自己的证书/密码提交到仓库。

---

## 5. 安装到手机

> 没有 DevEco Studio 的玩家：跳过本节，用 GitHub Release 的 HAP + 小白调试助手安装，
> 然后直接从第 6 节开始。

```bash
hdc list targets
hdc -t <serial> install -r out/touchable-ohos-0100D9F01D474000-unsigned.hap
hdc -t <serial> shell aa start -a EntryAbility -b com.nx2a.rhythm
```

如果启动失败并提示：

```text
Error Code:10106102 The device screen is locked during the application launch
```

先解锁手机并保持亮屏，再启动。`hdc` 在锁屏时不能拉起应用。

---

## 6. 把数据包传到手机并导入

### 6.1 传输方式

| 方式 | 建议 | 说明 |
|---|---|---|
| **USB** | ✅ 推荐 | 2.6 GB 几十秒到几分钟 |
| 无线 hdc | ❌ 不推荐 | 实测约 3 MB/s，2.6 GB 要十几分钟 |

把 `out/touchable-ohos-0100D9F01D474000-gamepack.zip` 拷到手机 `Documents/`（或任意文件管理可见目录）。

### 6.2 App 内导入

打开 App，点左下角 **UI**，有两个按钮：

**方式 A：导入数据包（ZIP）** —— 推荐

1. 点 **导入数据包（ZIP）**
2. 在系统文件选择器里选 `gamepack.zip`
3. App 会**直接从你选的文件解压**（引用式解压，不会把 ZIP 再复制一份到沙箱）
4. 等待进度 `正在解压 x / 3359 MB`（大包需要几分钟）
5. 显示 `ZIP 导入完成` 后，**完全关闭 App 再打开**

**方式 B：导入数据包（文件夹）**

1. 把解压好的 `game-pack` 文件夹拷到手机
2. 点 **导入数据包（文件夹）**，选择这个目录
3. 等待拷贝完成，完全关闭 App 再打开

### 6.3 空间要求

| 场景 | 峰值占用 |
|---|---|
| ZIP 导入，设备上还没有数据包 | 解压后 ≈ 3.2 GB |
| ZIP 导入，设备上已有旧包 | 旧包 + 新包 ≈ 6.4 GB |
| 文件夹导入 | 源目录 + 沙箱副本 ≈ 6.4 GB |

ZIP 不支持加密 ZIP 和 ZIP64 容器（>4 GB 或 >65535 个条目的 zip）；普通 store/deflate 压缩都支持。

---

## 7. 目录与产物速查

```text
touchable-ohos/
  app/                 HarmonyOS 工程（ArkTS + NAPI + 预编译 core）
    entry/libs/arm64-v8a/libdmcore.so   随仓库发布的 core
  core/libdmcore.so    core 副本（与 app 内同一份）
  native/              libdmhost.so 的 C 源码（含 ZIP 解压）
  tools/
    build_from_image.py  镜像 → 数据包 + HAP
    make_pack.py         已解包文件 → 数据包
    stage_data_pack.sh   把数据包打进临时 app
  vendor/nx2a/         从 nx2android vendor 的转链工具链（MIT）
  out/                 本地生成的数据包/HAP（git 忽略）
  dist/                本地构建的 HAP（git 忽略）
```

---

## 8. 只要无游戏数据的 HAP

如果你已经有别人给的数据包，或者只想构建外壳：

```bash
cd app
cp build-profile.template.json5 build-profile.json5
devecocli signature generate --type debug
devecocli build --build-mode debug --assemble-type assembleHap
```

生成的 HAP 只有外壳 + core（约 3.9 MB），启动后停在导入界面，不会崩溃。
数据包用 **UI → 导入数据包** 导入即可。

---

## 9. 发布者指南

发布这个项目时：

- ✅ 发布源码即可（`git archive` / 正常 clone）
- ✅ 可以发布 **code-only HAP**（`--no-embed-pack` 生成，约 3.9 MB）
- ✅ `core/libdmcore.so` 和 `app/entry/libs/arm64-v8a/libdmcore.so` 必须随仓库
- ❌ 不要提交 `out/`、`dist/`、`*.hap`、游戏 RomFS/mods/shaders、prod.keys
- ❌ 不要直接压缩整个工作目录发布（会把本地 3.2 GB 数据包带出去）
- ℹ️ 用 `git status --ignored` 自查，确认没有游戏数据被跟踪

---

## 10. 故障排查

| 现象 | 原因 | 解决 |
|---|---|---|
| 打开 App 闪退，core 报 `the module was rewritten for Apple (TARGET 0x0)` | mods 用默认 `DM_TARGET=apple` 生成 | 用 `build_from_image.py` 重新生成（已强制 ohos），或手工 `DM_TARGET=ohos patch_nso.py` |
| core 报 `musl thread slot layout mismatch` | 系统 libc 变了，线程槽位偏移要重测 | 需要改 nx2android 的 `OHOS_TSD_OFF`，不是数据包问题 |
| `Error Code:10106102 ... screen is locked` | 手机锁屏 | 解锁并亮屏后重试 |
| `P1-32 空间不足` | 手机剩余空间不够 | 清理空间；ZIP 需“解压后 + 旧包” |
| `P1-33 ZIP 里没有 nx2a-data.json` | 选错文件/压缩包层级不对 | 选 `game-pack` 根目录所在的 zip |
| `P1-37 解压失败（-3）` | 加密 ZIP / ZIP64 容器 | 用 `make_pack.py --zip` 重新打包 |
| 首次 `devecocli build` 报 `missing build-profile.json5` | 本机签名配置未生成 | 见第 4 节 |
| `hdc file send` 很慢 | 无线 hdc 带宽低 | 改用 USB |
| 导入后启动仍无画面 | 数据包不完整或 shader 不匹配 | 重新生成数据包，确认 `TARGET 0x2` |
| `spawn EPERM`（ohpm/hvigor） | 受限沙箱环境，禁止写 `~/.hvigor`、`~/.npm` | 在普通用户终端里构建 |

### 自查数据包是否健康

```bash
# mods 必须是 ohos 目标
grep TARGET out/game-pack/mods/*.fixups.tsv     # 期望 0x2

# zip 结构
unzip -l out/touchable-ohos-0100D9F01D474000-gamepack.zip | head
unzip -p out/touchable-ohos-0100D9F01D474000-gamepack.zip nx2a-data.json
```

---

## 11. 命令速查

```bash
# 依赖
python3 -m pip install -r requirements.txt

# 首次签名
cd app && cp build-profile.template.json5 build-profile.json5 && devecocli signature generate --type debug && cd ..

# 镜像 → 数据包 + HAP
python3 tools/build_from_image.py --keys prod.keys --game ./game --no-embed-pack --archive

# 已解包 → 数据包
python3 tools/make_pack.py --romfs RomFS --mods mods --shaders shaders.dmsv --out game-pack --zip game-pack.zip

# 构建 code-only HAP
cd app && devecocli build --build-mode debug --assemble-type assembleHap

# 设备
hdc list targets
hdc -t <serial> install -r out/touchable-ohos-0100D9F01D474000-unsigned.hap
hdc -t <serial> shell aa start -a EntryAbility -b com.nx2a.rhythm
hdc -t <serial> shell hilog -x -T nx2a-ohos

# 清理
rm -rf out/ dist/
```

---

## 12. FAQ

**Q: 可以直接玩别人转好的数据包吗？**
数据包包含游戏数据，版权归原权利人。请只使用自己合法拥有的游戏文件。

**Q: 为什么 HAP 要自己签名？**
HarmonyOS 安装应用需要签名。`build-profile.json5` 被 git 忽略，每个用户生成自己的证书。

**Q: 数据包和 HAP 可以混用不同版本吗？**
数据包必须匹配 `nx2a-data.json` 里的 `title_id`、`package`（`com.nx2a.rhythm`，兼容旧 `com.nx2a.dm`）。
mods 必须用 `DM_TARGET=ohos` 生成。

**Q: 支持哪些游戏？**
只支持 `0100D9F01D474000`（Rhythm Heaven Groove / 节奏天国 奇迹之星）。

**Q: 为什么 ZIP 导入不需要先复制 ZIP？**
App 把系统文件选择器返回的文件描述符交给 native 层，native 用 `pread` 直接读取原文件解压，
所以 ZIP 本身不会进入应用沙箱。
