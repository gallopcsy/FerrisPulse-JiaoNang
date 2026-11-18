// main.cpp — 设备任务栏胶囊 UI 层
// WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE 分层窗口；GDI+ ARGB 玻璃胶囊；UpdateLayeredWindow 呈现。
// （呈现机制说明见 README「已知偏差」：分层半透明玻璃替代 DWM system backdrop，视觉一致、无绘制残留。）
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <cstdio>
#include <cmath>
#include "core/CapsuleCore.h"
#include "adapters/RegistryReader.h"
#include "adapters/Environment.h"

using namespace capsule;

static HWND g_hwnd = nullptr;
static Config g_cfg;
static Snapshot g_snap;
static ViewModel g_vm;
static int g_capsuleW = 240, g_capsuleH = 32;
static int g_x = -10000, g_y = -10000;
// 内容命中盒（屏幕坐标窗口内相对值）：盒外 WM_NCHITTEST 返回 HTTRANSPARENT，点击落到下层任务栏图标
static int g_hitL = 0, g_hitR = 0, g_hitT = 0, g_hitB = 0;
// WinEvent 钩子（0=前台切换 1=窗口重排 → 重申置顶；2=任务栏显隐/移动 → 跟随）
static HWINEVENTHOOK g_hooks[3] = {nullptr, nullptr, nullptr};
// 任务栏窗口缓存 + 朝向（垂直任务栏放不下胶囊，直接隐藏而不是错位）
static HWND g_tray = nullptr;
static bool g_trayVertical = false;

// explorer 重启会重建任务栏窗口：句柄失效（或被新句柄替换）时必须重新解析
static void refreshTray() {
    if (!g_tray || !IsWindow(g_tray)) g_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
}

static bool isTrayClass(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[32]{};
    GetClassNameW(hwnd, cls, 32);
    return wcscmp(cls, L"Shell_TrayWnd") == 0;
}
// 充电呼吸动画开关（仅在「充电 + 可见」时挂 40ms 定时器，其余时刻零开销）
static bool g_animOn = false;
// 覆盖自检：Win11「快速设置 / 通知中心」由 DWM 合成层直接叠加，任何 TOPMOST 窗口都压不过它
// （实测：强制 SetWindowPos(HWND_TOPMOST) 后像素仍被面板覆盖）。唯一可行策略是让位：
// 用像素回采发现胶囊被盖住 → 收起；面板关闭（像素恢复）后再出现。
static int g_probeX = 0, g_probeY = 0;
static bool g_probeValid = false;
static unsigned g_probeRgb = 0;      // 满亮度时的期望色
static bool g_probeAnimated = false; // 呼吸动画中：亮度在 0.60–1.00 之间浮动
static bool g_yielded = false;      // 已因被覆盖而让位
static ULONGLONG g_yieldAt = 0;
static ULONGLONG g_shownAt = 0;     // 刚显示的宽限期（DWM 合成一帧需要时间）
static ULONG_PTR g_gdiplusToken = 0;

enum { IDT_POLL = 1, IDT_LIFE = 2, IDT_ANIM = 3 };
enum {
    ID_OPEN = 1001,
    ID_REFRESH = 1002,
    ID_FREQ_FOLLOW = 1010,
    ID_FREQ_30S = 1011,
    ID_FREQ_1M = 1012,
    ID_FREQ_5M = 1013,
    ID_AUTOSTART = 1020,
    ID_EXIT = 1021
};

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void roundRectPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    p.Reset();
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}

// 亮度缩放（充电呼吸用）：保持 alpha，仅按比例压 RGB
static Gdiplus::Color dim(Gdiplus::Color c, float k) {
    return Gdiplus::Color(c.GetA(),
                          (BYTE)(c.GetR() * k), (BYTE)(c.GetG() * k), (BYTE)(c.GetB() * k));
}

