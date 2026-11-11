// Environment.cpp
#include "Environment.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>

namespace capsule {
namespace adapters {

namespace {

std::wstring exePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring exeDir() {
    std::wstring p = exePath();
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

DWORD hostPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"FerrisPulse.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

struct EnumCtx { DWORD pid; HWND found; };

BOOL CALLBACK findMainWindow(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    // 跳过工具窗/消息窗，取主窗（WinForms 主窗非 WS_EX_TOOLWINDOW）
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE; // 找不到可见窗时后续靠广播
    ctx->found = hwnd;
    return FALSE;
}

} // namespace

bool IsHostRunning() { return hostPid() != 0; }

void ShowHostWindow() {
    UINT msg = RegisterWindowMessageW(L"FerrisPulse_ActivateExistingInstance");
    if (msg) SendNotifyMessageW(HWND_BROADCAST, msg, 0, 0);
}

void ToggleHostWindow() {
    UINT msg = RegisterWindowMessageW(L"FerrisPulse_ActivateExistingInstance");
    EnumCtx ctx{hostPid(), nullptr};
    if (ctx.pid) EnumWindows(findMainWindow, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found && IsWindowVisible(ctx.found) && !IsIconic(ctx.found)) {
        // 宿主 ToggleWindow 可见未最小化 → 隐藏（镜像 MainForm.cs ToggleWindow 分支）
        ShowWindow(ctx.found, SW_HIDE);
        return;
    }
    // 否则 → 激醒/显示（宿主 Program.WM_SHOWME → ShowWindow()）
    if (msg) SendNotifyMessageW(HWND_BROADCAST, msg, 0, 0);
}

bool IsAutostartOn() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LONG st = RegQueryValueExW(k, L"设备任务栏胶囊", nullptr, &type, nullptr, &size);
    RegCloseKey(k);
    return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

void SetAutostart(bool on) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    const wchar_t* valName = L"设备任务栏胶囊";
    if (on) {
        std::wstring data = L"\"" + exePath() + L"\"";
        RegSetValueExW(k, valName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(data.c_str()),
                       (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, valName);
    }
    RegCloseKey(k);
}

Config LoadConfig() {
    Config cfg;
    std::wstring path = exeDir() + L"\\capsule.config.json";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return cfg;
    char buf[512] = {};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    // 极简提取："freqMode": <num>
    const char* p = strstr(buf, "\"freqMode\"");
    if (p) {
        p = strchr(p + 9, ':');
        if (p) {
            long v = strtol(p + 1, nullptr, 10);
            if (v == 0) cfg.freqMode = FreqMode::FollowHost;
            else if (v == 30000) cfg.freqMode = FreqMode::Sec30;
            else if (v == 60000) cfg.freqMode = FreqMode::Min1;
            else if (v == 300000) cfg.freqMode = FreqMode::Min5;
        }
    }
    return cfg;
}

void SaveConfig(const Config& cfg) {
    std::wstring path = exeDir() + L"\\capsule.config.json";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\n  \"freqMode\": %d\n}\n", (int)cfg.freqMode);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, buf, (DWORD)strlen(buf), &written, nullptr);
    CloseHandle(h);
}

} // namespace adapters
} // namespace capsule
