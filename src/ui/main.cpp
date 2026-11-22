// main.cpp — 设备任务栏胶囊 UI 层
// WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE 分层窗口；GDI+ ARGB 玻璃胶囊；UpdateLayeredWindow 呈现。
// （呈现机制说明见 README「已知偏差」：分层半透明玻璃替代 DWM system backdrop，视觉一致、无绘制残留。）
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include "core/CapsuleCore.h"
#include "adapters/RegistryReader.h"
#include "adapters/Environment.h"

using namespace capsule;

static Config g_cfg;
static Snapshot g_snap;
static ViewModel g_vm;

// ── 每个任务栏一份实例（主屏 Shell_TrayWnd + 各副屏 Shell_SecondaryTrayWnd）──
// 渲染窗 = 该任务栏的 WS_CHILD 子窗口（排在 XAML 合成层之上）；
// 交互窗 = 对应的顶层分层窗（子窗口收不到鼠标消息，必须分离）。
struct Capsule {
    HWND bar = nullptr;       // 所属任务栏（主/副）
    HWND render = nullptr;    // 渲染子窗口
    HWND hit = nullptr;       // 交互窗口
    bool child = false;       // render 是否已成功挂成子窗口
    POINT clientPos{0, 0};    // render 在任务栏客户区的坐标
    int x = -10000, y = -10000;   // 屏幕坐标（交互窗定位用）
    bool vertical = false;    // 垂直任务栏
};

static Capsule g_main;              // 主屏
static std::vector<Capsule> g_mirrors;  // 副屏

// 兼容旧代码的别名（主实例）
#define g_hwnd      (g_main.render)
#define g_hitHwnd   (g_main.hit)
#define g_child     (g_main.child)
#define g_clientPos (g_main.clientPos)
#define g_x         (g_main.x)
#define g_y         (g_main.y)
#define g_trayVertical (g_main.vertical)

static HWND g_tray = nullptr;   // 主任务栏缓存
static int g_capsuleW = 240, g_capsuleH = 32;
static bool g_hover = false;    // 鼠标悬停在胶囊上（画一层淡淡的高亮）
// Win10 任务栏：任务按钮在 ReBarWindow32 里，且无 Win11 的合成层桥。
// Win11 是 XAML 岛（有 DesktopWindowContentBridge），无需避让。
static HWND g_rebar = nullptr;
// 内容命中盒（窗口内相对值）：盒外点击穿透到任务栏图标
static int g_hitL = 0, g_hitR = 0, g_hitT = 0, g_hitB = 0;
// 任务栏事件钩子。只订阅三类事件（SHOW / HIDE / LOCATIONCHANGE）：
// 早期用 [EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE] 整个区间，
// 区间内含 FOCUS / SELECTION / STATECHANGE 等组，会把全系统的这类事件
// 都编组进本进程（OUTOFCONTEXT 跨进程投递），实测是 CPU 占用的主要来源。
static HWINEVENTHOOK g_hooks[3] = {nullptr, nullptr, nullptr};

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
// 挂成任务栏子窗口（唯一形态，无 overlay 兜底：顶层窗口会被任务栏覆盖）。
// 顺序固定且不可换：去 TOPMOST → 切 WS_CHILD → SetParent；已显示的窗口必须先 SW_HIDE，
// 否则 SetParent 返回 ERROR_ALREADY_EXISTS(183)。失败则保持隐藏，由 LIFE 定时器重试。
static bool attachFor(Capsule& c) {
    if (!c.bar || !IsWindow(c.render) || !IsWindow(c.bar)) return false;
    if (c.child && GetParent(c.render) == c.bar) return true;

    const bool wasVisible = IsWindowVisible(c.render) != 0;
    if (wasVisible) ShowWindow(c.render, SW_HIDE);   // 已显示时 SetParent 会返回 183

    LONG_PTR style = GetWindowLongPtrW(c.render, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(c.render, GWL_EXSTYLE);
    SetWindowLongPtrW(c.render, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);          // ① 去置顶
    SetWindowLongPtrW(c.render, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD); // ② 先切子窗口
    SetParent(c.render, c.bar);                                             //   再挂父窗口

    const bool ok = (GetParent(c.render) == c.bar);
    c.child = ok;
    if (ok && wasVisible) ShowWindow(c.render, SW_SHOWNOACTIVATE);
    return ok;
}

static bool attachToTray() {
    refreshTray();
    g_main.bar = g_tray;
    return attachFor(g_main);
}

// 创建一对窗口（渲染 popup + 交互窗），供主屏/副屏共用。
// 渲染窗不带 WS_EX_TOPMOST（顶置窗口无法挂成子窗口）。
static bool createPair(Capsule& c) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!c.render) {
        c.render = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                   L"DeviceTaskbarCapsuleWnd", L"",
                                   WS_POPUP, 0, 0, 10, 10,
                                   nullptr, nullptr, inst, nullptr);
        if (!c.render) return false;
    }
    if (!c.hit) {
        c.hit = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST,
                                L"DeviceTaskbarCapsuleHitWnd", L"",
                                WS_POPUP, 0, 0, 10, 10,
                                nullptr, nullptr, inst, nullptr);
    }
    c.child = false;
    return c.hit != nullptr;
}

