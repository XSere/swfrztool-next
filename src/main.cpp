#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include "app.h"
#include "ui.h"

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

App g_app;
Ui g_ui;
static bool g_uiReady = false;

static void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (FAILED(hr)) {
        /* 旧版 Win10（1809-1909）的属性值为 19 */
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            float scale = (float)GetDpiForWindow(hwnd) / 96.0f;
            mmi->ptMinTrackSize.x = (LONG)(800 * scale);
            mmi->ptMinTrackSize.y = (LONG)(560 * scale);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;   /* 由 D2D 每帧重绘，避免闪烁 */

        case WM_SIZE:
            if (g_uiReady) {
                g_ui.Resize(LOWORD(lp), HIWORD(lp));
                /* 拖动缩放过程中立即按新尺寸重绘，避免画面被拉伸 */
                g_ui.Render(g_app);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (g_uiReady) {
                if (g_app.dialog == Dialog::Log) {
                    g_ui.ScrollLog((float)GET_WHEEL_DELTA_WPARAM(wp));
                } else if (g_app.dialog == Dialog::None) {
                    g_ui.ScrollCards((float)GET_WHEEL_DELTA_WPARAM(wp));
                }
            }
            return 0;

        case WM_MOUSEMOVE:
            if (g_uiReady) {
                g_ui.OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            return 0;

        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            if (g_uiReady) g_ui.OnMouseDown();
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            if (g_uiReady) g_ui.OnMouseUp();
            return 0;

        case WM_CHAR:
            if (g_uiReady) g_ui.OnChar((wchar_t)wp);
            return 0;

        case WM_KEYDOWN:
            if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) && g_app.dialog == Dialog::Log) {
                if (g_uiReady) g_ui.CopyLog(g_app.logText);
            } else if (wp == VK_ESCAPE) {
                if (g_uiReady) g_ui.OnEscape();
                g_app.CloseDialog();
            } else if (g_uiReady) {
                g_ui.OnKeyDown(wp);
            }
            return 0;

        case WM_DPICHANGED: {
            RECT* pr = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                         pr->right - pr->left, pr->bottom - pr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SwFrzToolGui2Wnd";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"希沃冰点还原控制",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 680,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        return 1;
    }

    ApplyDarkTitleBar(hwnd);

    if (!g_ui.Init(hwnd)) {
        MessageBoxW(hwnd, L"初始化 Direct2D 渲染失败。", L"错误", MB_ICONERROR);
        return 1;
    }
    g_uiReady = true;

    if (!g_app.Initialize()) {
        /* 驱动未安装/加载失败也继续进入主界面，显示“驱动未安装” */
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_app.Shutdown();
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsIconic(hwnd)) {
            g_ui.Render(g_app);
        }
    }
}
