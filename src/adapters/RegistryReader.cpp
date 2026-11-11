// RegistryReader.cpp — HKCU\Software\RazerBatteryTray 只读遍历（绝不写入）
// 键族结构与取值语义依据：SPEC「Implementation Decisions · 数据源」
#include "RegistryReader.h"
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace capsule {
namespace adapters {

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool readDword(HKEY k, const wchar_t* name, DWORD& out) {
    DWORD type = 0, size = sizeof(DWORD);
    return RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(&out), &size) == ERROR_SUCCESS
           && type == REG_DWORD;
}

bool readSz(HKEY k, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) return false;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS) return false;
    out = buf.data();
    return true;
}

long long readTimestamp(HKEY k, const wchar_t* name) {
    std::wstring w;
    if (!readSz(k, name, w)) return 0;
    return ParseIso8601(toUtf8(w));
}

// 枚举指定子键下的每级子键
void forEachSubkey(HKEY parent, const std::wstring& path,
                   const std::function<void(HKEY, const std::wstring&)>& fn) {
    HKEY sub = nullptr;
    if (RegOpenKeyExW(parent, path.c_str(), 0, KEY_READ, &sub) != ERROR_SUCCESS) return;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = 256;
        if (RegEnumKeyExW(sub, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY leaf = nullptr;
        if (RegOpenKeyExW(sub, name, 0, KEY_READ, &leaf) == ERROR_SUCCESS) {
            fn(leaf, std::wstring(name));
            RegCloseKey(leaf);
        }
    }
    RegCloseKey(sub);
}

// 读取单个设备子键的通用字段
DeviceReading readDeviceValues(HKEY sub, const std::wstring& keyName, SourceTier tier,
                               const wchar_t* pctName, const wchar_t* chgName,
                               const wchar_t* timeName, const wchar_t* nameName,
                               bool requireValid) {
    DeviceReading r;
    r.deviceKey = toUtf8(keyName);
    r.tier = tier;
    r.requiresValidFlag = requireValid;
    DWORD v = 0;
    if (readDword(sub, pctName, v)) r.percent = (int)v;
    if (chgName && readDword(sub, chgName, v)) r.charging = (v != 0);
    r.timestamp = readTimestamp(sub, timeName);
    std::wstring dn;
    if (nameName && readSz(sub, nameName, dn)) r.displayName = toUtf8(dn);
    // 通用名字回退链：CustomDevices 等键族无 DeviceName，真实名字在 CustomName/OriginalName
    if (r.displayName.empty() && readSz(sub, L"CustomName", dn) && !dn.empty()) {
        r.displayName = toUtf8(dn);
    } else if (r.displayName.empty() && readSz(sub, L"OriginalName", dn) && !dn.empty()) {
        r.displayName = toUtf8(dn);
    }
    if (requireValid) { r.validFlag = readDword(sub, L"IsValid", v) && v == 1; }
    return r;
}

// 单键品牌缓存：整键读成一条（后见设备语义，新旧由调用方/冲突消解处理）
void readSingleKey(HKEY root, const wchar_t* path, const wchar_t* label,
                   const wchar_t* pctName, const wchar_t* chgName,
                   const wchar_t* timeName, const wchar_t* nameName,
                   bool requireValid, std::vector<DeviceReading>& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DeviceReading r = readDeviceValues(k, label, SourceTier::BrandCache,
                                       pctName, chgName, timeName, nameName, requireValid);
    RegCloseKey(k);
    out.push_back(r);
}

void readPerDeviceFamily(HKEY root, const wchar_t* path, SourceTier tier,
                         const wchar_t* pctName, const wchar_t* chgName,
                         const wchar_t* timeName, const wchar_t* nameName,
                         bool requireValid, std::vector<DeviceReading>& out) {
    forEachSubkey(root, path, [&](HKEY sub, const std::wstring& key) {
        out.push_back(readDeviceValues(sub, key, tier, pctName, chgName, timeName, nameName, requireValid));
    });
}

} // namespace

