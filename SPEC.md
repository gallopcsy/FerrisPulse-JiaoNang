# SPEC — 设备任务栏胶囊（Device Taskbar Capsule）

> triage: ready-for-agent
> 状态: 已完成需求拷问（grilling 两轮共 29 问），前沿清零，等待实现
> 日期: 2026-11-11（Asia/Shanghai）· 目标平台: Windows 11 25H2

---

## Problem Statement

用户在 Windows 11 上运行 FerrisPulse 等开源外设电量程序（2.4G 接收器鼠标、蓝牙耳机等），电量信息只存在于：
- **托盘区小图标/提示**：需要视线离开工作区、鼠标移过去才能看到；
- **主窗口**：整窗打开太重；
- **系统任务栏**：根本没有这些信息的位置。

用户希望在任务栏里**常驻一条胶囊**，像系统原生控件一样随时展示**电量最低的那台设备**（形如 `⚡ ATK A9 Air+ [绿色渐变进度条] 86%`），一眼可见、不占托盘图标位、不打扰。

现状痛点（实测确认）：
1. 现有程序无任何对外数据接口（FerrisPulse 无 IPC），电量无法被第三方程序读取——只有官方预留的注册表缓存可读；
2. 竞品（HaloBattery 等）要求独占 HID，无法与宿主共存；
3. 曾经的 0138 适配方案读取了**根本不存在的电量数据**（设备不回报电量），已作废并完成环境清理。

## Solution

一个**独立的单文件 C++ 绿色程序**「设备任务栏胶囊」：
- 在任务栏底部叠加一条**毛玻璃圆角胶囊**（覆盖窗口，**不注册任何托盘图标**）；
- **只读** FerrisPulse 写入的注册表电量缓存键族，选出**电量最低的已连接设备**展示名称、渐变进度条与百分比，低于 30% 进度条变红；
- **随 FerrisPulse 生命周期**：胶囊自启并监控宿主进程，宿主出现则浮现、退出则隐去；全部设备无活数据时胶囊整体隐藏；
- 单击胶囊 ≡ 单击 FerrisPulse 托盘图标（主窗 Toggle）；悬停无任何交互；
- 右键胶囊弹出**五项精简菜单**：打开FerrisPulse / 立即刷新 / 自动同步频率 / 开机自动启动 / 退出本程序。

## User Stories

1. As an 外设用户, I want 任务栏常驻显示电量最低设备的名称与百分比, so that 我不用移开视线就能掌握最紧张的电量。
2. As an 外设用户, I want 电量以圆角进度条呈现且低于 30% 变红, so that 低电风险一眼可辨、无需读数字。
3. As an 外设用户, I want 进度条使用图示的绿色渐变与毛玻璃半透明背景, so that 胶囊融入 Windows 11 任务栏视觉而不突兀。
4. As an 外设用户, I want 胶囊显示设备名（如 ATK A9 Air+）, so that 多台设备并存时我能确认被展示的是哪一台。
5. As an 外设用户, I want 只显示**电量最低**的那台设备, so that 我永远优先看到最需要充电的外设。
6. As an 外设用户, I want 未连接/注册表无数据的设备**不参与**最低者评选, so that 离线设备不会因历史残值抢占展示位。
7. As an 外设用户, I want 所有设备都无活数据时**整条胶囊隐藏**, so that 没有可信信息时不显示任何可能误导的内容。
8. As an 外设用户, I want 数据源为 FerrisPulse 的注册表缓存且**只读**, so that 胶囊与宿主零冲突、绝不干扰宿主轮询。
9. As an 外设用户, I want 读取 FerrisPulse 写入的**全部键族**（DeviceState、CustomDevices、各品牌缓存）, so that 无论宿主用哪条驱动链路读到设备，胶囊都能看到。
10. As an 外设用户, I want 按官方可信度顺序消解同一设备多键冲突（DeviceState > 每设备子键 > 单键缓存，同级比时间戳）, so that 展示值是最新且最可信的。
11. As an 外设用户, I want 胶囊读取节奏默认**跟随宿主 RefreshInterval**, so that 数据不动我也不空转，符合“按宿主刷新频率”的约定。
12. As an 外设用户, I want 右键菜单「自动同步频率」提供 跟随宿主(默认)/30秒/1分钟/5分钟 四档, so that 我可以在跟随与手动之间切换以平衡及时性与开销。
13. As an 外设用户, I want 频率选择持久化到胶囊自己的 config.json, so that 重启后设置不丢，且不污染宿主的注册表配置。
14. As an 外设用户, I want 单击胶囊的效果与单击 FerrisPulse 托盘图标完全相同（Toggle 主窗口）, so that 胶囊成为托盘图标的等价入口。
15. As an 外设用户, I want **悬停不产生任何交互**（无弹窗、无提示、无动画响应）, so that 胶囊不会因路过任务栏而打扰我。
16. As an 外设用户, I want 右键菜单含「打开FerrisPulse」, so that 我可以从胶囊直接唤出宿主主窗口（走宿主官方激醒消息，无条件显示而非切换）。
17. As an 外设用户, I want 右键菜单含「立即刷新」, so that 我可以在节拍之间主动重读一次缓存（语义=名义刷新，仅重读注册表，不向宿主开新门）。
18. As an 外设用户, I want 右键菜单含「开机自动启动」开关, so that 胶囊可以随系统开机自行运行而无需手动启动。
19. As an 外设用户, I want 「开机自动启动」只控制胶囊自身, so that 宿主的自启策略不受胶囊影响。
20. As an 外设用户, I want 右键菜单含「退出本程序」且只退出胶囊, so that 退出观察者不会误杀宿主。
21. As an 外设用户, I want 胶囊**不注册任何托盘图标**, so that 托盘区保持我指定的零图标状态，全部信息只在任务栏胶囊。
22. As an 外设用户, I want 胶囊监控 FerrisPulse.exe 进程并随其出现而浮现、退出而隐去, so that 宿主不运行时任务栏不残留死胶囊。
23. As an 外设用户, I want 胶囊自身可自启（不依赖宿主拉起）, so that 宿主稍后启动时胶囊已在位待命。
24. As an 外设用户, I want 胶囊只适配 FerrisPulse 单宿主, so that 行为确定、无多源探测开销。
25. As an 系统用户, I want 胶囊为单文件绿色 exe（C++ 原生、目标 <1MB）, so that 存储与内存占用最小、无安装无依赖。
26. As an 系统用户, I want 胶囊使用 Win11 DWM 毛玻璃背景与无边框窗口, so that 与任务栏浑然一体并保持硬件加速渲染。
27. As an 系统用户, I want 胶囊窗口不抢占焦点、不可被 Alt-Tab 捏出, so that 它是纯展示层，不干扰我的窗口管理。
28. As an 系统用户, I want 胶囊自动对齐并锚定到任务栏（跟随任务栏位置/高度/DPI）, so that 移动任务栏或改缩放后胶囊仍正确就位。
29. As an 系统用户, I want 配置存于同目录 config.json 而非注册表, so that 绿色便携、随目录拷走即用。
30. As an 调试者, I want 0138 相关错误数据从环境中彻底消失（适配包与注册表键已删）, so that 胶囊的候选池从第一天起就只含真实数据。
31. As as 用户, I want 项目目录含 SPEC 与 README, so that 后续任何人（含代理）可独立理解设计与验收。
32. As an 外设用户, I want 设备名过长时截断显示而非换行撑破胶囊, so that 胶囊宽度保持稳定、不挤占任务栏其他图标。
33. As an 外设用户, I want 宿主写入异常值（越界、-1、IsValid=0）时该条目直接不参与评选, so that 脏数据不会以“最低电量”身份上屏。