// 测量：布局 = pad + 名字 + 10 + 进度条76 + 6 + 百分比 + pad
// 注意：Graphics 必须先于 DeleteDC 析构（内层作用域），否则析构触碰已释放 DC（UB）。
static int measureWidth() {
    std::wstring name = utf8ToWide(g_vm.displayName);
    if (name.empty()) name = L"--";
    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", g_vm.percent);

    int w = 160;
    {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        Gdiplus::RectF rn, rp;
        {
            Gdiplus::Graphics gr(mem);
            gr.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            Gdiplus::Font fName(L"Segoe UI", 9.5f);
            Gdiplus::Font fPct(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold);
            gr.MeasureString(name.c_str(), -1, &fName, Gdiplus::PointF(0, 0), &rn);
            gr.MeasureString(pct, -1, &fPct, Gdiplus::PointF(0, 0), &rp);
        } // gr 先析构
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        w = 12 + (int)rn.Width + 10 + 76 + 6 + (int)rp.Width + 12;
    }
    if (w < 160) w = 160;
    if (w > 420) w = 420; // 极端长名防溢出（名字另有 24 字符截断）
    return w;
}

// 锚定：Shell_TrayWnd → TrayNotifyWnd 左缘 -8 右对齐；垂直居中
// 只支持水平任务栏（上/下）：垂直任务栏宽度不足以放下胶囊，由 applyVisibility 隐藏。
static void anchor() {
    refreshTray();
    if (!g_tray) return;
    RECT tr{};
    if (!GetWindowRect(g_tray, &tr)) return;
    g_trayVertical = (tr.bottom - tr.top) > (tr.right - tr.left);
    if (g_trayVertical) return;
    LONG right = tr.right - 340; // 回退：任务栏右缘内收
    HWND tn = FindWindowExW(g_tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (tn) {
        RECT r{};
        if (GetWindowRect(tn, &r)) right = r.left;
    }
    int taskH = tr.bottom - tr.top;
    g_capsuleH = taskH - 16;
    if (g_capsuleH < 24) g_capsuleH = 24;
    if (g_capsuleH > 40) g_capsuleH = 40;
    g_y = tr.top + (taskH - g_capsuleH) / 2;
    g_x = right - 8 - g_capsuleW;
}

// GDI+ 渲染 → 分层窗口呈现
static void render() {
    const int W = g_capsuleW, H = g_capsuleH;
    if (W <= 0 || H <= 0) return;

    // 关键：GDI+ 必须画在 Bitmap（PARGB）上 —— Graphics(HDC) 不写 alpha，
    // 否则 UpdateLayeredWindow 按全透明合成（内容不可见）。
    Gdiplus::Bitmap surface(W, H, PixelFormat32bppPARGB);
    if (surface.GetLastStatus() != Gdiplus::Ok) return;
    Gdiplus::Graphics gr(&surface);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // 背景完全透明（无底板/描边）——仅绘制文字与进度条，
    // 让胶囊下方任务栏图标透出；文字加白色描边保证图标底上可读。
    Gdiplus::Font fName(L"Segoe UI", 9.5f);
    Gdiplus::Font fPct(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold);
    Gdiplus::SolidBrush textBr(Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));
    Gdiplus::SolidBrush outlineBr(Gdiplus::Color(0xD0, 0xFF, 0xFF, 0xFF));
    Gdiplus::SolidBrush pctBr(g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xD7, 0x00, 0x15)
                                              : Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));

    float y = (H - 16.0f) / 2.0f;
    float x = 12.0f;

    // 设备名（白色描边 + 正文）
    std::wstring name = utf8ToWide(g_vm.displayName);
    if (name.empty()) name = L"--";
    Gdiplus::RectF rn;
    gr.MeasureString(name.c_str(), -1, &fName, Gdiplus::PointF(0, 0), &rn);
    gr.DrawString(name.c_str(), -1, &fName, Gdiplus::PointF(x + 1.0f, y + 1.0f), &outlineBr);
    gr.DrawString(name.c_str(), -1, &fName, Gdiplus::PointF(x, y), &textBr);
    x += rn.Width + 10.0f;

    // 进度条：轨道 + 渐变填充（低电量红色）
    const float barW = 76.0f, barH = 8.0f;
    float barY = (H - barH) / 2.0f;
    Gdiplus::GraphicsPath track;
    roundRectPath(track, x, barY, barW, barH, barH / 2.0f);
    Gdiplus::SolidBrush trackBr(Gdiplus::Color(0x33, 0x00, 0x00, 0x00));
    gr.FillPath(&trackBr, &track);
    float fillW = barW * g_vm.percent / 100.0f;
    if (g_vm.percent > 0 && fillW < 4.0f) fillW = 4.0f;
    if (fillW > 1.0f) {
        // 充电呼吸：2s 一个周期，亮度 0.60 → 1.00（非充电恒为 1，视觉与旧版一致）
        float breath = 1.0f;
        if (g_vm.charging) {
            const double t = (double)(GetTickCount64() % 2000ull) / 2000.0;
            breath = 0.60f + 0.40f * (float)(0.5 * (1.0 - std::cos(t * 2.0 * 3.14159265358979323846)));
        }
        Gdiplus::Color c1 = g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xFF, 0x45, 0x3A)
                                            : Gdiplus::Color(0xFF, 0x34, 0xC7, 0x59);
        Gdiplus::Color c2 = g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xFF, 0x69, 0x61)
                                            : Gdiplus::Color(0xFF, 0x2F, 0xB4, 0x5A);
        if (breath < 0.999f) { c1 = dim(c1, breath); c2 = dim(c2, breath); }
        // 探针：进度条左端内侧一点（避开圆角/抗锯齿），用渐变起色作期望值
        g_probeX = g_x + (int)(x + 3.0f);
        g_probeY = g_y + (int)(barY + barH / 2.0f);
        g_probeRgb = ((unsigned)c1.GetR() << 16) | ((unsigned)c1.GetG() << 8) | (unsigned)c1.GetB();
        g_probeAnimated = g_vm.charging;
        g_probeValid = true;
        Gdiplus::LinearGradientBrush bar(Gdiplus::RectF(x, barY, fillW, barH), c1, c2, 0.0f);
        Gdiplus::GraphicsPath fill;
        roundRectPath(fill, x, barY, fillW, barH, barH / 2.0f);
        gr.SetClip(&fill);
        gr.FillRectangle(&bar, x, barY, fillW, barH);
        gr.ResetClip();
    }
    x += barW + 6.0f;

    // 百分比
    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", g_vm.percent);
    Gdiplus::RectF rp;
    gr.MeasureString(pct, -1, &fPct, Gdiplus::PointF(0, 0), &rp);
    gr.DrawString(pct, -1, &fPct, Gdiplus::PointF(x + 1.0f, y + 1.0f), &outlineBr);
    gr.DrawString(pct, -1, &fPct, Gdiplus::PointF(x, y), &pctBr);

    // 内容命中盒：盒外点击穿透到任务栏（透明像素不吞点击）
    g_hitL = 12;
    g_hitR = (int)(x + rp.Width);
    g_hitT = (int)y;
    g_hitB = (int)y + 16;

    gr.Flush();

    HBITMAP hBmp = nullptr;
    if (surface.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBmp) != Gdiplus::Ok || !hBmp) return;
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, hBmp);

    POINT ptSrc{0, 0};
    POINT ptDst{g_x, g_y};
    SIZE sz{W, H};
    // 字段序：{BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat}
    // AlphaFormat 必须 = AC_SRC_ALPHA 才启用逐像素混合（否则整窗按不透明拷贝，透明区变纯黑）
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hwnd, nullptr, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(hBmp);
}

