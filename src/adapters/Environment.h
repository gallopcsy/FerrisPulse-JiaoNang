// Environment.h — 进程监控 / 自启 / 配置 / 宿主窗口路由（Win32 适配层）
#pragma once
#include <string>
#include "core/CapsuleCore.h"

namespace capsule {
namespace adapters {

bool IsHostRunning();                 // FerrisPulse.exe 是否在运行
void ToggleHostWindow();              // 单击语义：镜像宿主托盘左键 ToggleWindow
void ShowHostWindow();                // 菜单「打开」= 仅广播 WM_SHOWME（只显示不切换）
bool IsAutostartOn();                 // 自启 Run 键状态
void SetAutostart(bool on);           // 写入/移除 HKCU Run 项（仅胶囊）
Config LoadConfig();                  // capsule.config.json
void SaveConfig(const Config& cfg);

} // namespace adapters
} // namespace capsule