## Implementation Decisions

**总体形态**
- 独立单文件 C++ 绿色 exe + 同目录 config.json + README；项目目录 `2.4G\设备任务栏胶囊`；目标 <1MB；Windows 11 25H2 优先（不做 Win10 分支）。
- 架构三层：**适配层**（Win32 I/O：注册表读取器、进程监控、config、自启 Run 键）→ **唯一接缝** `CapsuleCore::Reduce`（纯数据进、纯视图模型出）→ **表现层**（任务栏覆盖窗口、毛玻璃、右键菜单、单击路由）。全部业务规则只住 Core。

**数据源（只读铁律）**
- 只读 `HKCU\Software\RazerBatteryTray` 根下 FerrisPulse 写入的全部电量键族，**绝不写任何键**（宿主冷启动会回读这些键做休眠恢复，写入会污染宿主）。
- 21 个键位三类结构：①每设备子键（DeviceState、CustomDevices、雷蛇 HardwareCache 设备子键、MchoseCache\Z75S）②单键品牌缓存 13 个（后见覆盖语义）③单键值名=设备ID（RazerHeadsetCache 值为"电量|充电|Ticks"字符串、RateWriteCap 非电量）。
- 取值规则：`BatteryPercent`/`CachedBattery`（DWORD）0-100 有效，-1/缺失无效；`IsValid=1` 才可信的键（罗技、迈从鼠标、VGN）需校验；Apple 85/90 被宿主洗为 -1 属正常；RazerHeadset 需三段解析、Ticks 转时间戳；`ExactBatteryPercent`（雷蛇 SZ）可作精显补充；忽略全部非电量值（Dpi*/PollingRate/Backlight*/SensorId/RateCap 等）；跳过死常量键 AppleCache/UniversalGamingCache（宿主实际走 CustomDevices/DeviceState）。
- 冲突消解：同一设备多键出现按 `DeviceState > 每设备子键 > 单键缓存`，同级比时间戳（ISO-8601 "o" 格式本地时区）；单键缓存无法区分归属时结合 CustomDevices 的 LastSeen 判断最近归属。
- 时间戳：除 RazerHeadset 的 Ticks 外全部为 ISO-8601 往返格式 `DateTime.Now.ToString("o")`。