static void applyPollTimer() {
    static long long lastInterval = -1;
    long long iv = PollIntervalMs(g_snap, g_cfg);
    if (iv != lastInterval) {
        SetTimer(g_hwnd, IDT_POLL, (UINT)iv, nullptr);
        lastInterval = iv;
    }
}

// 重申置顶：不移动、不激活、不参与 z 序链重排（显示/兜底/事件守护三处复用）
static void raiseTopmost() {
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// 任务栏「实际露在屏幕上」判定：自动隐藏滑出屏幕后只剩 1–2px，视为隐藏。
// 胶囊是独立窗口，任务栏滑走时它不会跟着走，必须靠这里同步隐藏/跟随。
static bool trayOnScreen() {
    refreshTray();
    if (!g_tray || !IsWindowVisible(g_tray)) return false;
    RECT t{};
    if (!GetWindowRect(g_tray, &t)) return false;
    const LONG tw = t.right - t.left, th = t.bottom - t.top;
    if (tw <= 0 || th <= 0) return false;
    HMONITOR mon = MonitorFromWindow(g_tray, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT vis{};
    if (!IntersectRect(&vis, &t, &mi.rcMonitor)) return false;
    const LONG vw = vis.right - vis.left, vh = vis.bottom - vis.top;
    return (tw >= th) ? (vh >= 8 && vh * 3 >= th)   // 水平任务栏：露出 >=8px 且 >=1/3
                      : (vw >= 8 && vw * 3 >= tw);  // 垂直任务栏
}

// 像素回采：内部画的期望色与屏幕实际像素差太多 ⇒ 被合成层挡住
static bool coveredDown() {
    if (!g_probeValid || !IsWindowVisible(g_hwnd)) return false;
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const COLORREF px = GetPixel(dc, g_probeX, g_probeY);
    ReleaseDC(nullptr, dc);
    if (px == CLR_INVALID) return false;
    const int pr = (int)GetRValue(px), pg = (int)GetGValue(px), pb = (int)GetBValue(px);
    if (g_probeAnimated) {
        // 呼吸相位未知：只要求落在「同一色相 × 0.55–1.05 亮度」区间内
        auto inRange = [](int v, int base) {
            return v >= (int)(base * 0.55f) - 6 && v <= (int)(base * 1.05f) + 6;
        };
        return !(inRange(pr, (int)((g_probeRgb >> 16) & 0xFF)) &&
                 inRange(pg, (int)((g_probeRgb >> 8) & 0xFF)) &&
                 inRange(pb, (int)(g_probeRgb & 0xFF)));
    }
    const int dr = pr - (int)((g_probeRgb >> 16) & 0xFF);
    const int dg = pg - (int)((g_probeRgb >> 8) & 0xFF);
    const int db = pb - (int)(g_probeRgb & 0xFF);
    return dr * dr + dg * dg + db * db > 60 * 60;
}

static void applyVisibility() {
    const bool want = g_vm.visible && !g_trayVertical && trayOnScreen();
    const bool is = IsWindowVisible(g_hwnd) != 0;
    if (!want) {
        if (is) ShowWindow(g_hwnd, SW_HIDE);
        g_yielded = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!is) {
        // 被合成层盖住时已让位：低频重试（此时胶囊区域正被面板覆盖，用户看不到任何闪动）
        if (g_yielded && now - g_yieldAt < 900) return;
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        raiseTopmost();
        render(); // 先画，再回采
        g_shownAt = now;
        if (coveredDown()) {
            ShowWindow(g_hwnd, SW_HIDE);
            g_yielded = true;
            g_yieldAt = now;
        } else {
            g_yielded = false;
        }
        return;
    }
    if (now - g_shownAt < 400) return; // 刚显示：等 DWM 合成完成再判定，避免自激抖动
    if (coveredDown()) { // 显示中却被盖住 → 让位
        ShowWindow(g_hwnd, SW_HIDE);
        g_yielded = true;
        g_yieldAt = now;
    }
}

// 充电呼吸动画：40ms/帧（25fps），仅「充电 + 可见」时挂表；隐藏/离电立即摘表
static void applyAnimTimer() {
    bool want = g_vm.charging && g_vm.visible;
    if (want && !g_animOn) {
        g_animOn = SetTimer(g_hwnd, IDT_ANIM, 40, nullptr) != 0;
    } else if (!want && g_animOn) {
        KillTimer(g_hwnd, IDT_ANIM);
        g_animOn = false;
    }
}

// 布局一次到位：测宽 → 锚定 → （几何变了或 force 时）重绘 → 显隐 → 呼吸表。
// force=true 用于数据变化（内容必须重画）；false 用于任务栏跟随（几何没变就不重画）。
static void layout(bool force) {
    const int ox = g_x, oy = g_y, ow = g_capsuleW, oh = g_capsuleH;
    g_capsuleW = measureWidth();
    anchor();
    if (force || g_x != ox || g_y != oy || g_capsuleW != ow || g_capsuleH != oh) render();
    applyVisibility();
    applyAnimTimer();
}

static void followTray() { layout(false); }

static void sampleNow() {
    Snapshot s = adapters::ReadRegistrySnapshot();
    s.hostPresent = adapters::IsHostRunning();
    g_snap = std::move(s);
    g_vm = Reduce(g_snap, g_cfg);
    applyPollTimer();
    layout(true);
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"打开FerrisPulse");
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"立即刷新");
    HMENU sub = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"自动同步频率");
    // 频率档位表：{命令 ID, 档位, 文案}
    static const struct { UINT id; FreqMode mode; const wchar_t* text; } kFreqs[] = {
        {ID_FREQ_FOLLOW, FreqMode::FollowHost, L"跟随宿主（默认）"},
        {ID_FREQ_30S,    FreqMode::Sec30,     L"30秒"},
        {ID_FREQ_1M,     FreqMode::Min1,      L"1分钟"},
        {ID_FREQ_5M,     FreqMode::Min5,      L"5分钟"},
    };
    for (const auto& fr : kFreqs)
        AppendMenuW(sub, MF_STRING | (g_cfg.freqMode == fr.mode ? MF_CHECKED : 0), fr.id, fr.text);
    AppendMenuW(m, MF_STRING | (adapters::IsAutostartOn() ? MF_CHECKED : 0),
                ID_AUTOSTART, L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"退出本程序");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd); // 让菜单在别处点击时能正常关闭
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    bool freqChanged = false;
    for (const auto& fr : kFreqs)
        if (cmd == fr.id) { g_cfg.freqMode = fr.mode; freqChanged = true; }
    if (freqChanged) {
        adapters::SaveConfig(g_cfg);
        sampleNow(); // applyPollTimer 生效新档位
    } else if (cmd == ID_OPEN) {
        adapters::ShowHostWindow();
    } else if (cmd == ID_REFRESH) {
        sampleNow(); // 不打乱采样相位（仅即时重读）
    } else if (cmd == ID_AUTOSTART) {
        adapters::SetAutostart(!adapters::IsAutostartOn());
    } else if (cmd == ID_EXIT) {
        DestroyWindow(g_hwnd);
    }
}