// 主实例便捷封装
static bool createRenderWindow() { return createPair(g_main); }

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

    // 结果缓存：2s 心跳里绝大多数时刻文本并未变化，
    // 避免每 tick 重复 GetDC/CreateDC/建 2 个 Font/MeasureString。
    static std::wstring cName;
    static int cPct = -999;
    static float cScale = -1.0f;
    static int cW = 0;
    if (cW > 0 && cPct == g_vm.percent && cScale == s && cName == name) return cW;
    cName = name; cPct = g_vm.percent; cScale = s;
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
    cW = w;
    return w;
}

// 锚定：任意任务栏 → 其托盘区左缘 -inset 右对齐；垂直居中。
// 只支持水平任务栏（上/下）：垂直任务栏宽度不足，由 applyVisibility 隐藏。
static void anchorFor(Capsule& c) {
    if (!c.bar || !IsWindow(c.bar)) return;
    RECT tr{};
    if (!GetWindowRect(c.bar, &tr)) return;
    c.vertical = (tr.bottom - tr.top) > (tr.right - tr.left);
    if (c.vertical) return;
    LONG right = tr.right - 340;   // 回退：任务栏右缘内收
    HWND tn = FindWindowExW(c.bar, nullptr, L"TrayNotifyWnd", nullptr);
    if (tn) {
        RECT r{};
        if (GetWindowRect(tn, &r)) right = r.left;
    }
    // 每个任务栏按其所在显示器取 DPI
    UINT dpi = c.render ? GetDpiForWindow(c.render) : 96;
    if (dpi == 0) dpi = 96;
    const float s = (float)dpi / 96.0f;
    if (&c == &g_main) { g_scale = s; refreshDpiScale(); }   // 主实例维护全局缩放

    int taskH = tr.bottom - tr.top;
    int h = taskH - (int)(metrics::kTaskbarPad * s);
    const int minH = (int)(metrics::kMinH * s);
    const int maxH = (int)(metrics::kMaxH * s);
    if (h < minH) h = minH;
    if (h > maxH) h = maxH;
    if (&c == &g_main) g_capsuleH = h;
    c.y = tr.top + (taskH - h) / 2;
    c.x = right - (int)(metrics::kEdgeInset * s) - g_capsuleW;
    POINT p{c.x, c.y};
    if (ScreenToClient(c.bar, &p)) c.clientPos = p;
}

static void anchor() { anchorFor(g_main); }

