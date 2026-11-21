// BluetoothBattery.cpp — 蓝牙设备电量直读（只读 SetupAPI 驱动属性，零竞争）
#include "BluetoothBattery.h"
#include <windows.h>
#include <setupapi.h>
#include <string>
#include <vector>
#include <cstdio>

namespace capsule {
namespace adapters {

namespace {

// BLE 电量属性：{104ea319-6ee2-4701-bd47-8ddbf425bbe5} pid=2
const GUID kFmtBattery = {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}};
constexpr DWORD kPidBatteryLevel = 2;

DEVPROPKEY batteryKey() {
    DEVPROPKEY k{};
    k.fmtid = kFmtBattery;
    k.pid = kPidBatteryLevel;
    return k;
}

// 12 位十六进制 MAC 规范化（去分隔符、转大写）；非法返回空
std::string normalizeMac(const std::string& in) {
    std::string out;
    out.reserve(12);
    for (char c : in) {
        if (c == ':' || c == '-' || c == '_' || c == ' ') continue;
        if (c >= '0' && c <= '9') out += c;
        else if (c >= 'a' && c <= 'f') out += static_cast<char>(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'F') out += c;
        else return {};
    }
    return out.size() == 12 ? out : std::string{};
}

} // namespace

bool ReadBleBattery(const std::string& macHex, int& percent) {
    const std::string mac = normalizeMac(macHex);
    if (mac.empty()) return false;

    // 只需要扫 BLE 与经典蓝牙两个枚举器；设备不在场时直接失败（调用方回退注册表）
    static const wchar_t* kEnums[] = {L"BTHLE", L"BTHENUM"};
    const DEVPROPKEY key = batteryKey();

    for (const wchar_t* en : kEnums) {
        HDEVINFO info = SetupDiGetClassDevsW(nullptr, en, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (info == INVALID_HANDLE_VALUE) continue;

        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &data); ++i) {
            // 实例 ID 内含 MAC（BTHLE\DEV_FF0D0118DDFD\...）
            wchar_t inst[512]{};
            if (!SetupDiGetDeviceInstanceIdW(info, &data, inst, 512, nullptr)) continue;

            // 转成 ASCII 便于比对（实例 ID 只含 ASCII）
            char ascii[512]{};
            size_t n = 0;
            for (; inst[n] && n < 511; ++n) ascii[n] = static_cast<char>(inst[n]);
            ascii[n] = 0;

            // 大写化后就地查找 MAC 子串
            for (size_t j = 0; j < n; ++j)
                if (ascii[j] >= 'a' && ascii[j] <= 'f') ascii[j] = static_cast<char>(ascii[j] - 'a' + 'A');
            if (std::string(ascii).find(mac) == std::string::npos) continue;

            // 命中设备：读 BatteryLevel（BYTE）
            BYTE buf[16]{};
            DWORD type = 0, need = 0;
            if (!SetupDiGetDevicePropertyW(info, &data, &key, &type, buf, sizeof(buf), &need, 0)) continue;
            if (type != DEVPROP_TYPE_BYTE) continue;

            const int level = static_cast<int>(buf[0]);
            if (level < 0 || level > 100) continue;

            percent = level;
            SetupDiDestroyDeviceInfoList(info);
            return true;
        }
        SetupDiDestroyDeviceInfoList(info);
    }
    return false;
}

} // namespace adapters
} // namespace capsule
