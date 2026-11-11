// test_core.cpp — CapsuleCore 接缝黑盒测试（零依赖 assert）
// 用法：capsule-tests.exe，退出码=失败数
#include "core/CapsuleCore.h"
#include <cstdio>

using namespace capsule;

static int g_failed = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failed;                                                    \
        }                                                                  \
    } while (0)

static DeviceReading mk(const char* name, int pct, SourceTier tier, long long ts,
                        const char* key = "") {
    DeviceReading r;
    r.displayName = name;
    r.percent = pct;
    r.tier = tier;
    r.timestamp = ts;
    r.deviceKey = key;
    return r;
}

static void test_hidden_cases() {
    Snapshot s;
    s.readings.push_back(mk("ATK A9 Air+", 86, SourceTier::DeviceState, 100));
    s.hostPresent = false;
    ViewModel vm = Reduce(s, {});                    // 宿主不在
    CHECK(!vm.visible);

    s.hostPresent = true;
    s.readings.clear();                              // 宿主在但无数据
    vm = Reduce(s, {});
    CHECK(!vm.visible);

    s.readings.push_back(mk("Bad", -1, SourceTier::DeviceState, 100));  // 只有无效数据
    vm = Reduce(s, {});
    CHECK(!vm.visible);
}

static void test_basic_display_and_lowest() {
    Snapshot s; s.hostPresent = true;
    s.readings.push_back(mk("ATK A9 Air+", 86, SourceTier::DeviceState, 100));
    s.readings.push_back(mk("Headset X", 42, SourceTier::BrandCache, 50));
    ViewModel vm = Reduce(s, {});
    CHECK(vm.visible);
    CHECK(vm.displayName == "Headset X");   // 最低者上屏
    CHECK(vm.percent == 42);
    CHECK(!vm.lowBattery);
}

static void test_validity_filters() {
    Snapshot s; s.hostPresent = true;
    s.readings.push_back(mk("Unknown", -1, SourceTier::BrandCache, 10));
    s.readings.push_back(mk("Over", 101, SourceTier::BrandCache, 10));
    s.readings.push_back(mk("Zero", 0, SourceTier::BrandCache, 10));    // 0% 有效！
    ViewModel vm = Reduce(s, {});
    CHECK(vm.visible);
    CHECK(vm.displayName == "Zero");
    CHECK(vm.lowBattery);

    DeviceReading needsValid = mk("VgnMouse", 55, SourceTier::BrandCache, 10);
    needsValid.requiresValidFlag = true;
    needsValid.validFlag = false;
    s.readings.clear();
    s.readings.push_back(needsValid);
    s.readings.push_back(mk("Ok", 70, SourceTier::BrandCache, 10));
    vm = Reduce(s, {});
    CHECK(vm.displayName == "Ok");          // IsValid=0 被过滤
}

static void test_tier_conflict() {
    // 同一设备（同名）同时出现在 DeviceState 与品牌缓存 → 层级高者胜，即使时间戳更旧
    Snapshot s; s.hostPresent = true;
    s.readings.push_back(mk("ATK A9 Air+", 50, SourceTier::DeviceState, 100));
    s.readings.push_back(mk("ATK A9 Air+", 80, SourceTier::BrandCache, 9999));
    ViewModel vm = Reduce(s, {});
    CHECK(vm.percent == 50);

    // 同层级 → 时间戳新者胜
    s.readings.clear();
    s.readings.push_back(mk("ATK A9 Air+", 40, SourceTier::BrandCache, 100));
    s.readings.push_back(mk("ATK A9 Air+", 70, SourceTier::BrandCache, 200));
    vm = Reduce(s, {});
    CHECK(vm.percent == 70);

    // 分组身份大小写不敏感
    s.readings.clear();
    s.readings.push_back(mk("atk a9 air+", 50, SourceTier::DeviceState, 100));
    s.readings.push_back(mk("ATK A9 Air+", 80, SourceTier::BrandCache, 9999));
    vm = Reduce(s, {});
    CHECK(vm.percent == 50);
}