// ── z序守护 ──────────────────────────────────────────────────────
// explorer 在任务栏交互（点图标、开快速设置、托盘气泡、开始菜单）时会重排 topmost 层，
// 把胶囊压到下面 → 表现为「胶囊被盖住 / 突然消失」。这里沿 z 序向上找第一个压住
// 胶囊且与之相交的可见窗口，用几何相交判定而不只认任务栏句柄。

// 是否有【任何可见窗口】压在自己上面并与胶囊矩形相交。
// Win11 点击 WLAN/声音/电池 弹出的「快速设置」是 explorer 临时顶起来的顶层窗口，
// 出现瞬间会排到胶囊之上；旧逻辑只比对 Shell_TrayWnd，对这种"别的窗口盖过来"完全无感，
// 表现为胶囊被面板盖住 → 这里按几何相交判定，不区分窗口来源。
static bool coveredFromAbove() {
    RECT me{};
    if (!GetWindowRect(g_hwnd, &me)) return false;
    HWND h = g_hwnd;
    for (int i = 0; i < 96; ++i) {
        h = GetWindow(h, GW_HWNDPREV); // 沿 z 序逐级向上
        if (!h) return false;
        if (h == g_tray) return true;                 // 任务栏压上来（含托盘空白区）
        if (!IsWindowVisible(h)) continue;
        RECT r{};
        if (!GetWindowRect(h, &r)) continue;
        if (r.right - r.left <= 0 || r.bottom - r.top <= 0) continue;
        if (r.right <= me.left || r.left >= me.right || r.bottom <= me.top || r.top >= me.bottom) continue;
        // 排除自己进程内的窗口（如 GDI+ 钩子窗），其余一律视为遮挡
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) continue;
        return true;                                   // 相交的可见窗口盖在上面
    }
    return false;
}

