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
// 任务栏显隐/移动的 WinEvent 钩子
static HWINEVENTHOOK g_hookTray = nullptr;
// 任务栏窗口缓存 + 朝向（垂直任务栏放不下胶囊，直接隐藏而不是错位）
static HWND g_tray = nullptr;
static bool g_trayVertical = false;
// SetParent 方案：渲染窗口挂成任务栏子窗口（排在 XAML 合成层之上，不被面板遮挡）；
// 交互窗口是独立顶层分层窗（子窗口收不到鼠标消息，必须分离）。
static bool g_child = false;
static POINT g_clientPos{0, 0};
static HWND g_hitHwnd = nullptr;

// ── DPI 缩放 ───────────────────────────────────────────────────────
// 进程是 PER_MONITOR_AWARE_V2：所有尺寸用物理像素。设计稿按 96 DPI(=100%) 绘制，
// 实际绘制时统一乘 g_scale，使 125%/150%/200% 屏幕上视觉大小一致。
// 基准尺寸（100% 缩放下的物理像素）集中在此，绘制代码不再散落魔法数字。
namespace metrics {
constexpr float kFontPx     = 13.0f;  // 字号（像素，UnitPixel）
constexpr float kPadX       = 12.0f;  // 左右内边距
constexpr float kNameGap    = 10.0f;  // 名字 → 进度条
constexpr float kBarW       = 76.0f;  // 进度条宽
constexpr float kBarH       = 8.0f;   // 进度条高
constexpr float kBarGap     = 6.0f;   // 进度条 → 百分比
constexpr float kTextH      = 16.0f;  // 文字行高（垂直居中用）
constexpr float kMinW       = 160.0f; // 胶囊最小/最大宽
constexpr float kMaxW       = 420.0f;
constexpr float kEdgeInset  = 8.0f;   // 胶囊右缘距托盘左缘
constexpr float kTaskbarPad = 16.0f;  // 任务栏高度减去此值 = 胶囊高
constexpr float kMinH       = 24.0f;
constexpr float kMaxH       = 40.0f;
} // namespace metrics

static float g_scale = 1.0f;   // 当前显示器缩放系数（96 DPI = 1.0）

// 取窗口所在显示器的缩放；跨屏拖动后需重算
static void refreshDpiScale() {
    UINT dpi = 96;
    if (g_hwnd) dpi = GetDpiForWindow(g_hwnd);
    if (dpi == 0) dpi = 96;
    g_scale = (float)dpi / 96.0f;
}

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
// 挂成任务栏子窗口。实证可行的确切顺序（探针程序 combo.exe 验证）：
//   1) 窗口以 popup 创建（WS_EX_LAYERED 保留）
//   2) SetWindowLongPtrW(GWL_EXSTYLE, ex & ~WS_EX_TOPMOST)
//   3) SetWindowLongPtrW(GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD)   ← 必须先切 CHILD
//   4) SetParent(hwnd, tray)                                          ← 再 SetParent
// 顺序反了（先 SetParent 再切样式）只会得到 owner 关系，胶囊仍被面板遮挡。
static bool attachToTray() {
    refreshTray();
    if (!g_tray || !g_hwnd) return false;
    if (g_child && GetParent(g_hwnd) == g_tray) return true;

    LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
    SetParent(g_hwnd, g_tray);

    if (GetParent(g_hwnd) != g_tray) {   // 未挂上 → 还原，稍后重试
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, (style & ~WS_CHILD) | WS_POPUP);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
        return false;
    }
    g_child = true;   // 子窗口 z 序由父窗口决定，无需再置顶
    return true;
}

// 创建渲染窗口（popup 起步，随后 attachToTray 转成任务栏子窗口）
static bool createRenderWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST,
                             L"DeviceTaskbarCapsuleWnd", L"",
                             WS_POPUP, 0, 0, 10, 10,
                             nullptr, nullptr, inst, nullptr);
    g_child = false;
    return g_hwnd != nullptr;
}

// 单实例互斥（自重启时需要先释放，否则新进程会被挡掉）
static HANDLE g_mtx = nullptr;

// 充电呼吸动画开关（仅在「充电 + 可见」时挂 40ms 定时器，其余时刻零开销）
static bool g_animOn = false;
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

