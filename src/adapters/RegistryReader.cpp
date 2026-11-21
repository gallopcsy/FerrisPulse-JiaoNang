// RegistryReader.cpp — HKCU\Software\RazerBatteryTray 只读遍历（绝不写入）
// 键族结构与取值语义依据：SPEC「Implementation Decisions · 数据源」
#include "RegistryReader.h"
#include "BluetoothBattery.h"
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

// 蓝牙设备：优先读 Windows SetupAPI 的 BLE 电量（与「设置→蓝牙」同源，实时准确）。
// 读不到（设备休眠/不在场/非 BLE）则保留注册表缓存值作为兜底。
// 识别依据：DeviceId 以 "BT_" 开头，且 HardwareFingerprint 是 12 位十六进制 MAC。
void preferBleBattery(HKEY sub, DeviceReading& r) {
    if (r.deviceKey.rfind("BT_", 0) != 0) return;   // 非蓝牙设备：保持注册表值
    std::wstring fp;
    if (!readSz(sub, L"HardwareFingerprint", fp) || fp.empty()) return;
    int pct = -1;
    if (!ReadBleBattery(toUtf8(fp), pct)) return;   // 读不到 → 兜底
    r.percent = pct;                                // 读到 → 覆盖注册表陈旧缓存
    // 视为当前时刻（epoch 秒），便于同一设备多来源时的冲突消解
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    r.timestamp = (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
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
        DeviceReading r = readDeviceValues(sub, key, tier, pctName, chgName, timeName, nameName, requireValid);
        preferBleBattery(sub, r);   // 蓝牙设备：BLE 实时值优先，注册表兜底
        out.push_back(r);
    });
}

// —— 键族描述表：三类结构 ①每设备子键族 ②单键品牌缓存 ——
// 字段序 {path, [tier|label], pct, chg, time, name, requireValid}
struct FamilySpec {
    const wchar_t* path; SourceTier tier;
    const wchar_t* pct, *chg, *time, *name; bool valid;
};
const FamilySpec kFamilies[] = {
    // 1) DeviceState<sanitized> — 最可信（写入时已做 0-100 校验）
    {L"DeviceState",       SourceTier::DeviceState, L"BatteryPercent", L"IsCharging", L"LastUpdated", L"DeviceName", false},
    // 2) CustomDevices<deviceId> — CachedBattery(-1=未知)、LastSeen
    {L"CustomDevices",     SourceTier::PerDevice,   L"CachedBattery",  nullptr,       L"LastSeen",    L"DeviceName", false},
    // 3) HardwareCache<deviceId>（雷蛇混合结构的每设备子键）
    {L"HardwareCache",     SourceTier::PerDevice,   L"BatteryPercent", nullptr,       L"LastUpdated", L"DeviceName", false},
    // 4) MchoseCache\Z75S\<HEX>（每端点子键）
    {L"MchoseCache\\Z75S", SourceTier::PerDevice,   L"BatteryPercent", L"IsCharging", L"LastUpdated", L"DeviceName", false},
};

struct SingleSpec {
    const wchar_t* path, *label, *pct, *chg, *time, *name; bool valid;
};
const SingleSpec kSingles[] = {
    {L"HardwareCache",      L"Razer",           L"BatteryPercent",   nullptr,       L"LastUpdated", L"DeviceName", false}, // 根值=旧版单设备遗留
    {L"AcerCache",          L"Acer",            L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    {L"HyperXCache",        L"HyperX",          L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    {L"LogitechCache",      L"Logitech",        L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", true }, // 需 IsValid=1
    {L"MchoseCache",        L"Mchose",          L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    {L"MchoseCache",        L"Mchose G75",      L"G75BatteryPercent",L"G75IsCharging", L"G75LastUpdated", L"DeviceName", false},
    {L"MchoseMouseCache",   L"Mchose Mouse",    L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", true }, // 需 IsValid=1
    {L"NuphyCache",         L"Nuphy",           L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    {L"RapooCache",         L"Rapoo",           L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    {L"SteelSeriesCache",   L"SteelSeries",     L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    {L"VgnCache",           L"VGN",             L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", true }, // 需 IsValid=1
    {L"VgnKeyboardCache",   L"VGN Keyboard",    L"BatteryPercent",   L"IsCharging", L"LastUpdated", L"DeviceName", false},
    // Valkyrie：键鼠分列（同键两套值）
    {L"ValkyrieCache",      L"Valkyrie Keyboard", L"KbBatteryPercent",   nullptr, L"LastUpdated", L"KbDeviceName",   false},
    {L"ValkyrieCache",      L"Valkyrie Mouse",    L"MouseBatteryPercent",nullptr, L"LastUpdated", L"MouseDeviceName", false},
};

} // namespace

Snapshot ReadRegistrySnapshot() {
    Snapshot snap;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RazerBatteryTray", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return snap;

    // —— 三类结构 ①每设备子键族 + ②单键品牌缓存：按表遍历（表定义见匿名命名空间）——
    for (const auto& f : kFamilies)
        readPerDeviceFamily(root, f.path, f.tier, f.pct, f.chg, f.time, f.name, f.valid, snap.readings);
    for (const auto& s : kSingles)
        readSingleKey(root, s.path, s.label, s.pct, s.chg, s.time, s.name, s.valid, snap.readings);

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
                std::wstring v;
                if (!readSz(k, name, v)) continue; // 仅 REG_SZ/REG_EXPAND_SZ
                DeviceReading r;
                int pct = 0; bool chg = false; long long ts = 0;
                if (ParseHeadsetValue(toUtf8(v), pct, chg, ts)) {
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