// 被压住 / 被降级出 topmost / 被别的 topmost 窗口盖住 —— 三种都要救回来；重入保护防递归
static void ensureTopmost() {
    static bool inCall = false;
    if (inCall || !g_hwnd || !IsWindowVisible(g_hwnd)) return;
    const bool lostTopmost = (GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
    if (!lostTopmost && !coveredFromAbove()) return;
    // 限流：explorer 弹面板时会连发重排事件，最多 120ms 抢一次，避免与宿主反复争抢
    static ULONGLONG lastRaise = 0;
    const ULONGLONG now = GetTickCount64();
    if (!lostTopmost && now - lastRaise < 120) return;
    lastRaise = now;
    inCall = true;
    raiseTopmost();
    inCall = false;
}

static void CALLBACK onZEvent(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD) {
    // 实测 explorer 重排事件 idObject 可能是 OBJID_CLIENT(-4) 等，不按对象过滤；
    // ensureTopmost 内部自带“真的被压住才动手”的判定，回调本身零开销。
    (void)idObject;
    ensureTopmost();
    // 面板/合成层出现或消失时也立刻复核一次可见性（不必等 2s LIFE 兜底）
    applyVisibility();
}

// 任务栏显隐/滑入滑出（自动隐藏）→ 立即跟随，不等 2s 轮询
static void CALLBACK onTrayEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    // 任务栏（含自动隐藏滑入滑出、explorer 重启换句柄）→ 立即跟随
    if (isTrayClass(hwnd)) {
        g_tray = hwnd;
        followTray();
        if (IsWindowVisible(g_hwnd)) raiseTopmost();
        return;
    }
    // 其它顶层窗口出现/移动（快速设置、通知中心、托盘气泡……）→ 被盖住就立刻拉回
    ensureTopmost();
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT rc;
            GetClientRect(hwnd, &rc);
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
            if (PtInRect(&rc, pt) &&
                pt.x - rc.left >= g_hitL && pt.x - rc.left < g_hitR &&
                pt.y - rc.top >= g_hitT && pt.y - rc.top < g_hitB)
                return HTCLIENT;        // 文字/进度条区域：胶囊交互（左键切换、右键菜单）
            return HTTRANSPARENT;       // 其余透明像素：点击落到下层任务栏图标
        }
        case WM_LBUTTONDOWN:
            adapters::ToggleHostWindow(); // 单击 ≡ 宿主托盘左键
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showMenu();
            return 0;
        case WM_TIMER:
            if (wp == IDT_POLL) {
                sampleNow();
            } else if (wp == IDT_ANIM) {
                if (!g_vm.charging || !g_vm.visible) {
                    KillTimer(hwnd, IDT_ANIM);
                    g_animOn = false;
                } else {
                    render(); // 仅重绘呼吸相位，不重锚定/不重读注册表
                }
            } else if (wp == IDT_LIFE) {
                bool host = adapters::IsHostRunning();
                if (host != g_snap.hostPresent) {
                    sampleNow(); // 宿主出现/退出 → 立即重采
                } else {
                    followTray();
                    // 任务栏/托盘气泡等 topmost 窗口重排会把胶囊压回任务栏下方 → 每 2s 重申置顶
                    if (IsWindowVisible(g_hwnd)) raiseTopmost();
                }
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0; // 分层窗口由 UpdateLayeredWindow 呈现，无传统绘制
        }
        case WM_DESTROY:
            KillTimer(hwnd, IDT_POLL);
            KillTimer(hwnd, IDT_LIFE);
            KillTimer(hwnd, IDT_ANIM);
            g_animOn = false;
            for (HWINEVENTHOOK& h : g_hooks) if (h) { UnhookWinEvent(h); h = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // DPI 感知（多显示器不同缩放时按物理像素锚定）
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 单实例（胶囊自身的本地互斥；与宿主的全局互斥无关）
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\DeviceTaskbarCapsule_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // DPI 缩放下的任务栏高度修正：读取 TaskbarSizeHint?（Win11 固定48物理px，anchor 从 TrayRect 实测）

    g_cfg = adapters::LoadConfig();

    Gdiplus::GdiplusStartupInput gpsi;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gpsi, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DeviceTaskbarCapsuleWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST,
                             wc.lpszClassName, L"",
                             WS_POPUP,
                             0, 0, 10, 10,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    sampleNow(); // 首采：决定是否显示
    SetTimer(g_hwnd, IDT_LIFE, 2000, nullptr);

    // 事件驱动：z 序被压 → 毫秒级拉回；任务栏滑入滑出/重建 → 立即跟随（2s LIFE 仅兜底）
    struct { UINT lo, hi; WINEVENTPROC proc; } const watchers[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, onZEvent},
        {EVENT_OBJECT_REORDER,    EVENT_OBJECT_REORDER,    onZEvent},
        {EVENT_OBJECT_SHOW,       EVENT_OBJECT_LOCATIONCHANGE, onTrayEvent},
    };
    refreshTray();
    for (int i = 0; i < 3; ++i)
        g_hooks[i] = SetWinEventHook(watchers[i].lo, watchers[i].hi, nullptr,
                                     watchers[i].proc, 0, 0, WINEVENT_OUTOFCONTEXT);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (mtx) CloseHandle(mtx);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}
