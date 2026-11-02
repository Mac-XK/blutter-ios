# SYT.Tiantian 2.8.27 — iOS Flutter 逆向笔记（blutter for Mach-O）

样本：`SYT.Tiantian-2.8.27-eeveedecrypter.ipa`（天天朋友圈，`SYT.Tiantian`，3.5.1 / 328）
解压目录：`SYT.Tiantian-2.8.27/Payload/Runner.app`
Dart：`3.13.5 (stable) ios_arm64`，snapshot hash `0451907c2eaa8467e848c0067bfe8ed4`
flags：`product no-asan no-msan no-tsan no-shared_data no-code_comments no-dwarf_stack_traces arm64 ios no-compressed-pointers`

## 1. 二进制布局

| 文件 | 说明 |
|---|---|
| `Runner` (17MB) | Flutter 宿主壳，`cryptid=0` 已脱壳 |
| `Frameworks/App.framework/App` (15MB) | **Dart AOT 全量业务逻辑**，`cryptid=0` |
| `Frameworks/Flutter.framework/Flutter` (9.5MB) | engine，仅用于取 Dart 版本 |
| `App.framework/flutter_assets/` | 静态资源 |

`App` 内部（Mach-O，vmaddr==file offset）：

| 段 | 地址 | 内容 |
|---|---|---|
| `__TEXT,__text` | `0x4000` size `0x79a7d0` | `_kDartSnapshotText`（AOT 机器码） |
| `__TEXT,__const` | `0x7a0000` size `0x6583b0` | `_kDartSnapshotData`（object pool） |
| `__unwind_info` | `0xdf83b0` | 栈展开 |

iOS 是 **fused snapshot**：只有 `_kDartSnapshotData` / `_kDartSnapshotText` 两个符号，
没有 Android 的 `_kDartVmSnapshot*` / `_kDartIsolateSnapshot*`。snapshot 头 20 字节后紧跟
32 字节 hash，再接 features 字符串（与 blutter 的 ELF 读取逻辑一致，故可复用）。

## 2. blutter 移植改动（~/tools/blutter）

上游 blutter 只吃 Android ELF，且只适配到 Dart 3.11。为 3.13.5 + Mach-O 所做改动：

| 文件 | 改动 |
|---|---|
| `extract_dart_info.py` | 新增 `MachOImage`（LC_SEGMENT_64 + LC_SYMTAB 解析），从 `_kDartSnapshotData+20` 取 hash/flags；从 Flutter 的 `cputype` 判 arch、正则取 Dart 版本 |
| `blutter.py` | `find_compat_macro` 新增 `NO_HEAP_BITS`、`NO_OBJECT_STORE_STUB_LIST`、`NO_AOT_CLOSURE_OFFSETS` 探测 |
| `dartvm_fetch_build.py` | 兼容 3.13 的 `version_in.cc`（`version.cc` 需生成）；CMake 模板补 `DART_TARGET_OS_MACOS` |
| `scripts/CMakeLists.txt` | iOS 目标同时定义 `DART_TARGET_OS_MACOS;DART_TARGET_OS_MACOS_IOS` |
| `blutter/CMakeLists.txt` | 新增上述 3 个 compat 宏的 `set(defines ...)` |
| `src/ElfHelper.cpp` | 新增 `findSnapshotsMachO()`；`MapLibAppSo()` 改为按 magic（`0xfeedfacf`/`ELF`）分派，替换掉原有不能编译的 Mach-O 分支 |
| `src/DartLoader.cpp` | Dart ≥3.12 把 snapshot 从 `Dart_InitializeParams` 移到 `Dart_CreateIsolateGroup`，用 SFINAE 探测成员做双分支 |
| `src/DartStub.h` | ObjectStore stub 列表在 3.13 已删除；补 `*VMStub` 旧名别名 |
| `src/DartApp.cpp` | `loadStubs()` 在 3.13 下置空；未知 stub、空候选 stub 改为跳过 |
| `src/Disassembler_arm64.h` | `CSREG_DART_HEAP` 只在 `DART_COMPRESSED_POINTERS` 下存在（iOS 从无压缩指针） |
| `src/CodeAnalyzer_arm64.cpp` | 压缩指针反解路径在非压缩构建下置空；write barrier 的 stub 查表加空判断；closure 偏移缺失时用哨兵值 |
| `src/DartTypes.cpp` | 未识别的 abstract type cid（实测 172）不再 FATAL |
| `src/DartDumper.cpp` / `VarValue.cpp` | IL 指令与 VarItem 值可能为空，全部加空判断 |