Snapshot ReadRegistrySnapshot() {
    Snapshot snap;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RazerBatteryTray", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return snap;

    // —— 每设备子键（三类结构之 ①）——
    // 1) DeviceState<sanitized>  —— 最可信（写入时已做 0-100 校验）
    readPerDeviceFamily(root, L"DeviceState", SourceTier::DeviceState,
                        L"BatteryPercent", L"IsCharging", L"LastUpdated", L"DeviceName",
                        false, snap.readings);
    // 2) CustomDevices<deviceId>  —— CachedBattery(-1=未知)、LastSeen
    readPerDeviceFamily(root, L"CustomDevices", SourceTier::PerDevice,
                        L"CachedBattery", nullptr, L"LastSeen", L"DeviceName",
                        false, snap.readings);
    // 3) HardwareCache\<deviceId>（雷蛇混合结构的每设备子键）
    readPerDeviceFamily(root, L"HardwareCache", SourceTier::PerDevice,
                        L"BatteryPercent", nullptr, L"LastUpdated", L"DeviceName",
                        false, snap.readings);
    // 4) MchoseCache\Z75S\<HEX>（每端点子键）
    readPerDeviceFamily(root, L"MchoseCache\\Z75S", SourceTier::PerDevice,
                        L"BatteryPercent", L"IsCharging", L"LastUpdated", L"DeviceName",
                        false, snap.readings);

    // —— 单键品牌缓存（三类结构之 ②，13 族；Apple/UniversalGaming 为死常量，跳过）——
    readSingleKey(root, L"HardwareCache", L"Razer", L"BatteryPercent", nullptr, L"LastUpdated",
                  L"DeviceName", false, snap.readings); // 根值=旧版单设备遗留
    readSingleKey(root, L"AcerCache", L"Acer", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", false, snap.readings);
    readSingleKey(root, L"HyperXCache", L"HyperX", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", false, snap.readings);
    readSingleKey(root, L"LogitechCache", L"Logitech", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", true, snap.readings); // 需 IsValid=1
    readSingleKey(root, L"MchoseCache", L"Mchose", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", false, snap.readings);
    readSingleKey(root, L"MchoseCache", L"Mchose G75", L"G75BatteryPercent", L"G75IsCharging",
                  L"G75LastUpdated", L"DeviceName", false, snap.readings);
    readSingleKey(root, L"MchoseMouseCache", L"Mchose Mouse", L"BatteryPercent", L"IsCharging",
                  L"LastUpdated", L"DeviceName", true, snap.readings); // 需 IsValid=1
    readSingleKey(root, L"NuphyCache", L"Nuphy", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", false, snap.readings);
    readSingleKey(root, L"RapooCache", L"Rapoo", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", false, snap.readings);
    readSingleKey(root, L"SteelSeriesCache", L"SteelSeries", L"BatteryPercent", L"IsCharging",
                  L"LastUpdated", L"DeviceName", false, snap.readings);
    readSingleKey(root, L"VgnCache", L"VGN", L"BatteryPercent", L"IsCharging", L"LastUpdated",
                  L"DeviceName", true, snap.readings); // 需 IsValid=1
    readSingleKey(root, L"VgnKeyboardCache", L"VGN Keyboard", L"BatteryPercent", L"IsCharging",
                  L"LastUpdated", L"DeviceName", false, snap.readings);
    // Valkyrie：键鼠分列（同键两套值）
    readSingleKey(root, L"ValkyrieCache", L"Valkyrie Keyboard", L"KbBatteryPercent", nullptr,
                  L"LastUpdated", L"KbDeviceName", false, snap.readings);
    readSingleKey(root, L"ValkyrieCache", L"Valkyrie Mouse", L"MouseBatteryPercent", nullptr,
                  L"LastUpdated", L"MouseDeviceName", false, snap.readings);

    // —— 单键值名=设备ID（三类结构之 ③）——
    // RazerHeadsetCache：值="电量|充电|Ticks" 字符串
    {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, L"RazerHeadsetCache", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t name[256];
            DWORD idx = 0;
            for (;;) {
                DWORD n = 256;
                if ( RegEnumValueW(k, idx++, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                DWORD type = 0, size = 0;
                if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ) continue;
                std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
                if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS) continue;
                DeviceReading r;
                int pct = 0; bool chg = false; long long ts = 0;
                if (ParseHeadsetValue(toUtf8(buf.data()), pct, chg, ts)) {
                    r.displayName = toUtf8(name); // deviceId 作为身份与显示
                    r.deviceKey = r.displayName;
                    r.percent = pct;
                    r.charging = chg;
                    r.timestamp = ts;
                    r.tier = SourceTier::BrandCache;
                    snap.readings.push_back(r);
                }
            }
            RegCloseKey(k);
        }
    }

    RegCloseKey(root);

    // —— 读取节奏「跟随宿主」：HKCU\Software\FerrisPulse\RefreshInterval（DWORD 毫秒）——
    HKEY cfg = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\FerrisPulse", 0, KEY_READ, &cfg) == ERROR_SUCCESS) {
        DWORD ms = 0;
        if (readDword(cfg, L"RefreshInterval", ms)) snap.hostRefreshIntervalMs = ms;
        RegCloseKey(cfg);
    }
    return snap;
}

} // namespace adapters
} // namespace capsule
