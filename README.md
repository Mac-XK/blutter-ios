# blutter-ios

把 [blutter](https://github.com/worawit/blutter) 移植到 iOS `App.framework` 的改动版，支持 Mach-O 与 Dart 3.12 / 3.13 的 fused AOT 快照。

## 这是什么

blutter 的思路是：编译一份与目标应用版本完全一致的 Dart 运行时，把 Dart AOT 快照载入内存，再用 VM 自身的元数据还原出所有类、函数和对象池。相比纯静态解析，它能拿到函数名、类的继承关系、对象池里每个字符串和常量的归属，是 Flutter 逆向里最省事的一条路。

上游只处理 Android 的 `libapp.so`（ELF，四个快照符号），且适配到 Dart 3.11。iOS 的情况不同：

- `App.framework/App` 是标准 Mach-O，没有 ELF 段表；
- 快照是 fused 形式，只有 `_kDartSnapshotData` 和 `_kDartSnapshotText` 两个符号；
- Dart 3.12 之后 `Dart_InitializeParams` 移除了快照字段，快照改由 `Dart_CreateIsolateGroup` 传入；
- Dart 3.13 删除了 `ObjectStore` 里的 isolate stub 列表，StubCode 全部改名。

本仓库把这些差异都处理掉了，直接对解密的 iOS 包跑，产出与 Android 版一致的 `pp.txt` / `objs.txt` / `asm/`。

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | macOS（Apple Silicon 推荐）或 Linux |
| 编译器 | clang 16+（需要 C++20 `<format>`） |
| 构建 | cmake 3.20+、ninja、pkg-config |
| 依赖库 | icu4c、capstone |
| Python | 3.9+，`pyelftools`、`requests` |
| 网络 | 首次编译需要拉 Dart SDK 源码（sparse checkout 约 72MB） |

macOS 安装依赖：

```bash
brew install cmake ninja pkg-config icu4c capstone
pip3 install pyelftools requests
```

访问 GitHub 较慢时先设代理：

```bash
export https_proxy=http://127.0.0.1:7897 http_proxy=http://127.0.0.1:7897 all_proxy=socks5://127.0.0.1:7897
```

## 快速开始

### 1. 准备输入

从解密后的 IPA 里取出两个二进制，放进任意目录，文件名必须保持 `App` 和 `Flutter`：

```bash
unzip -q app.ipa -d work
mkdir -p blutter_in
cp work/Payload/Runner.app/Frameworks/App.framework/App        blutter_in/App
cp work/Payload/Runner.app/Frameworks/Flutter.framework/Flutter blutter_in/Flutter
```

确认已脱壳（`cryptid` 应为 0）：

```bash
otool -l blutter_in/App | grep -A5 LC_ENCRYPTION_INFO
```

### 2. 编译 Dart 运行时

需要知道目标应用的 Dart 版本和快照 hash。两者都能从二进制里读：

```bash
# Dart 版本
strings -a blutter_in/Flutter | grep -E '\(stable\)'
# 3.13.5 (stable) ... on "ios_arm64"

# 快照 hash：从 _kDartSnapshotData 指向的地址 +0x14 起取 32 字节
otool -l blutter_in/App | grep -A3 "sectname __const"
# 得到 vmaddr 后在该偏移处十六进制查看，前 0x14 字节是头，随后 32 字节即 hash
```

然后编译（首次约十分钟，279 个目标文件）：

```bash
python3 dartvm_fetch_build.py 3.13.5 ios arm64 <SNAPSHOT_HASH>
```

支持的目标组合：

| 参数 | 取值 |
|---|---|
| 版本 | `3.13.5`、`3.12.x`、`3.11.x` 等，需与二进制一致 |
| os | `ios` 或 `android` |
| arch | `arm64` 或 `x64` |

### 3. 运行

```bash
python3 blutter.py blutter_in blutter_out
```

首次会自动编译分析器本体（约一分钟）。之后的运行只做分析，不再重复编译。

输出：

```
Dart version: 3.13.5, Snapshot: 0451907c2eaa8467e848c0067bfe8ed4, Target: ios arm64
flags: product no-asan ... arm64 ios no-compressed-pointers
libapp is loaded at 0x102898000
Dart heap at 0x0
Analyzing the application
Dumping Object Pool
Generating application assemblies
Generating Frida script
```

`Dart heap at 0x0` 在 iOS 上是正常的，无指针压缩时不存在 heap base 位移。

## 命令行参数

| 参数 | 说明 |
|---|---|
| `indir` | 包含 `App`/`Flutter`（或 `libapp.so`/`libflutter.so`）的目录 |
| `outdir` | 输出目录 |
| `--rebuild` | 强制重新编译分析器并重跑分析 |
| `--no-analysis` | 跳过代码分析，只导出对象池与类信息。编译失败或分析器崩溃时的降级路径 |
| `--dart-version 3.13.5_ios_arm64` | 没有 libflutter 时直接指定目标，此时 `indir` 传 `App` 的路径 |
| `--vs-sln` | 生成 Visual Studio 工程（Windows 调试用） |

## 产物怎么看

| 文件 | 内容 | 用途 |
|---|---|---|
| `pp.txt` | 对象池全量 | 按字符串找逻辑的入口，`[pp+0x1dd28] String: "isVip"` 这种行就是池偏移与内容 |
| `objs.txt` | 类与常量对象 | 看类的字段、常量值 |
| `asm/<package>/**.dart` | 逐指令反汇编 + IL 注释 | 主力读物，`bl` 后面直接写被调函数全名 |
| `ida_script/` | IDA 标签脚本 | 想把符号导进 IDA 时用 |
| `blutter_frida.js` | Frida hook 模板 | 运行时验证 |

`asm` 的文件按 Dart package 分目录，包名基本对应 `pubspec.yaml` 里的依赖。业务代码通常在最后一个目录（应用自身的包名），第三方库可以直接跳过。

一个函数长这样：

```
  get _ isVip(/* No info */) {
    // ** addr: 0x9b4a8, size: 0x90
    // 0x9b4c0: LoadField: r0 = r1->field_7
    // 0x9b4d8: r2 = "is_vip_str"
    // 0x9b4e0: GDT[cid_x0 + -0xce9]()
    // 0x9b520: bl  SessionAccount::boolean
```

`** addr` 是该函数在二进制里的偏移。由于 iOS 的 `__TEXT` 段 `vmaddr` 与 `fileoff` 都是 0，这个偏移可以直接当文件偏移在 Hopper / 010 Editor 里跳转。

## 典型分析流程

1. 在 `pp.txt` 里按业务关键词搜字符串，拿到池偏移；
2. 用池偏移在 `asm` 里反查引用，定位所在的 dart 文件与函数；
3. 顺着 `bl` 注释往下看调用链，确认判断条件；
4. 需要动态验证时用 `blutter_frida.js` 里的模板改地址下 hook。

查字符串：

```bash
grep -nE "会员|VIP|订阅" blutter_out/pp.txt
```

查交叉引用：

```bash
grep -rn "isVip" blutter_out/asm/<你的包名>/ | head
```

## 与上游的差异

| 文件 | 改动 |
|---|---|
| `extract_dart_info.py` | 新增 `MachOImage`（`LC_SEGMENT_64` + `LC_SYMTAB` 解析）；从 `_kDartSnapshotData+0x14` 取 hash 与 features；从 Flutter 的 cputype 判架构、正则取 Dart 版本 |
| `blutter.py` | `find_compat_macro` 增加 `NO_HEAP_BITS`、`NO_OBJECT_STORE_STUB_LIST`、`NO_AOT_CLOSURE_OFFSETS` 探测 |
| `dartvm_fetch_build.py` | 兼容 3.13 的 `version_in.cc`；checkout 时不再误删已有 SDK |
| `scripts/CMakeLists.txt` | iOS 目标需同时定义 `DART_TARGET_OS_MACOS` 与 `DART_TARGET_OS_MACOS_IOS` |
| `blutter/CMakeLists.txt` | 新增三个兼容宏的 compile definition |
| `src/ElfHelper.cpp` | 新增 `findSnapshotsMachO`；`MapLibAppSo` 按 magic 分派，替换掉上游不可编译的 Mach-O 分支 |
| `src/DartLoader.cpp` | Dart 3.12+ 快照由 `Dart_CreateIsolateGroup` 传入，用 SFINAE 探测成员做双分支 |
| `src/DartStub.h` | 3.13 删除 ObjectStore stub 列表，补 `*VMStub` 旧名别名 |
| `src/DartApp.cpp` | `loadStubs` 在 3.13 下置空；未知 stub、空候选改为跳过 |
| `src/Disassembler_arm64.h` | `CSREG_DART_HEAP` 仅存在于压缩指针构建 |
| `src/CodeAnalyzer_arm64.cpp` | 解压指针路径在非压缩构建下置空；写屏障 stub 查表加空判断；闭包偏移缺失用哨兵值 |
| `src/DartTypes.cpp` | 未知 abstract type cid 不再 `FATAL` |
| `src/DartDumper.cpp`、`src/VarValue.cpp` | IL 指令与 VarItem 值可能为空，补空判断 |
| `src/FridaWriter.cpp` | `AOT_Closure_context_offset` 缺失时输出 0 |

完整 diff 见 `patches/blutter-ios-3.13.5.diff`。

## 常见问题

| 现象 | 原因与处理 |
|---|---|
| `ELFError: Magic number does not match` | 用的是上游版本。本仓库已支持 Mach-O，确认 `extract_dart_info.py` 是改动版 |
| `dart.cc: #error What operating system?` | CMake 模板缺 `DART_TARGET_OS_MACOS`，检查 `scripts/CMakeLists.txt` |
| 链接报 `dart::Version::String()` 未定义 | `version.cc` 没生成。手动执行 `python3 scripts/dartvm_make_version.py dartsdk/v<版本> <HASH>`，再跑 `dartvm_create_srclist.py` 后重新 configure |
| `Cannot find libapp.so` | 输入目录里文件必须叫 `App` 和 `Flutter` |
| 分析中途段错误 | 先加 `--no-analysis` 确认快照能加载；能加载说明是分析器问题，不是样本问题 |
| `Dart heap at 0x0` | 正常，无指针压缩时没有 heap base |
| 编译 Dart 运行时很慢 | 首次需要编译 279 个源文件，之后有缓存，`--rebuild` 才会重来 |

## 已知限制

- 仅验证 arm64，x64 未测试；
- Dart 3.13 下 `ObjectStore` 无 stub 表，`bl` 到 VM stub 的位置只显示地址，没有符号名；
- 闭包类函数参数识别精度下降（`AOT_Closure_*_offset` 在 3.13 已移除），普通方法不受影响；
- 混淆过的 Dart 代码（`--obfuscate`）函数名会是 `a`、`b` 这类，工具本身无法还原语义，只能靠字符串和调用关系推。

## 目录结构

```
blutter.py                入口
dartvm_fetch_build.py     拉取并编译 Dart 运行时
extract_dart_info.py      版本/快照信息提取
scripts/                  Dart 运行时构建模板
blutter/                  分析器源码（CMake 工程）
patches/                  对上游的完整 diff
docs/port-notes.md        移植过程记录
```

## 致谢

上游 [worawit/blutter](https://github.com/worawit/blutter) 提供了整套思路与实现，本仓库只做了平台与版本适配。

## 许可

沿用上游 MIT 许可，见 `LICENSE`。

## 免责

仅供安全研究与自有样本分析使用。请勿用于未授权的软件修改或分发。
