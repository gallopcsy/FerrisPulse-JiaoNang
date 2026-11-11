// CapsuleCore.cpp — 业务规则实现（测试接缝）
#include "CapsuleCore.h"
#include <algorithm>
#include <cctype>

namespace capsule {

namespace {

std::string lowerCopy(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// 分组身份：优先按（规范化）设备名，无名则按键
std::string groupIdentity(const DeviceReading& r) {
    if (!r.displayName.empty()) return lowerCopy(r.displayName);
    if (!r.deviceKey.empty()) return lowerCopy(r.deviceKey);
    return "<unknown>";
}

bool readingUsable(const DeviceReading& r) {
    if (r.percent < 0 || r.percent > 100) return false;
    if (r.requiresValidFlag && !r.validFlag) return false;
    return true;
}

// 同一设备多条读数：先比可信度层级，再比时间戳新旧
bool betterOf(const DeviceReading& a, const DeviceReading& b) {
    if (a.tier != b.tier) return static_cast<int>(a.tier) < static_cast<int>(b.tier);
    return a.timestamp > b.timestamp; // 未知(0)视为最旧
}

} // namespace

ViewModel Reduce(const Snapshot& snap, const Config& cfg) {
    (void)cfg;
    ViewModel vm;
    if (!snap.hostPresent) return vm; // 宿主不在 → 隐藏

    // 1) 分组消解冲突
    std::vector<DeviceReading> best;
    for (const auto& r : snap.readings) {
        if (!readingUsable(r)) continue;
        auto it = std::find_if(best.begin(), best.end(),
                               [&](const DeviceReading& b) { return groupIdentity(b) == groupIdentity(r); });
        if (it == best.end()) best.push_back(r);
        else if (betterOf(r, *it)) *it = r;
    }
    if (best.empty()) return vm; // 无活数据 → 隐藏

    // 2) 最低电量者上屏；同值比时间戳新者
    const DeviceReading* winner = &best[0];
    for (const auto& r : best) {
        if (r.percent < winner->percent) winner = &r;
        else if (r.percent == winner->percent && r.timestamp > winner->timestamp) winner = &r;
    }

    vm.displayName = TruncateName(winner->displayName.empty()
                                      ? (winner->deviceKey.empty() ? "\xE6\x9C\xAA\xE7\x9F\xA5\xE8\xAE\xBE\xE5\xA4\x87" : winner->deviceKey)
                                      : winner->displayName,
                                  kMaxNameChars);
    vm.percent = winner->percent;
    vm.charging = winner->charging;
    vm.lowBattery = winner->percent < kLowBatteryThreshold;
    vm.visible = true;
    return vm;
}

long long PollIntervalMs(const Snapshot& snap, const Config& cfg) {
    if (cfg.freqMode != FreqMode::FollowHost) return static_cast<long long>(cfg.freqMode);
    switch (snap.hostRefreshIntervalMs) {
        case 30000: case 60000: case 300000:
            return snap.hostRefreshIntervalMs;
        default:
            return 60000; // 宿主缺省也是 60000
    }
}

std::string TruncateName(const std::string& utf8, size_t maxChars) {
    size_t chars = 0, i = 0, cutAt = std::string::npos;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > utf8.size()) len = 1; // 截断的非法序列按 1 字节
        if (chars == maxChars) { cutAt = i; break; }
        i += len;
        ++chars;
    }
    if (cutAt == std::string::npos) return utf8; // 未超长
    return utf8.substr(0, cutAt) + "\xE2\x80\xA6"; // …
}

long long ParseIso8601(const std::string& s) {
    // yyyy-mm-ddThh:mm:ss[.fffffff][+|-hh:mm]
    if (s.size() < 19) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    // days_from_civil（Howard Hinnant 算法）
    y -= mo <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
    long long epoch = days * 86400LL + h * 3600LL + mi * 60LL + sec;
    // 时区偏移
    if (s.size() >= 25) {
        char sign = 0; int offH = 0, offM = 0;
        const size_t p = s.size() - 6;
        if ((s[p] == '+' || s[p] == '-') &&
            std::sscanf(s.c_str() + p, "%c%2d:%d", &sign, &offH, &offM) == 3) {
            const long long off = offH * 3600LL + offM * 60LL;
            epoch -= (sign == '+' ? 1 : -1) * off;
        }
    }
    return epoch;
}

bool ParseHeadsetValue(const std::string& value, int& percent, bool& charging, long long& timestamp) {
    // "电量|充电|Ticks"
    const size_t p1 = value.find('|');
    if (p1 == std::string::npos) return false;
    const size_t p2 = value.find('|', p1 + 1);
    if (p2 == std::string::npos) return false;
    try {
        percent = std::stoi(value.substr(0, p1));
        const int c = std::stoi(value.substr(p1 + 1, p2 - p1 - 1));
        const long long ticks = std::stoll(value.substr(p2 + 1));
        if (percent < 0 || percent > 100) return false;
        if (ticks <= 0) return false;
        charging = (c != 0);
        // .NET ticks: 100ns since 0001-01-01；epoch(1970) = 62135596800s
        timestamp = ticks / 10000000LL - 62135596800LL;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace capsule