// Win10 适配：任务按钮容器（ReBarWindow32）默认占满任务栏剩余宽度，
// 会盖住/挤压右侧的胶囊。这里把它的宽度收窄到胶囊左缘，给它让出空间。
// Win11 无 ReBarWindow32（任务栏是 XAML 岛），本函数自动跳过。
static void avoidRebarOnWin10() {
    refreshTray();
    if (!g_tray || !g_main.render) return;
    // 平台判定只做一次（g_platform: 0=未知 1=Win11(XAML岛) 2=Win10(ReBar)）。
    // 注意 Win11 也可能残留 ReBarWindow32，故不能靠 rebar 是否存在判版本。
    static int g_platform = 0;
    if (g_platform == 0)
        g_platform = FindWindowExW(g_tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) ? 1 : 2;
    if (g_platform == 1) { g_rebar = nullptr; return; }   // Win11：直接返回，无系统调用

    if (!g_rebar || !IsWindow(g_rebar))
        g_rebar = FindWindowExW(g_tray, nullptr, L"ReBarWindow32", nullptr);
    if (!g_rebar) return;

    RECT rb{}, tb{};
    if (!GetClientRect(g_rebar, &rb) || !GetWindowRect(g_tray, &tb)) return;
    // 目标宽度：胶囊左缘相对任务栏客户区的 x
    POINT p{g_main.x, 0};
    if (!ScreenToClient(g_tray, &p)) return;
    const int wantW = p.x < 0 ? 0 : p.x;
    static int g_rebarAppliedW = -1;
    if (wantW > 0 && wantW != rb.right && wantW != g_rebarAppliedW) {
        SetWindowPos(g_rebar, nullptr, 0, 0, wantW, rb.bottom,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_rebarAppliedW = wantW;
    }
}

// 枚举所有「有托盘区」的水平副屏任务栏
static std::vector<HWND> secondaryBars() {
    std::vector<HWND> out;
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (!IsWindowVisible(t)) continue;
        RECT r{};
        if (!GetWindowRect(t, &r)) continue;
        if (r.right - r.left <= r.bottom - r.top) continue;   // 只要水平任务栏
        if (!FindWindowExW(t, nullptr, L"TrayNotifyWnd", nullptr)) continue;  // 无托盘区则跳过
        out.push_back(t);
    }
    return out;
}

