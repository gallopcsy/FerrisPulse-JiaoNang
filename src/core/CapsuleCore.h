// CapsuleCore.h — 设备任务栏胶囊 唯一测试接缝
// 纯逻辑：无 Win32、无 I/O。输入=注册表快照+宿主状态+配置，输出=视图模型。
#pragma once
#include <string>
#include <vector>

namespace capsule {

// 数据来源可信度（低者优先）：DeviceState > 每设备子键 > 单键品牌缓存
enum class SourceTier : int { DeviceState = 0, PerDevice = 1, BrandCache = 2 };

struct DeviceReading {
    std::string deviceKey;    // 键族内唯一标识（可为空）
    std::string displayName;  // UTF-8 设备名（可为空）
    int percent = -1;         // 原始电量；-1=未知
    bool requiresValidFlag = false; // 该键族要求 IsValid==1 才可信
    bool validFlag = true;    // IsValid 值
    bool charging = false;
    SourceTier tier = SourceTier::BrandCache;
    long long timestamp = 0;  // epoch 秒；0=未知（旧者优先级低）
};

struct Snapshot {
    std::vector<DeviceReading> readings;
    bool hostPresent = false;            // FerrisPulse.exe 是否在运行
    long long hostRefreshIntervalMs = 0; // HKCU\Software\FerrisPulse\RefreshInterval（0=未知）
};

// 读取频率档：跟随宿主 / 30秒 / 1分钟 / 5分钟（值即毫秒，便于直传）
enum class FreqMode : int { FollowHost = 0, Sec30 = 30000, Min1 = 60000, Min5 = 300000 };

struct Config {
    FreqMode freqMode = FreqMode::FollowHost;
};

struct ViewModel {
    bool visible = false;
    std::string displayName;  // UTF-8，已截断
    int percent = 0;
    bool charging = false;
    bool lowBattery = false;  // percent < 30
};

// 业务规则唯一入口：有效性过滤→同设备冲突消解→最低电量评选→隐藏规则
ViewModel Reduce(const Snapshot& snap, const Config& cfg);

// 本次采样到下次采样的间隔（毫秒）
long long PollIntervalMs(const Snapshot& snap, const Config& cfg);

// UTF-8 按“字符”截断（超长加省略号）；不破坏多字节序列
std::string TruncateName(const std::string& utf8, size_t maxChars);

// ISO-8601 "o" 往返格式（2026-11-04T17:55:59.1636597+08:00）→ epoch 秒；失败返回 0
long long ParseIso8601(const std::string& s);

// RazerHeadset 值 "电量|充电|Ticks"（DateTime.Now.Ticks, 100ns since year1）
// 成功且 0<=percent<=100 → true
bool ParseHeadsetValue(const std::string& value, int& percent, bool& charging, long long& timestamp);

constexpr int kLowBatteryThreshold = 30;  // 低于 30% 显示红色
constexpr size_t kMaxNameChars = 24;      // 设备名最大显示字符数

} // namespace capsule