// 测量：布局 = pad + 名字 + gap + 进度条 + gap + 百分比 + pad（全部按 DPI 缩放）
// 注意：Graphics 必须先于 DeleteDC 析构（内层作用域），否则析构触碰已释放 DC（UB）。
static int measureWidth() {
    std::wstring name = utf8ToWide(g_vm.displayName);
    if (name.empty()) name = L"--";
    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", g_vm.percent);

    const float s = g_scale;
    const float fontPx = metrics::kFontPx * s;
    int w = (int)(metrics::kMinW * s);
    {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        Gdiplus::RectF rn, rp;
        {
            Gdiplus::Graphics gr(mem);
            gr.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            // UnitPixel：直接用物理像素，避免 pt→px 的隐式 96 DPI 假设
            Gdiplus::Font fName(L"Segoe UI", fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font fPct(L"Segoe UI", fontPx, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            gr.MeasureString(name.c_str(), -1, &fName, Gdiplus::PointF(0, 0), &rn);
            gr.MeasureString(pct, -1, &fPct, Gdiplus::PointF(0, 0), &rp);
        } // gr 先析构
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        // 文字宽度已是物理像素，常量需乘 s
        w = (int)(metrics::kPadX * s + rn.Width + metrics::kNameGap * s + metrics::kBarW * s +
                  metrics::kBarGap * s + rp.Width + metrics::kPadX * s);
    }
    const int minW = (int)(metrics::kMinW * s);
    const int maxW = (int)(metrics::kMaxW * s);
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
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
    refreshDpiScale();   // 任务栏可能跨屏移动过，重新取当前显示器 DPI
    const float s = g_scale;
    int taskH = tr.bottom - tr.top;
    g_capsuleH = taskH - (int)(metrics::kTaskbarPad * s);
    const int minH = (int)(metrics::kMinH * s);
    const int maxH = (int)(metrics::kMaxH * s);
    if (g_capsuleH < minH) g_capsuleH = minH;
    if (g_capsuleH > maxH) g_capsuleH = maxH;
    g_y = tr.top + (taskH - g_capsuleH) / 2;
    g_x = right - (int)(metrics::kEdgeInset * s) - g_capsuleW;
    POINT p{g_x, g_y};
    if (ScreenToClient(g_tray, &p)) g_clientPos = p; // 子窗口用客户区坐标
}

// 交互窗口：与胶囊同位置同尺寸；内容盒 alpha=1（1/255，肉眼不可见但可命中），
// 盒外 alpha=0 → 点击穿透到任务栏图标。渲染窗是子窗口、收不到鼠标，点击全靠它。
static void syncHitWindow(int W, int H) {
    if (!g_hitHwnd) return;
    const bool vis = IsWindowVisible(g_hwnd) != 0;
    if (!vis) {
        if (IsWindowVisible(g_hitHwnd)) ShowWindow(g_hitHwnd, SW_HIDE);
        return;
    }
    Gdiplus::Bitmap hit(W, H, PixelFormat32bppPARGB);
    if (hit.GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics g(&hit);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::SolidBrush box(Gdiplus::Color(1, 0, 0, 0));
        if (g_hitR > g_hitL && g_hitB > g_hitT)
            g.FillRectangle(&box, g_hitL, g_hitT, g_hitR - g_hitL, g_hitB - g_hitT);
        g.Flush();
        HBITMAP hb = nullptr;
        if (hit.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hb) == Gdiplus::Ok && hb) {
            HDC mdc = CreateCompatibleDC(nullptr);
            HGDIOBJ old = SelectObject(mdc, hb);
            POINT src{0, 0}, dst{g_x, g_y};
            SIZE sz{W, H};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(g_hitHwnd, nullptr, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
            SelectObject(mdc, old);
            DeleteDC(mdc);
            DeleteObject(hb);
        }
    }
    if (!IsWindowVisible(g_hitHwnd)) ShowWindow(g_hitHwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hitHwnd, HWND_TOPMOST, g_x, g_y, W, H,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
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

    // DPI 缩放：所有尺寸乘 g_scale，保证 125%/150%/200% 下视觉一致
    const float s = g_scale;
    const float fontPx = metrics::kFontPx * s;
    const float padX   = metrics::kPadX * s;
    const float textH  = metrics::kTextH * s;

    // 背景完全透明（无底板/描边）——仅绘制文字与进度条，
    // 让胶囊下方任务栏图标透出；文字加白色描边保证图标底上可读。
    Gdiplus::Font fName(L"Segoe UI", fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fPct(L"Segoe UI", fontPx, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBr(Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));
    Gdiplus::SolidBrush outlineBr(Gdiplus::Color(0xD0, 0xFF, 0xFF, 0xFF));
    Gdiplus::SolidBrush pctBr(g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xD7, 0x00, 0x15)
                                              : Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));

    float y = (H - textH) / 2.0f;
    float x = padX;

    // 设备名（白色描边 + 正文）
    std::wstring name = utf8ToWide(g_vm.displayName);
    if (name.empty()) name = L"--";
    Gdiplus::RectF rn;
    gr.MeasureString(name.c_str(), -1, &fName, Gdiplus::PointF(0, 0), &rn);
    gr.DrawString(name.c_str(), -1, &fName, Gdiplus::PointF(x + s, y + s), &outlineBr);
    gr.DrawString(name.c_str(), -1, &fName, Gdiplus::PointF(x, y), &textBr);
    x += rn.Width + metrics::kNameGap * s;

    // 进度条：轨道 + 渐变填充（低电量红色）
    const float barW = metrics::kBarW * s, barH = metrics::kBarH * s;
    float barY = (H - barH) / 2.0f;
    Gdiplus::GraphicsPath track;
    roundRectPath(track, x, barY, barW, barH, barH / 2.0f);
    Gdiplus::SolidBrush trackBr(Gdiplus::Color(0x33, 0x00, 0x00, 0x00));
    gr.FillPath(&trackBr, &track);
    float fillW = barW * g_vm.percent / 100.0f;
    const float fillMin = 4.0f * s;
    if (g_vm.percent > 0 && fillW < fillMin) fillW = fillMin;
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
        Gdiplus::LinearGradientBrush bar(Gdiplus::RectF(x, barY, fillW, barH), c1, c2, 0.0f);
        Gdiplus::GraphicsPath fill;
        roundRectPath(fill, x, barY, fillW, barH, barH / 2.0f);
        gr.SetClip(&fill);
        gr.FillRectangle(&bar, x, barY, fillW, barH);
        gr.ResetClip();
    }
    x += barW + metrics::kBarGap * s;

    // 百分比
    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", g_vm.percent);
    Gdiplus::RectF rp;
    gr.MeasureString(pct, -1, &fPct, Gdiplus::PointF(0, 0), &rp);
    gr.DrawString(pct, -1, &fPct, Gdiplus::PointF(x + s, y + s), &outlineBr);
    gr.DrawString(pct, -1, &fPct, Gdiplus::PointF(x, y), &pctBr);

    // 内容命中盒：盒外点击穿透到任务栏（透明像素不吞点击）——同样按 DPI 缩放
    g_hitL = (int)padX;
    g_hitR = (int)(x + rp.Width);
    g_hitT = (int)y;
    g_hitB = (int)(y + textH);

    gr.Flush();

    HBITMAP hBmp = nullptr;
    if (surface.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBmp) != Gdiplus::Ok || !hBmp) return;
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, hBmp);

    POINT ptSrc{0, 0};
    POINT ptDst{g_x, g_y};
    if (g_child) ptDst = g_clientPos;
    SIZE sz{W, H};
    syncHitWindow(W, H);
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
    if (g_child) return; // 子窗口 z 序由父窗口决定
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
static void applyVisibility() {
    const bool want = g_vm.visible && !g_trayVertical && trayOnScreen();
    const bool is = IsWindowVisible(g_hwnd) != 0;
    if (want && !is) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        raiseTopmost();
        syncHitWindow(g_capsuleW, g_capsuleH);
    } else if (!want && is) {
        ShowWindow(g_hwnd, SW_HIDE);
        if (g_hitHwnd && IsWindowVisible(g_hitHwnd)) ShowWindow(g_hitHwnd, SW_HIDE);
    } else if (want && is && g_hitHwnd && !IsWindowVisible(g_hitHwnd)) {
        syncHitWindow(g_capsuleW, g_capsuleH);
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

static void sampleNow() {
    Snapshot s = adapters::ReadRegistrySnapshot();
    s.hostPresent = adapters::IsHostRunning();
    g_snap = std::move(s);
    g_vm = Reduce(g_snap, g_cfg);
    applyPollTimer();
    layout(true);
}

// explorer 重启会连带销毁任务栏子窗口；实测进程内重建后 ULW 不出图（永远空白），
// 故改为「重启自己」：新进程走正常初始化。旧进程必须真正退出并释放互斥，否则出现双实例。
static void restartSelf() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    // 关键：不创建子进程，而是交给外部延时启动，避免与本进程抢互斥。
    // 做法：先自杀（PostQuitMessage → 进程退出），再让 shell 起新实例。
    std::wstring cmd = L"/c timeout /t 1 /nobreak >nul & start \"\" \"";
    cmd += path;
    cmd += L"\"";
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys) + L"\\cmd.exe";
    STARTUPINFOW si2{};
    si2.cb = sizeof(si2);
    si2.dwFlags = STARTF_USESHOWWINDOW;
    si2.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi2{};
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2)) {
        CloseHandle(pi2.hProcess);
        CloseHandle(pi2.hThread);
    }
    if (g_mtx) { ReleaseMutex(g_mtx); CloseHandle(g_mtx); g_mtx = nullptr; }
    PostQuitMessage(0);   // 立即退出消息循环 → 进程结束，把互斥让给新实例
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
    // 关键：owner 必须是「交互窗口」(顶层)，不能用 g_hwnd。
    // g_hwnd 是任务栏子窗口 —— 把它设为前台会把任务栏拉到前台，于是弹出的是任务栏的菜单。
    HWND owner = g_hitHwnd ? g_hitHwnd : g_hwnd;
    // 先让当前前台窗口（可能是任务栏/开始菜单）失去激活，菜单才会正常接管；
    // 但不能 SetForegroundWindow(owner)：交互窗是 NOACTIVATE 的，强设会被拒绝并留下任务栏在前台。
    SetForegroundWindow(owner);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                pt.x, pt.y, owner, nullptr);
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