复现：

```sh
export https_proxy=http://127.0.0.1:7897 http_proxy=http://127.0.0.1:7897 all_proxy=socks5://127.0.0.1:7897
python3 ~/tools/blutter/dartvm_fetch_build.py 3.13.5 ios arm64 0451907c2eaa8467e848c0067bfe8ed4
python3 ~/tools/blutter/blutter.py --rebuild blutter_in blutter_out_asm
```

## 3. 产物（blutter_out_asm/，116MB）

| 文件 | 内容 |
|---|---|
| `pp.txt` | object pool 全量（字符串、常量、函数指针） |
| `objs.txt` | 类/常量对象 dump |
| `asm/ttpyq/**` | **1359 个 .dart 文件**，含逐指令反汇编 + IL 注释（`bl #0x...` 直接标出被调函数全名） |
| `ida_script/` | 供 IDA 导入的符号脚本 |
| `blutter_frida.js` | Frida hook 模板 |

## 4. 会员/VIP 逻辑定位

| 位置 | 地址 | 说明 |
|---|---|---|
| `features/auth/domain/session_account.dart` `get isVip` | `0x9b4a8` | 会话账户上的 VIP 判断入口（读 `field_7`） |
| `features/poster/domain/poster_template.dart` `get vipOnly` | `0x309e74` | 模板是否 VIP 专属 |
| `features/poster/presentation/poster_detail_page.dart` `_PosterDetailPageState._setVip` | `0x4eb730` | 详情页设置 VIP 态的 async 方法，闭包 `0x4ebaf0` |
| `features/ads/presentation/quota_reward_dialog.dart` `offerQuotaReward` | `0x99f14` | 看广告换额度（`QuotaRewardKind`） |

相关字符串（pp.txt）：`"isVip"`、`"vip"`、`"VIP 专属模板"`、`"此模板需要 VIP 会员"`、`"VIP 素材保存不限次数"`、`"天 VIP 会员，需"`。

服务端接口（`.html` 后缀，老 PHP 后端）：`member_info.html`、`member_info_three.html`、`Point_exchange_vip`、`/extension/ai/quota`、`/j/wechatGroup/getUserQuota`，base `https://api-v2.360pyq.com`。

**结论：客户端只做 UI gating，VIP 真值由 `/Main/Member/member_info*` 返回并缓存在 `SessionAccount`。**

## 5. 其它发现

- 编译期泄漏路径：`/Users/wengshouyu/ttpyq-ios-build-main/.dart_tool/...` → 工程名 `ttpyq-ios-build-main`
- Dart 侧未混淆（1399 条 `package:` 路径可读）
- 原生 SDK：AnyThink / GDTMob / Smartdigimkt / 穿山甲(CSJ) / AlipaySDK / Tquic

## 6. VIP 判断点全表（blutter 定位）

| 符号 | 地址 | 逻辑 |
|---|---|---|
| `SessionAccount.get isVip` | `0x9b4a8` | 取 map `"is_vip_str"`，为空再取 `"is_vip"`，交 `SessionAccount::boolean` 归一 |
| `SessionAccount.get isLocked` | `0xa5104` | `"is_lock"` / `"status"`，**账号封禁**，与 VIP 无关 |
| `SessionAccount::boolean` | `0x9b384` | 通用字符串→bool，29 处调用，不可整体改 |
| `UserProfile.get vip` | `0x316390` | 取 `"is_vip"` → boolean，用于主页 VIP 标识 |
| `PosterTemplate.get vipOnly` | `0x309e74` | 取模板 map `"isVip"`/`"is_vip"` → 模板是否 VIP 专属 |
| `DateStyle.get vip` | `0x2d9764` | 取样式 map `"isVip"` → 样式是否 VIP 专属 |
| `GroupQuota.remainingViews` | `0x387014` | `vipDayViewQuota`/`userDayViewQuota` − `todayViewed` |
| `GroupQuota.remainingCreates` | `0x4c053c` | `vipTotalCreateQuota`/`userTotalCreateQuota` − `totalCreated` |