// 交互窗口：与胶囊同位置同尺寸；内容盒 alpha=1（1/255，肉眼不可见但可命中），
// 盒外 alpha=0 → 点击穿透到任务栏图标。渲染窗是子窗口、收不到鼠标，点击全靠它。
static void syncHitFor(Capsule& c, int W, int H) {
    if (!c.hit || !c.render) return;
    const bool vis = IsWindowVisible(c.render) != 0;
    if (!vis) {
        if (IsWindowVisible(c.hit)) ShowWindow(c.hit, SW_HIDE);
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
            POINT src{0, 0}, dst{c.x, c.y};
            SIZE sz{W, H};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(c.hit, nullptr, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
            SelectObject(mdc, old);
            DeleteDC(mdc);
            DeleteObject(hb);
        }
    }
    if (!IsWindowVisible(c.hit)) ShowWindow(c.hit, SW_SHOWNOACTIVATE);
    SetWindowPos(c.hit, HWND_TOPMOST, c.x, c.y, W, H,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

static void syncHitWindow(int W, int H) { syncHitFor(g_main, W, H); }

// ── 渲染表面缓存（内存优化核心）───────────────────────────────────
// 原实现每帧新建 Bitmap / 内存 DC / 2 个 Font / 4 个 Brush，并 GetHBITMAP 再复制一次。
// 这里常驻一份 DIB section（32bpp、自上而下）+ 内存 DC + GDI+ Bitmap 包装，
// 画刷与字体按字号缓存：仅尺寸或 DPI 变化时重建，稳态下每帧零堆分配。
struct Surface {
    int w = 0, h = 0;
    HBITMAP dib = nullptr;
    HDC dc = nullptr;
    void* bits = nullptr;
    Gdiplus::Bitmap* bmp = nullptr;
    float fontPx = -1.0f;
    Gdiplus::Font* fName = nullptr;
    Gdiplus::Font* fPct = nullptr;
    Gdiplus::SolidBrush* brText = nullptr;
    Gdiplus::SolidBrush* brOutline = nullptr;
    Gdiplus::SolidBrush* brPct = nullptr;
    Gdiplus::SolidBrush* brPctLow = nullptr;
};
static Surface g_surf;

static void releaseSurface() {
    delete g_surf.bmp; g_surf.bmp = nullptr;
    if (g_surf.dc) { DeleteDC(g_surf.dc); g_surf.dc = nullptr; }
    if (g_surf.dib) { DeleteObject(g_surf.dib); g_surf.dib = nullptr; }
    g_surf.bits = nullptr;
    g_surf.w = g_surf.h = 0;
}

static void releaseFonts() {
    delete g_surf.fName;  g_surf.fName = nullptr;
    delete g_surf.fPct;   g_surf.fPct = nullptr;
    delete g_surf.brText;    g_surf.brText = nullptr;
    delete g_surf.brOutline; g_surf.brOutline = nullptr;
    delete g_surf.brPct;     g_surf.brPct = nullptr;
    delete g_surf.brPctLow;  g_surf.brPctLow = nullptr;
    g_surf.fontPx = -1.0f;
}

static bool ensureFonts(float fontPx) {
    if (g_surf.fontPx == fontPx && g_surf.fName) return true;
    releaseFonts();
    g_surf.fName = new Gdiplus::Font(L"Segoe UI", fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_surf.fPct  = new Gdiplus::Font(L"Segoe UI", fontPx, Gdiplus::FontStyleBold,    Gdiplus::UnitPixel);
    if (g_surf.fName->GetLastStatus() != Gdiplus::Ok || g_surf.fPct->GetLastStatus() != Gdiplus::Ok) {
        releaseFonts();
        return false;
    }
    g_surf.brText    = new Gdiplus::SolidBrush(Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));
    g_surf.brOutline = new Gdiplus::SolidBrush(Gdiplus::Color(0xD0, 0xFF, 0xFF, 0xFF));
    g_surf.brPct     = new Gdiplus::SolidBrush(Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));
    g_surf.brPctLow  = new Gdiplus::SolidBrush(Gdiplus::Color(0xFF, 0xD7, 0x00, 0x15));
    g_surf.fontPx = fontPx;
    return true;
}

static bool ensureSurface(int w, int h) {
    if (g_surf.bmp && g_surf.w == w && g_surf.h == h) return true;
    releaseSurface();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      // 负高度 = 自上而下，与 GDI+ 坐标一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return false; }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DeleteObject(dib); return false; }
    SelectObject(dc, dib);
    auto* bmp = new Gdiplus::Bitmap(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp; DeleteDC(dc); DeleteObject(dib);
        return false;
    }
    g_surf.dib = dib; g_surf.dc = dc; g_surf.bits = bits; g_surf.bmp = bmp;
    g_surf.w = w; g_surf.h = h;
    return true;
}

// GDI+ 渲染 → 分层窗口呈现
static void renderFor(Capsule& c) {
    if (!c.render) return;
    const int W = g_capsuleW, H = g_capsuleH;
    if (W <= 0 || H <= 0) return;

    // DPI 缩放：所有尺寸乘 g_scale，保证 125%/150%/200% 下视觉一致
    const float s = g_scale;
    const float fontPx = metrics::kFontPx * s;
    const float padX   = metrics::kPadX * s;
    const float textH  = metrics::kTextH * s;

    // 复用常驻表面与缓存画刷/字体（尺寸或 DPI 变化时才重建）
    if (!ensureSurface(W, H) || !ensureFonts(fontPx)) return;
    Gdiplus::Graphics gr(g_surf.bmp);
    if (gr.GetLastStatus() != Gdiplus::Ok) return;
    // 复用缓冲区：必须以 SourceCopy + 全透明清空，否则上一帧像素残留
    gr.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    gr.Clear(Gdiplus::Color(0, 0, 0, 0));
    gr.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::Font& fName = *g_surf.fName;
    Gdiplus::Font& fPct  = *g_surf.fPct;
    Gdiplus::SolidBrush& textBr    = *g_surf.brText;
    Gdiplus::SolidBrush& outlineBr = *g_surf.brOutline;
    Gdiplus::SolidBrush& pctBr     = g_vm.lowBattery ? *g_surf.brPctLow : *g_surf.brPct;

    // 悬停：画一层极淡的圆角底板，给出"可点击"的视觉反馈
    if (g_hover && &c == &g_main) {
        Gdiplus::GraphicsPath plate;
        roundRectPath(plate, 0.5f, 0.5f, W - 1.0f, H - 1.0f, 6.0f * s);
        Gdiplus::SolidBrush plateBr(Gdiplus::Color(0x1A, 0x00, 0x00, 0x00));
        gr.FillPath(&plateBr, &plate);
    }

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

    // 内容命中盒：左右按内容宽度，上下覆盖整个胶囊高度 ——
    // 否则点击胶囊上下留白处会穿透到任务栏，弹出任务栏自己的菜单。
    g_hitL = (int)padX;
    g_hitR = (int)(x + rp.Width);
    g_hitT = 0;
    g_hitB = H;

    gr.Flush();

    POINT ptSrc{0, 0};
    POINT ptDst{c.x, c.y};
    if (c.child) ptDst = c.clientPos;
    SIZE sz{W, H};
    syncHitFor(c, W, H);
    // 字段序：{BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat}
    // AlphaFormat 必须 = AC_SRC_ALPHA 才启用逐像素混合（否则整窗按不透明拷贝，透明区变纯黑）
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(c.render, nullptr, &ptDst, &sz, g_surf.dc, &ptSrc, 0, &bf, ULW_ALPHA);
}

static void render() { renderFor(g_main); }

// ── 副屏镜像 ─────────────────────────────────────────────────────
// 每块副屏任务栏一份胶囊，与主屏同步显隐/内容。约每秒调一次（由 IDT_LIFE 驱动）。
static void destroyMirror(Capsule& c) {
    if (c.hit && IsWindow(c.hit)) DestroyWindow(c.hit);
    if (c.render && IsWindow(c.render)) DestroyWindow(c.render);
    c = Capsule{};
}

// 副屏枚举节流：枚举窗口是系统调用，没必要 2s 一次。
// 默认每 5 个心跳（约 10s）重新枚举；显示器拓扑变化由 WM_DISPLAYCHANGE 立即触发。
static bool g_resyncMirrors = true;
static int g_mirrorTick = 0;

static void syncMirrors(bool visible) {
    const bool fullScan = g_resyncMirrors || (++g_mirrorTick % 5 == 0);
    if (!visible) {                      // 主屏不可见 → 全部镜像也隐藏
        for (auto& m : g_mirrors) {
            if (m.render && IsWindowVisible(m.render)) ShowWindow(m.render, SW_HIDE);
            if (m.hit && IsWindowVisible(m.hit)) ShowWindow(m.hit, SW_HIDE);
        }
        return;
    }

    static std::vector<HWND> lastBars;
    if (fullScan) { lastBars = secondaryBars(); g_resyncMirrors = false; }
    const std::vector<HWND>& bars = lastBars;

    // 清理：任务栏消失 或 窗口没了
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        const bool barGone = std::find(bars.begin(), bars.end(), it->bar) == bars.end();
        if (barGone || !IsWindow(it->render)) { destroyMirror(*it); it = g_mirrors.erase(it); }
        else ++it;
    }

    // 新增：为每块新副屏建一对窗口
    for (HWND b : bars) {
        bool exists = false;
        for (const auto& m : g_mirrors) if (m.bar == b) { exists = true; break; }
        if (exists) continue;
        Capsule c;
        c.bar = b;
        if (createPair(c) && attachFor(c)) g_mirrors.push_back(c);
        else destroyMirror(c);
    }

    // 定位 + 绘制
    for (auto& m : g_mirrors) {
        anchorFor(m);
        if (m.vertical) {                 // 垂直副屏任务栏：不显示
            if (m.render && IsWindowVisible(m.render)) ShowWindow(m.render, SW_HIDE);
            continue;
        }
        if (!m.child) attachFor(m);
        if (!m.child) continue;
        if (!IsWindowVisible(m.render)) ShowWindow(m.render, SW_SHOWNOACTIVATE);
        renderFor(m);
    }
}