// 任务栏显隐/滑入滑出（自动隐藏）→ 立即跟随，不等 2s 轮询
static void CALLBACK onTrayEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    // 任务栏（含自动隐藏滑入滑出、explorer 重启换句柄）→ 立即跟随
    if (isTrayClass(hwnd)) {
        const bool newTray = (g_tray != nullptr && hwnd != g_tray);
        g_tray = hwnd;
        // explorer 重启 → 任务栏子窗口已被连带销毁 → 重启自己（进程内重建无法出图）
        if (newTray) restartSelf();
        layout(false);
        return;
    }
}

// 交互窗口过程：只处理鼠标（渲染窗是子窗口，收不到鼠标消息）
static LRESULT CALLBACK hitProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_LBUTTONDOWN:
            adapters::ToggleHostWindow();
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showMenu();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DESTROY:
            g_hitHwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            // 渲染窗是任务栏子窗口，收不到系统点击派发；这里按内容盒返回 CLIENT，
            // 命中后由下面的 WM_*BUTTON 分支直接转发给交互逻辑（同进程调用，最可靠）。
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            if (pt.x >= g_hitL && pt.x < g_hitR && pt.y >= g_hitT && pt.y < g_hitB)
                return HTCLIENT;
            return HTTRANSPARENT;
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
                    // 自愈：渲染窗被销毁（任务栏重建）→ 重启自己；尚未挂成子窗口 → 重试挂载
                    if (!IsWindow(g_hwnd)) {
                        restartSelf();
                    } else if (!g_child) {
                        attachToTray();
                    }
                    layout(false);
                    if (!g_child && IsWindowVisible(g_hwnd)) raiseTopmost();
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
        case WM_DPICHANGED:
            // 跨显示器拖动：DPI 变了 → 重算缩放、尺寸、位置并重绘
            refreshDpiScale();
            layout(true);
            return 0;
        case WM_DESTROY:
            if (g_hitHwnd) { DestroyWindow(g_hitHwnd); g_hitHwnd = nullptr; }
            KillTimer(hwnd, IDT_POLL);
            KillTimer(hwnd, IDT_LIFE);
            KillTimer(hwnd, IDT_ANIM);
            g_animOn = false;
            if (g_hookTray) { UnhookWinEvent(g_hookTray); g_hookTray = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // DPI 感知（多显示器不同缩放时按物理像素锚定）
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 单实例（胶囊自身的本地互斥；与宿主的全局互斥无关）
    g_mtx = CreateMutexW(nullptr, TRUE, L"Local\\DeviceTaskbarCapsule_SingleInstance");
    if (g_mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 自重启场景：旧进程正在退出，等它释放互斥（最多 3 秒）
        CloseHandle(g_mtx);
        g_mtx = nullptr;
        for (int i = 0; i < 30; ++i) {
            Sleep(100);
            g_mtx = CreateMutexW(nullptr, TRUE, L"Local\\DeviceTaskbarCapsule_SingleInstance");
            if (g_mtx && GetLastError() != ERROR_ALREADY_EXISTS) break;
            if (g_mtx) { CloseHandle(g_mtx); g_mtx = nullptr; }
        }
        if (!g_mtx) return 0;   // 旧实例仍在运行 → 本进程退出
    }

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

    WNDCLASSEXW hc = wc;
    hc.lpfnWndProc = hitProc;
    hc.lpszClassName = L"DeviceTaskbarCapsuleHitWnd";
    RegisterClassExW(&hc);

    if (!createRenderWindow()) return 1;

    // 交互窗口：独立顶层分层窗（子窗口收不到鼠标消息）
    g_hitHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST,
                                hc.lpszClassName, L"",
                                WS_POPUP,
                                0, 0, 10, 10,
                                nullptr, nullptr, inst, nullptr);

    attachToTray(); // 挂成任务栏子窗口（失败时 LIFE 定时器持续重试）
    sampleNow(); // 首采：决定是否显示
    SetTimer(g_hwnd, IDT_LIFE, 2000, nullptr);

    // 任务栏显隐/滑入滑出/explorer 重启 → 立即跟随（2s LIFE 仅兜底）
    refreshTray();
    g_hookTray = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE,
                                 nullptr, onTrayEvent, 0, 0, WINEVENT_OUTOFCONTEXT);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_mtx) { CloseHandle(g_mtx); g_mtx = nullptr; }
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}