布尔常量编码：`add x0, x22, #0x20` = true，`#0x30` = false（x22 = NULL_REG）。

## 7. Patch 结果

| 偏移 | 原指令 | 补丁 | 效果 |
|---|---|---|---|
| `0x9b4a8` | `stp fp,lr,[sp,#-0x10]!` | `add x0,x22,#0x20; ret` | `isVip` 恒为 true |
| `0x316390` | 同上 | `add x0,x22,#0x20; ret` | `UserProfile.vip` 恒 true |
| `0x309e74` | 同上 | `add x0,x22,#0x30; ret` | 模板不再算 VIP 专属 |
| `0x2d9764` | 同上 | `add x0,x22,#0x30; ret` | 样式不再算 VIP 专属 |
| `0x38706c` | `sub x2,x1,x0` | `mov x2,x1` | 剩余浏览数不再扣今日已用 |
| `0x4c0594` | `sub x2,x1,x0` | `mov x2,x1` | 剩余创作数不再扣已用 |

产物：`SYT.Tiantian-2.8.27-vip-patched.ipa`（ad-hoc 签名，需自签后安装）、`tiantian_vip.js`（Frida 运行时补丁，免改包）。

限制：服务端 `member_info` 仍返回非会员，涉及服务端校验的付费/权益接口不受影响。

## 8. 安装问题排查

### Apple 登录失败
原因：首次打包用 `codesign --force --deep -s -` 做 ad-hoc 签名，**丢掉了 entitlements**。
原包 entitlements（`codesign -d --entitlements :-`）：

```
application-identifier            = 7C84HD28CB.SYT.Tiantian
com.apple.developer.applesignin   = ["Default"]
com.apple.developer.associated-domains = ["applinks:new.360pyq.com"]
com.apple.developer.team-identifier = 7C84HD28CB
```

`sign_in_with_apple` 依赖 `com.apple.developer.applesignin`，缺失即 `ASAuthorizationError` 失败。

修复：改用 `ldid -Sentitlements.plist` 重签（TrollStore/越狱路径），已重新打包；
用自有证书签名时走 `resign.sh`，entitlements 从 mobileprovision 提取。

### 会员有效期显示「未开通」
`_MembershipPageState._expiry(raw)` @ `0x3e0f44`：
`int.tryParse(raw)` → null 或 `<=0` 直接返回 `"未开通"`；否则 `秒*1000 → DateTime → yyyy-MM-dd`。
服务端对补丁账号返回 0/空，故显示未开通。

补丁（`0x3e0f80`）：
```
movz x1, #0xb400
movk x1, #0x7a45, lsl #16   ; x1 = 0x7a45b400 (2035-01-01 前后)
b    0x3e0f9c               ; 跳过 "未开通" 分支，直接走格式化
```

### 去广告未生效
广告全部由服务端开关控制，`AppConfig::feature("<xxx>_ad_switch")` @ `0x940bc` 返回 bool，5 个开关：
`splash_screen_switch` / `interstitial_ad_switch` / `banner_ad_switch` / `native_ad_switch` / `motivational_video_switch`。

| 位置 | 原指令 | 补丁 |
|---|---|---|
| `NativeInterstitialGateway.configured` `0x371f9c` | `stp fp,lr,...` | `add x0,x22,#0x30; ret` → 插屏/开屏恒关 |
| `SourceBanner._build` 判定点 `0x2b520c` | `tbz w0,#4,0x2b5224` | `b 0x2b5210` → 恒返回空 SizedBox |
| `SourceFeedAd._build` 判定点 `0x2b5754` | `tbz w0,#4,0x2b576c` | `b 0x2b5758` → 恒返回空 SizedBox |
| `NativeRewardGateway._show` `0x939ec` | `tbnz w0,#4,...` | 未改（用户主动触发，保留看广告换额度） |