**展示与评选**
- 仅展示**电量最低**设备；候选池 = 全部键族中有效的已连接设备；无候选 → 胶囊整体隐藏。
- 视图：`⚡ 设备名 [进度条] NN%`；<30% 进度条红色，其余绿色渐变；设备名过长截断。
- 读取节奏默认跟随宿主 RefreshInterval（30s/1m/5m 三档与宿主一致），手动档同集合；节奏只决定“重读注册表”的频率。
- 不做灰显/陈旧线：未连接即不参与（陈旧议题已随 0138 删除作废）。

**交互与生命周期**
- 单击胶囊 ≡ 单击宿主托盘图标（Toggle 主窗，已知通道：广播 `WM_SHOWME = RegisterWindowMessage("FerrisPulse_ActivateExistingInstance")` → 宿主无条件 ShowWindow；Toggle 语义由宿主托盘左键实现）；**悬停零交互**。
- 右键菜单五项及通道：打开FerrisPulse（广播激醒消息）；立即刷新（**名义刷新**：仅立即重读注册表，不给宿主开新门——用户已明确选择，不做宿主补丁）；自动同步频率（四档子菜单，持久化 config.json）；开机自动启动（HKCU Run 键，仅胶囊）；退出本程序（仅胶囊）。
- 覆盖窗口标志：WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT（不抢焦点、不进 Alt-Tab、鼠标点击穿透规则除外——点击需命中，故命中区按菜单/单击需求配置）；锚定任务栏，随位置/高度/DPI 自适应。
- 毛玻璃：DWM 系统背景类型（system backdrop）+ 无边框窗口，圆角自绘。
- 生命周期：Run 键自启 + 监控宿主进程名；宿主进程出现→浮现，退出→隐去。

**配置**
- config.json（同目录）：读取频率档（默认“跟随宿主”）；未来扩展位预留给设置界面（设置界面本期不做）。

## Testing Decisions

- **好测试的定义**：只断言**外部可观察行为**——给定注册表快照/宿主状态/配置输入，断言 Reduce 输出的视图模型（显示哪台设备、百分比、颜色档、隐藏/显示、文案）；不断言内部函数调用、不 mock 实现细节。
- **被测模块**：唯一接缝 `CapsuleCore::Reduce`，覆盖：有效性过滤（-1/越界/IsValid）、三类结构遍历取值、冲突消解优先级与时间戳比较、最低电量评选、全无候选→隐藏、30% 红线边界（29/30/31）、频率档位选择、RazerHeadset 字符串三段解析、超长设备名截断策略。
- **测试载体**：绿地仓库自建**零依赖 assert 测试可执行文件**（与项目“零依赖、最小体积”选型一致），注册表输入以构造的内存快照注入（适配层解析与 Core 之间以结构体交接，测试不碰真实注册表）。
- **先例**：本项目无既有测试；旁证为 FerrisPulse 仓库的 PowerShell 黑盒脚本风格（其 tests 目录曾以反射+断言方式验证行为，本次不沿用其载体，仅沿用“黑盒断言外部行为”的精神）。
- **不进单测**：覆盖窗口、DWM、进程监控、菜单弹出、Run 键写入——以人工验收清单覆盖（清单条目对应 User Stories 14–23、26–28）。

## Out of Scope

- FerrisPulse 之外的任何宿主（LGSTrayBattery HTTP API、HaloBattery 直读等）——首版单宿主，数据源接口留内部抽象即可。
- 独立设置界面（Q16 缓做；能力已并入菜单与 config.json）。
- 给 FerrisPulse 提“外部刷新”补丁/开新门（Q20 已选名义刷新）。
- 灰显/陈旧数据阈值（Q19 议题随 0138 删除作废）。
- 0138（前行者A5）电量方案：设备不回报电量，方案作废；相关适配包与注册表键已从环境删除，不得复活。
- Win10 支持、托盘图标、多设备轮播/列表展示、悬停提示、宿主升级自动跟版的真菜单劫持。
- 对 FerrisPulse 源码的任何修改（仓库已删除；实现期仅从 GitHub 上游读取参照）。

## Further Notes

- 本 spec 是项目第一份文档（绿地），由两轮 grilling 共 29 问定稿；问题追踪器未配置，按用户指示以本文件代替（triage 标记见头部）。
- 实现期参照 FerrisPulse 上游源码（GitHub 可读或克隆本地）核对：托盘菜单项语义、激醒消息名、RefreshInterval 键位置、键族写入点。
- 环境清理已完成并取证：`%APPDATA%\FerrisPulse\adapters\sinowealth_0138.fp-adapter.json` 已删；`DeviceState\FDAS_258A_0138`（94% 错值）已删；`DeviceState\Razer_1532_00EF`（测试残留假 85%）已删；当前 DeviceState 键族为空。
- 曾评估过的方案与其结局：真菜单劫持（弃，菜单位置与投递脆弱）、多宿主数据源（弃，首版单宿主）、宿主刷新补丁（弃，Q20=a）、灰显规则（议题作废）。
- 关键风险已全部验证为可行：注册表缓存为宿主官方预留扩展点（只读安全）；Win11 任务栏叠加有开源先例；托盘点击等价语义有已知消息通道。
