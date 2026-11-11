// main.cpp — 设备任务栏胶囊 UI 层
// WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE 分层窗口；GDI+ ARGB 玻璃胶囊；UpdateLayeredWindow 呈现。
// （呈现机制说明见 README「已知偏差」：分层半透明玻璃替代 DWM system backdrop，视觉一致、无绘制残留。）
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <string>
#include <cstdio>
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
static ULONG_PTR g_gdiplusToken = 0;

enum { IDT_POLL = 1, IDT_LIFE = 2 };
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

// 测量：布局 = pad + 闪电 + 名字 + 10 + 进度条76 + 6 + 百分比 + pad
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
        Gdiplus::RectF re, rn, rp;
        {
            Gdiplus::Graphics gr(mem);
            gr.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            Gdiplus::Font fEmoji(L"Segoe UI Emoji", 10.0f);
            Gdiplus::Font fName(L"Segoe UI", 9.5f);
            Gdiplus::Font fPct(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold);
            gr.MeasureString(L"⚡", -1, &fEmoji, Gdiplus::PointF(0, 0), &re);
            gr.MeasureString(name.c_str(), -1, &fName, Gdiplus::PointF(0, 0), &rn);
            gr.MeasureString(pct, -1, &fPct, Gdiplus::PointF(0, 0), &rp);
        } // gr 先析构
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        w = 12 + (int)re.Width + 4 + (int)rn.Width + 10 + 76 + 6 + (int)rp.Width + 12;
    }
    if (w < 160) w = 160;
    if (w > 420) w = 420; // 极端长名防溢出（名字另有 24 字符截断）
    return w;
}

// 锚定：Shell_TrayWnd → TrayNotifyWnd 左缘 -8 右对齐；垂直居中
static void anchor() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    RECT tr{};
    if (!GetWindowRect(tray, &tr)) return;
    LONG right = tr.right - 340; // 回退：任务栏右缘内收
    HWND tn = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
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

    const float r = H / 2.0f; // 胶囊形
    Gdiplus::GraphicsPath bg;
    roundRectPath(bg, 0.5f, 0.5f, (float)W - 1, (float)H - 1, r - 0.5f);
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(0xD8, 0xFF, 0xFF, 0xFF)); // 半透明白
    gr.FillPath(&bgBrush, &bg);
    Gdiplus::Pen edge(Gdiplus::Color(0x4D, 0xFF, 0xFF, 0xFF), 1.0f);
    gr.DrawPath(&edge, &bg);

    Gdiplus::Font fEmoji(L"Segoe UI Emoji", 10.0f);
    Gdiplus::Font fName(L"Segoe UI", 9.5f);
    Gdiplus::Font fPct(L"Segoe UI", 9.5f, Gdiplus::FontStyleBold);
    Gdiplus::SolidBrush textBr(Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));
    Gdiplus::SolidBrush pctBr(g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xD7, 0x00, 0x15)
                                              : Gdiplus::Color(0xE6, 0x20, 0x20, 0x20));

    float y = (H - 16.0f) / 2.0f;
    float x = 12.0f;

    // 闪电
    Gdiplus::RectF re;
    gr.MeasureString(L"⚡", -1, &fEmoji, Gdiplus::PointF(0, 0), &re);
    gr.DrawString(L"⚡", -1, &fEmoji, Gdiplus::PointF(x, y - 2.0f), &textBr);
    x += re.Width + 4.0f;

    // 设备名
    std::wstring name = utf8ToWide(g_vm.displayName);
    if (name.empty()) name = L"--";
    Gdiplus::RectF rn;
    gr.MeasureString(name.c_str(), -1, &fName, Gdiplus::PointF(0, 0), &rn);
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
        Gdiplus::LinearGradientBrush bar(
            Gdiplus::RectF(x, barY, fillW, barH),
            g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xFF, 0x45, 0x3A) : Gdiplus::Color(0xFF, 0x34, 0xC7, 0x59),
            g_vm.lowBattery ? Gdiplus::Color(0xFF, 0xFF, 0x69, 0x61) : Gdiplus::Color(0xFF, 0x2F, 0xB4, 0x5A),
            0.0f);
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
    gr.DrawString(pct, -1, &fPct, Gdiplus::PointF(x, y), &pctBr);

    gr.Flush();

    HBITMAP hBmp = nullptr;
    if (surface.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBmp) != Gdiplus::Ok || !hBmp) return;
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, hBmp);

    POINT ptSrc{0, 0};
    POINT ptDst{g_x, g_y};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_ALPHA, ULW_ALPHA, 255, 0};
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

static void applyVisibility() {
    bool want = g_vm.visible;
    bool is = IsWindowVisible(g_hwnd) != 0;
    if (want && !is) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    } else if (!want && is) {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

static void present() {
    g_capsuleW = measureWidth();
    anchor();
    render();
}

static void sampleNow() {
    Snapshot s = adapters::ReadRegistrySnapshot();
    s.hostPresent = adapters::IsHostRunning();
    g_snap = std::move(s);
    g_vm = Reduce(g_snap, g_cfg);
    applyPollTimer();
    present();
    applyVisibility();
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"打开FerrisPulse");
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"立即刷新");
    HMENU sub = CreatePopupMenu();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"自动同步频率");
    AppendMenuW(sub, MF_STRING | (g_cfg.freqMode == FreqMode::FollowHost ? MF_CHECKED : 0),
                ID_FREQ_FOLLOW, L"跟随宿主（默认）");
    AppendMenuW(sub, MF_STRING | (g_cfg.freqMode == FreqMode::Sec30 ? MF_CHECKED : 0),
                ID_FREQ_30S, L"30秒");
    AppendMenuW(sub, MF_STRING | (g_cfg.freqMode == FreqMode::Min1 ? MF_CHECKED : 0),
                ID_FREQ_1M, L"1分钟");
    AppendMenuW(sub, MF_STRING | (g_cfg.freqMode == FreqMode::Min5 ? MF_CHECKED : 0),
                ID_FREQ_5M, L"5分钟");
    BOOL on = adapters::IsAutostartOn();
    AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), ID_AUTOSTART,
                L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"退出本程序");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd); // 让菜单在别处点击时能正常关闭
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case ID_OPEN: adapters::ShowHostWindow(); break;
        case ID_REFRESH: sampleNow(); break; // 不打乱采样相位（仅即时重读）
        case ID_FREQ_FOLLOW: g_cfg.freqMode = FreqMode::FollowHost; goto cfg;
        case ID_FREQ_30S: g_cfg.freqMode = FreqMode::Sec30; goto cfg;
        case ID_FREQ_1M: g_cfg.freqMode = FreqMode::Min1; goto cfg;
        case ID_FREQ_5M: g_cfg.freqMode = FreqMode::Min5; goto cfg;
        case ID_AUTOSTART: adapters::SetAutostart(!adapters::IsAutostartOn()); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default: break;
    }
    return;
cfg:
    adapters::SaveConfig(g_cfg);
    sampleNow(); // applyPollTimer 生效新档位
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
            } else if (wp == IDT_LIFE) {
                bool host = adapters::IsHostRunning();
                if (host != g_snap.hostPresent) {
                    sampleNow(); // 宿主出现/退出 → 立即重采
                } else {
                    int ox = g_x, oy = g_y, ow = g_capsuleW, oh = g_capsuleH;
                    g_capsuleW = measureWidth();
                    anchor();
                    if (g_x != ox || g_y != oy || g_capsuleW != ow || g_capsuleH != oh) render();
                    applyVisibility();
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

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (mtx) CloseHandle(mtx);
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}
