# 设备任务栏胶囊 · Device Taskbar Capsule

**给 FerrisPulse（灵脉）加一枚常驻任务栏的电量胶囊**：设备名 · 渐变进度条 · 百分比，
贴在系统托盘左侧，鼠标一扫即知键鼠耳机电量。

不改宿主一行代码、不碰 HID、零额外耗电——它只是把宿主已经写进注册表的电量缓存
"搬到"任务栏上常驻显示。

> **下载**：[Releases](https://github.com/gallopcsy/FerrisPulse-JiaoNang/releases/latest)　|　**协议**：[MIT](LICENSE)

---

## 为什么做这个

FerrisPulse 是多设备 2.4G/蓝牙电量监视器，但看电量要开面板。日常高频动作
"瞄一眼还剩多少电"，本该和看时间一样顺手。胶囊把这一步压缩成任务栏上的一个常驻数字。

## 特性

- **零竞争只读**：仅读取宿主写入的注册表缓存（`HKCU\Software\RazerBatteryTray`），
  不开 HID、不改宿主、不注入、不 hook 它的代码。
- **单文件极简**：静态编译、无控制台、无运行库依赖，`设备任务栏胶囊.exe` ≈ 315 KB，拷走即用。
- **自动隐身**：宿主运行才出现；宿主退出 2 秒内消失；没有任何有效电量数据时整体隐藏——任务栏零残留。
- **不抢焦点**：`WS_EX_NOACTIVATE` + 点击穿透（胶囊外的区域完全不拦截鼠标）。
- **常驻置顶**：WinEvent 监听任务栏/托盘的窗口重排事件即时重申 `HWND_TOPMOST`，另有 2 秒定时兜底，
  点任务栏、开通知中心都不会把胶囊压没。
- **低电告警**：任一可用设备电量 < 30% 时进度条变红（29 红 / 30 绿，边界有测试）。
- **多设备取最低**：同时接入多台设备时显示"最紧急"的那台，等级冲突按数据新鲜度消解。
- **跟随宿主节奏**：采样间隔默认同步 FerrisPulse 的刷新频率，可手动切 30s / 1min / 5min。

## 快速开始

1. 电脑上装好并运行 **FerrisPulse（灵脉）**，让设备电量写入注册表。
2. 双击 `设备任务栏胶囊.exe` —— 托盘左侧立刻出现胶囊。
3. 右键胶囊 → **开机自动启动**，以后开机常驻。

没有现成 exe？看下面「构建」自己编译，或到仓库 Release 下载。

## 交互

| 操作 | 效果 |
|---|---|
| **左键单击胶囊** | 等价 FerrisPulse 托盘左键：面板可见→隐藏，否则广播唤起宿主面板 |
| **右键单击胶囊** | 弹出菜单 ↓ |

1. **打开 FerrisPulse** —— 只显示窗口，不切换状态
2. **立即刷新** —— 即时重读注册表，不打乱采样相位
3. **自动同步频率** —— 跟随宿主（默认）/ 30秒 / 1分钟 / 5分钟，持久化到同目录 `capsule.config.json`
4. **开机自动启动** —— 写 `HKCU\...\Run`，仅胶囊本体
5. **退出本程序**

## 工作原理

```
 HKCU\Software\RazerBatteryTray      HKCU\Software\FerrisPulse
        （宿主写的电量缓存）               （宿主刷新间隔）
                │                                │
                ▼                                ▼
      src/adapters/RegistryReader     src/adapters/Environment
         只读遍历 21 个键位            FerrisPulse.exe 探活 / 托盘消息
                └────────────┬───────────────────┘
                             ▼
                  src/core/CapsuleCore::Reduce
              纯数据→视图模型（唯一业务接缝，10 组黑盒测试）
                             │  有效性过滤 · 层级消解 · 选最低 · 红线 30% · 截名
                             ▼
                     src/ui/main.cpp
        分层窗口 + GDI+ ARGB 玻璃胶囊 · 托盘左缘锚定 · 置顶守护 · 菜单
```

宿主缺席判定 = 进程名 `FerrisPulse.exe`；唤起/隐藏走注册消息
`FerrisPulse_ActivateExistingInstance`——与宿主源码约定一致，但完全不依赖其内部实现。

## 数据源（只读，21 键位全景）

根：`HKCU\Software\RazerBatteryTray`

- **每设备子键族（4）**：`DeviceState\<sanitized>`（最可信）> `CustomDevices\<id>`
  > `HardwareCache\<id>`、`MchoseCache\Z75S\<HEX>`
- **单键品牌缓存（14）**：Razer(根) / Acer / HyperX / Logitech / Mchose / Mchose G75 /
  Mchose Mouse / Nuphy / Rapoo / SteelSeries / VGN / VGN Keyboard /
  Valkyrie Keyboard / Valkyrie Mouse —— 其中 Logitech、Mchose Mouse、VGN 需 `IsValid=1`
- **值名=设备ID**：`RazerHeadsetCache` = "电量|充电|Ticks"
- **跟随宿主**：`HKCU\Software\FerrisPulse\RefreshInterval`（DWORD 毫秒，缺省 60000）
- **跳过**：AppleCache、UniversalGamingCache、RateWriteCap、HiddenDevices 等非电量值（零 I/O）

业务规则（有效性过滤、层级冲突消解、最低电量评选、红线阈值、24 字节截名、隐藏规则、节奏计算）
全部在 `src/core/CapsuleCore.cpp`，与界面完全解耦、有测试覆盖。

## 构建

需要 MinGW-w64 g++（`winget install -e --id BrechtSanders.WinLibs.MCF.UCRT`）与 PowerShell 7：

```powershell
pwsh -File build.ps1
```

产物：

- `bin\capsule-tests.exe` —— 接缝黑盒测试（零依赖 assert，退出码=失败数）
- `bin\设备任务栏胶囊.exe` —— 绿色单文件主程序（`-static -s -flto`，无控制台）

## 在其他电脑 / 其他设备上使用

胶囊不绑定任何一台机器，换机三步：

1. **前提**：该电脑安装并运行 FerrisPulse（灵脉）。
   胶囊全部数据来自宿主写入的注册表缓存，宿主不在跑时胶囊会自动隐藏（设计如此，不是故障）。
2. **拷一个文件**：把 `bin\设备任务栏胶囊.exe`（≈315 KB，静态编译、免安装、无 VC 运行库）
   复制到目标电脑任意目录，双击即用；右键菜单勾上「开机自动启动」即可常驻。
3. **或者从源码编译**：`git clone <本仓库>` → 装 MinGW-w64 → `pwsh -File build.ps1`。

系统要求：

- Windows 10 / 11 x64（依赖任务栏 `Shell_TrayWnd` / `TrayNotifyWnd` 窗口结构）
- FerrisPulse 支持的任意 2.4G / 蓝牙外设（罗技、雷蛇、Mchose、VGN、瓦尔基里、Nuphy、Rapoo、
  海盗船 HyperX、SteelSeries、宏碁 Acer 等，见上表）
- macOS / Linux 不支持（Win32 专用）

## 项目结构

    src/core/      CapsuleCore.h/.cpp   <- 唯一测试接缝（纯逻辑，零平台依赖）
    src/adapters/  RegistryReader.*     <- 注册表只读遍历（21 键位，表驱动）
                   Environment.*        <- 宿主探活 / 托盘消息 / Run 键 / config.json
    src/ui/        main.cpp             <- 分层窗口、GDI+ 渲染、锚定、置顶守护、菜单
    tests/         test_core.cpp        <- 接缝黑盒测试（10 组，退出码=失败数）
    SPEC.md                             <- 规格书（Q1–Q29 决策冻结）

## 已知偏差（实现期工程决定）

1. **玻璃机制**：原规格的 DWM system backdrop 与像素 alpha 语义不可靠（残留/隐形风险），
   改用分层窗口 + GDI+ ARGB 半透明白色胶囊（`UpdateLayeredWindow`），视觉一致且完全确定。
2. **点击切换近似**：宿主隐藏分支带"保存窗口位置"逻辑，胶囊用 `SW_HIDE` 镜像"隐藏"语义，
   显示分支与宿主一致（广播唤起），用户体验等价。
3. **RazerHeadset 显示名**：该族只存 deviceId 值名（无 DeviceName），胶囊直接显示 deviceId。
4. **单实例**：自带 `Local\DeviceTaskbarCapsule_SingleInstance` 互斥，与宿主的
   `Global\FerrisPulse_*` 互不相干。

## 许可

本项目基于 **MIT License** 开源，详见 [LICENSE](LICENSE)。

Copyright (c) 2026 gallopcsy