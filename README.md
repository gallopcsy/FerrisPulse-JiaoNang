# 设备任务栏胶囊（FerrisPulse 任务栏外挂）

在任务栏右侧（系统托盘左侧）常驻一枚胶囊：**⚡ 设备名 · 绿色渐变进度条 · NN%**。
只读 FerrisPulse 的注册表电量缓存，零 HID 竞争、不改宿主一行代码。
规格书见同目录 SPEC.md（Q1–Q29 全部决策已冻结）。

## 构建

需要 MinGW-w64 g++（已装：winget install -e --id BrechtSanders.WinLibs.MCF.UCRT）。

    pwsh -File build.ps1

产物：

- bin\capsule-tests.exe —— 接缝 CapsuleCore::Reduce 黑盒测试（零依赖 assert，退出码=失败数）
- bin\设备任务栏胶囊.exe —— 绿色单文件主程序（-static -s，无控制台）

## 运行

- 双击设备任务栏胶囊.exe；FerrisPulse 在跑且注册表有电量数据时自动出现。
- **左键单击胶囊** ≡ FerrisPulse 托盘左键（可见→隐藏，否则广播 WM_SHOWME 唤醒）。
- **右键单击** 打开菜单：
  1. 打开 FerrisPulse（仅广播 WM_SHOWME，只显示不切换）
  2. 立即刷新（即时重读注册表，不打乱采样相位）
  3. 自动同步频率 → 跟随宿主（默认）/ 30秒 / 1分钟 / 5分钟 → 写入同目录 capsule.config.json
  4. 开机自动启动（HKCU\...\Run，仅胶囊本体）
  5. 退出本程序
- 宿主退出 → 胶囊隐藏；宿主启动（2s 轮询发现）→ 重新出现。
- 无任何有效电量数据 → 胶囊整体隐藏（任务栏无残留）。

## 数据源（只读，21 键位全景）

根：HKCU\Software\RazerBatteryTray

- **每设备子键**：DeviceState\<sanitized>（最可信）> CustomDevices\<id>（CachedBattery，-1=未知）> HardwareCache\<id>、MchoseCache\Z75S\<HEX>
- **单键品牌缓存**（13 族，后写入胜）：Acer / HyperX / Logitech / Mchose(含 G75) / MchoseMouse / Nuphy / Rapoo / SteelSeries / Valkyrie(键鼠分列) / VGN / VGNKeyboard / Razer(HardwareCache 根) —— 其中 Logitech、MchoseMouse、VGN 需 IsValid=1
- **值名=设备ID**：RazerHeadsetCache = "电量|充电|Ticks"
- 跟随宿主节奏：HKCU\Software\FerrisPulse\RefreshInterval（DWORD 毫秒，30000/60000/300000，缺省 60000）
- **跳过**：AppleCache、UniversalGamingCache（死常量，零 I/O）、RateWriteCap、HiddenDevices、根值 PrimaryDevice/AutoSwitch 等非电量值

业务规则（有效性过滤、层级冲突消解、最低电量评选、红色阈值 30%、名字截断 24 字、隐藏规则、节奏计算）
全部在 src/core/CapsuleCore.cpp，有测试覆盖。

## 验收清单（Win32 层，人工）

- [ ] ATK A9 Air+ 在宿主面板显示 86% 时胶囊同显示 86%
- [ ] 电量 <30% 进度条变红（29 红 / 30 绿 / 31 绿，测试已覆盖边界）
- [ ] 胶囊左键 → 宿主窗口隐藏；再左键 → 广播唤回
- [ ] 右键五项菜单齐全，频率档带勾选并持久化（重启后保持）
- [ ] 关闭 FerrisPulse → 胶囊 2s 内消失；重启 → 回来
- [ ] 任务栏无胶囊自己的图标、窗口不抢焦点（NOACTIVATE）
- [ ] 改宿主刷新间隔 → 胶囊采样节奏跟随（跟随宿主档）
- [ ] 自启项写入/移除正确，注销重登后胶囊自启

## 已知偏差（实现期工程决定）

1. **玻璃机制**：spec 写的是 DWM system backdrop（毛玻璃）。该机制与 GDI 直绘的像素 alpha 语义不可靠（残留/隐形风险），
   改用**分层窗口 + GDI+ ARGB 半透明白色胶囊**（UpdateLayeredWindow）：视觉同为浅色半透明玻璃、圆角自绘、无绘制残留，
   且完全确定。任务栏尺度下与参考图一致。
2. **点击切换近似**：宿主 ToggleWindow（MainForm.cs:1487）隐藏分支带“保存窗口位置”逻辑；胶囊用 SW_HIDE 镜像
   “隐藏”语义，显示分支与宿主一致（广播 WM_SHOWME）。用户体验等价。
3. **RazerHeadset 显示名**：该族只存 deviceId 值名（无 DeviceName），胶囊直接显示 deviceId。
4. **单实例**：胶囊自身加 Local\DeviceTaskbarCapsule_SingleInstance 互斥（防重复双开），与宿主的
   Global\FerrisPulse_* 互不相干。

## 结构

    src/core/      CapsuleCore.h/.cpp   <- 唯一测试接缝（纯逻辑）
    src/adapters/  RegistryReader.*     <- 注册表只读遍历（21 键位）
                   Environment.*        <- 进程监控 / WM_SHOWME / Run 键 / config.json
    src/ui/        main.cpp             <- 分层窗口、GDI+ 渲染、锚定、菜单
    tests/         test_core.cpp        <- 接缝黑盒测试
