// BluetoothBattery.h — 蓝牙设备电量直读（Windows SetupAPI 驱动属性缓存）
// 原理：Windows 自己维护 BLE Battery Service(0x180F) 的驱动属性
//       DEVPKEY_Device_BatteryLevel，与「设置 → 蓝牙」显示值同源。
// 只读：不连接设备、不开 GATT、不占用蓝牙通道，零竞争。
#pragma once
#include <string>

namespace capsule {
namespace adapters {

// 按 12 位十六进制 MAC（如 "FF0D0118DDFD"）读取 BLE 电量。
// 成功且 0<=percent<=100 返回 true；失败返回 false（调用方应回退到注册表缓存）。
bool ReadBleBattery(const std::string& macHex, int& percent);

} // namespace adapters
} // namespace capsule