static void applyPollTimer() {
    static long long lastInterval = -1;
    long long iv = PollIntervalMs(g_snap, g_cfg);
    if (iv != lastInterval) {
        SetTimer(g_hwnd, IDT_POLL, (UINT)iv, nullptr);
        lastInterval = iv;
    }
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

// 胶囊可见性：宿主在跑 + 非垂直任务栏 + 任务栏真的露在屏上 + 已挂成子窗口
static void applyVisibility() {
    const bool want = g_vm.visible && !g_trayVertical && trayOnScreen();
    const bool is = IsWindowVisible(g_hwnd) != 0;
    if (want && !is) {
        // 只有挂成任务栏子窗口才显示；挂载失败时保持隐藏，绝不退回顶层窗口
        if (!g_child) return;
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        syncHitWindow(g_capsuleW, g_capsuleH);
    } else if (!want && is) {
        ShowWindow(g_hwnd, SW_HIDE);
        if (g_hitHwnd && IsWindowVisible(g_hitHwnd)) ShowWindow(g_hitHwnd, SW_HIDE);
    } else if (want && is && g_hitHwnd && !IsWindowVisible(g_hitHwnd)) {
        syncHitWindow(g_capsuleW, g_capsuleH);   // 渲染窗可见但命中窗丢了 → 补同步
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
    avoidRebarOnWin10();   // Win10 任务栏任务按钮避让（Win11 自动跳过）
    // 副屏镜像：跟随主屏的可见性与内容
    syncMirrors(g_vm.visible && !g_trayVertical && trayOnScreen());
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
    // owner 必须是「交互窗口」(顶层)：g_hwnd 是任务栏子窗口，拿它当 owner 会把菜单
    // 归到任务栏的 z 序 band，被开始菜单/任务栏盖住。
    HWND owner = g_hitHwnd ? g_hitHwnd : g_hwnd;

    // 菜单窗口的 z 序跟随 owner 的 band：开始菜单/快速设置在上层 band，
    // 因此菜单弹出前先把 owner 抬到 topmost 最前，保证菜单整体覆盖任务栏。
    SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetForegroundWindow(owner);

    // TPM_WORKAREA：让菜单避开任务栏区域，减少与任务栏重叠的概率。
    UINT cmd = TrackPopupMenuEx(m,
                                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_WORKAREA,
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
        case WM_MOUSEMOVE: {
            if (!g_hover) {
                g_hover = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                render();   // 重绘一次显示高亮
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hover) { g_hover = false; render(); }
            return 0;
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
        case WM_DISPLAYCHANGE:
            // 显示器插拔/分辨率变化 → 立即重扫副屏任务栏，不等节流窗口
            g_resyncMirrors = true;
            g_mirrorTick = 4;          // 下一次 syncMirrors 即触发全量扫描
            refreshDpiScale();
            layout(true);
            return 0;
        case WM_DESTROY:
            if (g_hitHwnd) { DestroyWindow(g_hitHwnd); g_hitHwnd = nullptr; }
            KillTimer(hwnd, IDT_POLL);
            KillTimer(hwnd, IDT_LIFE);
            KillTimer(hwnd, IDT_ANIM);
            g_animOn = false;
            for (auto& h : g_hooks) if (h) { UnhookWinEvent(h); h = nullptr; }
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

    // 一次性创建主实例的渲染窗 + 交互窗（createPair 内部去重）
    if (!createRenderWindow()) return 1;

    attachToTray(); // 挂成任务栏子窗口（失败时 LIFE 定时器持续重试）
    sampleNow(); // 首采：决定是否显示
    SetTimer(g_hwnd, IDT_LIFE, 2000, nullptr);

    // 任务栏显隐/滑入滑出/explorer 重启 → 立即跟随（2s LIFE 仅兜底）
    refreshTray();
    // 窄订阅 + 跳过本进程自身事件（我们自己也会 SetWindowPos，避免自激）
    const DWORD hookFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    g_hooks[0] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                 nullptr, onTrayEvent, 0, 0, hookFlags);
    g_hooks[1] = SetWinEventHook(EVENT_OBJECT_HIDE, EVENT_OBJECT_HIDE,
                                 nullptr, onTrayEvent, 0, 0, hookFlags);
    g_hooks[2] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                 nullptr, onTrayEvent, 0, 0, hookFlags);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    // GDI+ 资源必须先于 GdiplusShutdown 释放（否则析构会触碰已关闭的 GDI+）
    releaseSurface();
    releaseFonts();
    if (g_mtx) { CloseHandle(g_mtx); g_mtx = nullptr; }
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}