static void test_low_battery_boundary() {
    Snapshot s; s.hostPresent = true;
    s.readings.clear(); s.readings.push_back(mk("D", 29, SourceTier::DeviceState, 1));
    CHECK(Reduce(s, {}).lowBattery);
    s.readings.clear(); s.readings.push_back(mk("D", 30, SourceTier::DeviceState, 1));
    CHECK(!Reduce(s, {}).lowBattery);
    s.readings.clear(); s.readings.push_back(mk("D", 31, SourceTier::DeviceState, 1));
    CHECK(!Reduce(s, {}).lowBattery);
}

static void test_truncation() {
    CHECK(TruncateName("abc", 5) == "abc");                    // 不超长原样
    CHECK(TruncateName("abcdefgh", 5) == "abcde\xE2\x80\xA6");   // ASCII 截断+…
    // 中文按字符计数（不破坏 UTF-8）
    const std::string cn = "\xE5\x89\x8D\xE8\xA1\x8C\xE8\x80\x85\xE0\xB8\xB5"; // 前行者(3字)+0xE0..=5字符序列? 见下
    std::string long_cn = "";
    for (int i = 0; i < 10; ++i) long_cn += "\xE8\xAE\xBE\xE5\xA4\x87"; // 设备 x10
    std::string cut = TruncateName(long_cn, 4);
    CHECK(cut.size() == 4 * 3 + 3);                             // 4个汉字+省略号(3字节)
    CHECK(cut.find("\xE2\x80\xA6") == cut.size() - 3);         // 省略号在末尾
    (void)cn;
}

static void test_parse_iso8601() {
    // 2026-11-04T17:55:59.1636597+08:00 → UTC 09:55:59
    long long ts = ParseIso8601("2026-11-04T17:55:59.1636597+08:00");
    CHECK(ts == 1793786159LL);
    CHECK(ParseIso8601("garbage") == 0);
    CHECK(ParseIso8601("") == 0);
    CHECK(ParseIso8601("2026-11-04T17:55:59Z") >= 1793750400LL); // 无偏移也解析
}

static void test_parse_headset() {
    int pct = 0; bool chg = false; long long ts = 0;
    // ticks for 2026-11-04T09:55:59Z = (1793786159+62135596800)*1e7
    CHECK(ParseHeadsetValue("94|1|639293829590000000", pct, chg, ts));
    CHECK(pct == 94);
    CHECK(chg == true);
    CHECK(ts == 1793786159LL);
    CHECK(!ParseHeadsetValue("101|0|639293829590000000", pct, chg, ts)); // 越界
    CHECK(!ParseHeadsetValue("bad", pct, chg, ts));
    CHECK(!ParseHeadsetValue("50|0|0", pct, chg, ts)); // 0 ticks 非法
}

static void test_poll_interval() {
    Snapshot s; s.hostRefreshIntervalMs = 30000;
    Config c; c.freqMode = FreqMode::FollowHost;
    CHECK(PollIntervalMs(s, c) == 30000);
    s.hostRefreshIntervalMs = 0;                     // 未知 → 缺省 60s
    CHECK(PollIntervalMs(s, c) == 60000);
    s.hostRefreshIntervalMs = 12345;                 // 异常值 → 缺省
    CHECK(PollIntervalMs(s, c) == 60000);
    c.freqMode = FreqMode::Sec30;                    // 手动档无视宿主
    s.hostRefreshIntervalMs = 300000;
    CHECK(PollIntervalMs(s, c) == 30000);
    c.freqMode = FreqMode::Min5;
    CHECK(PollIntervalMs(s, c) == 300000);
}

static void test_charging_passthrough() {
    Snapshot s; s.hostPresent = true;
    DeviceReading r = mk("Mouse", 55, SourceTier::DeviceState, 1);
    r.charging = true;
    s.readings.push_back(r);
    CHECK(Reduce(s, {}).charging);
}

int main() {
    test_hidden_cases();
    test_basic_display_and_lowest();
    test_validity_filters();
    test_tier_conflict();
    test_low_battery_boundary();
    test_truncation();
    test_parse_iso8601();
    test_parse_headset();
    test_poll_interval();
    test_charging_passthrough();
    if (g_failed) { std::printf("%d FAILED\n", g_failed); return g_failed; }
    std::printf("ALL PASS\n");
    return 0;
}
