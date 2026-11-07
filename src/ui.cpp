#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui.h"
#include <combaseapi.h>
#include <commdlg.h>
#include <shlobj.h>

/* winuser.h 的 DrawText 宏会破坏 ID2D1RenderTarget::DrawText */
#undef DrawText

#include <algorithm>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "shell32")

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static ComPtr<ID2D1SolidColorBrush> Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c) {
    ComPtr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(c, b.GetAddressOf());
    return b;
}

static void Text(IDWriteFactory* dw, ID2D1DeviceContext* dc, const std::wstring& s,
                 IDWriteTextFormat* fmt, const D2D1_RECT_F& r, ID2D1Brush* b) {
    if (s.empty()) return;
    const float maxW = std::max(1.0f, r.right - r.left);
    const float maxH = std::max(1.0f, r.bottom - r.top);

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, maxW, maxH, layout.GetAddressOf());
    if (FAILED(hr)) return;

    /* 文本格式已设置段落垂直居中，直接按 rect 左上角绘制即可；
       不要再手动加 (maxH - height)/2，否则文字会偏下。 */
    dc->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void FillRR(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float rad, ID2D1Brush* b) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, rad, rad);
    dc->FillRoundedRectangle(rr, b);
}

static void StrokeRR(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float rad, ID2D1Brush* b, float w) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, rad, rad);
    dc->DrawRoundedRectangle(rr, b, w);
}

static void BrowseOpenFile(HWND hwnd, std::wstring& out) {
    wchar_t buf[1024] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"所有文件\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) out = buf;
}

static void BrowseFolder(HWND hwnd, std::wstring& out) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t buf[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, buf)) out = buf;
        ILFree(pidl);
    }
}

static D2D1_COLOR_F Darken(const D2D1_COLOR_F& c, float f) {
    return D2D1::ColorF(c.r * f, c.g * f, c.b * f, c.a);
}

static D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) {
    return D2D1::ColorF(c.r, c.g, c.b, a);
}

static std::wstring FormatBytes(DWORD64 bytes) {
    wchar_t units[][4] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    wchar_t buf[64] = { 0 };
    swprintf_s(buf, L"%.2f %s", v, units[u]);
    return buf;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/* ------------------------------------------------------------------ */
/* 初始化 / 资源                                                      */
/* ------------------------------------------------------------------ */

bool Ui::Init(HWND hwnd) {
    m_hwnd = hwnd;
    m_theme = ui::MakeTheme();

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m_qpcFreq = (double)f.QuadPart;
    m_time = Now();
    m_appearStart = m_time;

    if (!CreateDeviceResources()) return false;
    if (!CreateTextFormats()) return false;

    RECT rc;
    GetClientRect(hwnd, &rc);
    Resize((UINT)rc.right, (UINT)rc.bottom);
    return true;
}

double Ui::Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / m_qpcFreq;
}

bool Ui::CreateDeviceResources() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, m_d3d.GetAddressOf(), &fl, m_d3dCtx.GetAddressOf());
    if (FAILED(hr)) {
        /* 无硬件加速时退回 WARP，兼容虚拟机/低端环境 */
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, m_d3d.GetAddressOf(), &fl, m_d3dCtx.GetAddressOf());
    }
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_d3d.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    D2D1_FACTORY_OPTIONS opts = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                           &opts, (void**)m_d2dFactory.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), m_d2dDevice.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_dc.GetAddressOf());
    if (FAILED(hr)) return false;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) {
        rc.right = 1;
        rc.bottom = 1;
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = (UINT)rc.right;
    scd.Height = (UINT)rc.bottom;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(m_d3d.Get(), m_hwnd, &scd, nullptr, nullptr, m_swap.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             (IUnknown**)m_dwrite.GetAddressOf());
    if (FAILED(hr)) return false;

    return true;
}

bool Ui::CreateTextFormats() {
    auto mk = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                  ComPtr<IDWriteTextFormat>& out) -> bool {
        HRESULT hr = m_dwrite->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", out.GetAddressOf());
        if (FAILED(hr)) return false;
        out->SetTextAlignment(align);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    return mk(30, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, m_fBigC)
        && mk(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, m_fTitle)
        && mk(22, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, m_fTitleC)
        && mk(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, m_fBody)
        && mk(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, m_fBodyC)
        && mk(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, m_fSmall)
        && mk(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, m_fSmallC);
}

void Ui::Resize(UINT wPx, UINT hPx) {
    if (wPx == 0 || hPx == 0) return;
    m_wPx = wPx;
    m_hPx = hPx;

    m_dpi = (float)GetDpiForWindow(m_hwnd);
    m_scale = m_dpi / 96.0f;
    m_w = wPx / m_scale;
    m_h = hPx / m_scale;

    if (!m_swap || !m_dc) return;

    m_dc->SetTarget(nullptr);
    m_target.Reset();

    HRESULT hr = m_swap->ResizeBuffers(0, wPx, hPx, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return;

    m_dc->SetDpi(m_dpi, m_dpi);

    /* 背景渐变刷（DIP 坐标） */
    D2D1_GRADIENT_STOP stops[2] = { {0.0f, m_theme.bgTop}, {1.0f, m_theme.bgBottom} };
    ComPtr<ID2D1GradientStopCollection> coll;
    if (SUCCEEDED(m_dc->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.GetAddressOf()))) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gp =
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, m_h));
        D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties();
        m_dc->CreateLinearGradientBrush(&gp, &bp, coll.Get(), m_bgBrush.GetAddressOf());
    }

    ComPtr<IDXGISurface> surface;
    hr = m_swap->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) return;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        m_dpi, m_dpi);

    hr = m_dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, m_target.GetAddressOf());
    if (FAILED(hr)) return;
    m_dc->SetTarget(m_target.Get());
}

/* ------------------------------------------------------------------ */
/* 输入                                                               */
/* ------------------------------------------------------------------ */

void Ui::OnMouseMove(int xPx, int yPx) {
    m_mouse.x = (LONG)(xPx / m_scale);
    m_mouse.y = (LONG)(yPx / m_scale);
}

void Ui::OnMouseDown() {
    m_lDown = true;
}

void Ui::OnMouseUp() {
    m_lDown = false;
    POINT p = m_mouse;
    for (auto it = m_hot.rbegin(); it != m_hot.rend(); ++it) {
        if (p.x >= it->rect.left && p.x <= it->rect.right &&
            p.y >= it->rect.top && p.y <= it->rect.bottom) {
            if (it->action) it->action();
            break;
        }
    }
}

void Ui::InsertInputText(const std::wstring& text) {
    if (!m_activeInput) return;

    std::wstring filtered;
    filtered.reserve(text.size());
    for (wchar_t c : text) {
        if (m_activeNumeric) {
            if (c >= L'0' && c <= L'9') filtered.push_back(c);
        } else if (c >= 0x20) {
            filtered.push_back(c);
        }
    }
    if (filtered.empty()) return;

    size_t at = m_caret > m_activeInput->size() ? m_activeInput->size() : m_caret;
    size_t limit = m_activeNumeric ? 18 : 260;
    if (m_activeInput->size() >= limit) return;
    size_t room = (m_activeInput->size() + filtered.size() > limit)
                      ? (limit - m_activeInput->size())
                      : filtered.size();
    if (room == 0) return;
    filtered.resize(room);

    m_activeInput->insert(at, filtered);
    m_caret = at + filtered.size();
}

void Ui::PlaceCaretAt(const D2D1_RECT_F& r, const std::wstring& text) {
    if (text.empty()) {
        m_caret = 0;
        return;
    }

    ComPtr<IDWriteTextLayout> layout;
    float maxW = r.right - r.left - 20.0f;
    float maxH = r.bottom - r.top;
    if (FAILED(m_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), m_fBody.Get(),
                                          maxW, maxH, layout.GetAddressOf()))) {
        m_caret = text.size();
        return;
    }

    float x = m_mouse.x - (r.left + 10.0f);
    if (x < 0.0f) x = 0.0f;

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    layout->HitTestPoint(x, 0.0f, &trailing, &inside, &hit);

    m_caret = hit.textPosition;
    if (trailing) m_caret += hit.length;
    if (m_caret > text.size()) m_caret = text.size();
}

std::wstring Ui::GetClipboardText() {
    std::wstring w;
    if (!OpenClipboard(m_hwnd)) return w;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            w = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return w;
}

void Ui::SetClipboardText(const std::wstring& w) {
    if (!OpenClipboard(m_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, w.c_str(), bytes);
            GlobalUnlock(h);
        }
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

void Ui::OnChar(wchar_t ch) {
    if (!m_activeInput) return;
    if (ch >= 0x20) {
        InsertInputText(std::wstring(1, ch));
    }
}

void Ui::OnKeyDown(WPARAM key) {
    if (!m_activeInput) return;

    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl) {
        if (key == 'C') { SetClipboardText(*m_activeInput); return; }
        if (key == 'X') { SetClipboardText(*m_activeInput); m_activeInput->clear(); m_caret = 0; return; }
        if (key == 'V') { InsertInputText(GetClipboardText()); return; }
        return;
    }

    switch (key) {
        case VK_BACK:
            if (m_caret > 0 && !m_activeInput->empty()) {
                m_activeInput->erase(m_caret - 1, 1);
                m_caret--;
            }
            break;
        case VK_DELETE:
            if (m_caret < m_activeInput->size()) {
                m_activeInput->erase(m_caret, 1);
            }
            break;
        case VK_LEFT:
            if (m_caret > 0) m_caret--;
            break;
        case VK_RIGHT:
            if (m_caret < m_activeInput->size()) m_caret++;
            break;
        case VK_HOME:
            m_caret = 0;
            break;
        case VK_END:
            m_caret = m_activeInput->size();
            break;
        default:
            break;
    }
}

void Ui::OnEscape() {
    m_activeInput = nullptr;
    m_caret = 0;
    m_activeNumeric = false;
}

bool Ui::PtIn(const D2D1_RECT_F& r) const {
    return m_mouse.x >= r.left && m_mouse.x <= r.right &&
           m_mouse.y >= r.top && m_mouse.y <= r.bottom;
}

float Ui::Hover(const std::string& id, const D2D1_RECT_F& r) {
    bool over = PtIn(r);
    auto& a = m_hovers[id];
    a.To(over ? 1.0f : 0.0f, m_time);
    return a.Eval(m_time);
}

/* ------------------------------------------------------------------ */
/* 渲染                                                               */
/* ------------------------------------------------------------------ */

void Ui::Render(App& app) {
    m_time = Now();
    m_hot.clear();

    if (m_w <= 0.0f || m_h <= 0.0f) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        Resize((UINT)rc.right, (UINT)rc.bottom);
    }

    if (app.dialog != m_prevDialog) {
        m_dialogAnim.from = 0.0f;
        m_dialogAnim.to = 0.0f;
        m_dialogAnim.current = 0.0f;
        m_dialogAnim.start = m_time;
        m_prevDialog = app.dialog;
        m_activeInput = nullptr;
        m_activeNumeric = false;
        m_caret = 0;
    }
    if (app.dialog != Dialog::None) {
        m_dialogAnim.To(1.0f, m_time);
    }

    if (app.toastSeq != m_lastToastSeq) {
        m_lastToastSeq = app.toastSeq;
        m_toastText = app.toast;
        m_toastStart = m_time;
    }

    m_dc->BeginDraw();
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dc->Clear(&m_theme.bgTop);

    DrawBackground();
    DrawMain(app);
    if (app.dialog != Dialog::None) {
        DrawModal(app);
    }
    DrawToast();

    HRESULT hr = m_dc->EndDraw();
    if (SUCCEEDED(hr)) {
        m_swap->Present(1, 0);
    } else if (hr == D2DERR_RECREATE_TARGET) {
        Resize(m_wPx, m_hPx);
    }
}

void Ui::ScrollLog(float delta) {
    m_logScroll -= delta / 120.0f * 60.0f;
    if (m_logScroll < 0.0f) m_logScroll = 0.0f;
}

void Ui::ScrollCards(float delta) {
    m_cardsScroll -= delta / 120.0f * 60.0f;
    if (m_cardsScroll < 0.0f) m_cardsScroll = 0.0f;
    if (m_cardsScroll > m_cardsScrollMax) m_cardsScroll = m_cardsScrollMax;
}

void Ui::CopyLog(const std::string& utf8) {
    std::wstring w = Utf8ToWide(utf8);
    if (w.empty()) w = L"(暂无内核日志)";
    SetClipboardText(w);
}

void Ui::DrawBackground() {
    if (m_bgBrush) {
        m_dc->FillRectangle(D2D1::RectF(0, 0, m_w, m_h), m_bgBrush.Get());
    } else {
        auto top = Brush(m_dc.Get(), m_theme.bgTop);
        m_dc->FillRectangle(D2D1::RectF(0, 0, m_w, m_h), top.Get());
    }

    /* 顶部轻微的主题色光晕 */
    auto glow = Brush(m_dc.Get(), D2D1::ColorF(0x3B / 255.0f, 0x82 / 255.0f, 0xF6 / 255.0f, 0.045f));
    m_dc->FillRectangle(D2D1::RectF(0, 0, m_w, 150), glow.Get());
}

void Ui::DrawToast() {
    if (m_toastText.empty() || m_toastStart < 0.0) return;
    double t = m_time - m_toastStart;
    const double dur = 3.0;
    if (t < 0.0 || t > dur) {
        m_toastText.clear();
        return;
    }

    double inA = t / 0.18;
    if (inA > 1.0) inA = 1.0;
    double outA = (dur - t) / 0.35;
    if (outA > 1.0) outA = 1.0;
    double aD = inA < outA ? inA : outA;
    if (aD < 0.0) aD = 0.0;
    float ease = (float)(aD * aD * (3.0 - 2.0 * aD));
    float slide = (1.0f - ease) * -44.0f;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwrite->CreateTextLayout(m_toastText.c_str(), (UINT32)m_toastText.size(),
                                          m_fBodyC.Get(), 1200.0f, 80.0f, layout.GetAddressOf()))) {
        return;
    }
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    float w = tm.width + 48.0f;
    if (w < 220.0f) w = 220.0f;
    if (w > m_w - 32.0f) w = m_w - 32.0f;
    float h = 44.0f;
    D2D1_RECT_F r = D2D1::RectF((m_w - w) / 2.0f, 18.0f + slide,
                                (m_w + w) / 2.0f, 18.0f + slide + h);

    auto sh = Brush(m_dc.Get(), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f * ease));
    FillRR(m_dc.Get(), D2D1::RectF(r.left + 2.0f, r.top + 6.0f, r.right + 2.0f, r.bottom + 6.0f), h / 2.0f, sh.Get());

    auto bg = Brush(m_dc.Get(), D2D1::ColorF(0.11f, 0.13f, 0.17f, 0.97f * ease));
    FillRR(m_dc.Get(), r, h / 2.0f, bg.Get());

    auto bd = Brush(m_dc.Get(), WithAlpha(m_theme.accent, 0.85f * ease));
    StrokeRR(m_dc.Get(), r, h / 2.0f, bd.Get(), 1.0f);

    auto dotB = Brush(m_dc.Get(), D2D1::ColorF(m_theme.accent.r, m_theme.accent.g, m_theme.accent.b, ease));
    D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(r.left + 18.0f, (r.top + r.bottom) / 2.0f), 4.0f, 4.0f);
    m_dc->FillEllipse(dot, dotB.Get());

    auto tb = Brush(m_dc.Get(), D2D1::ColorF(0.95f, 0.97f, 1.0f, ease));
    Text(m_dwrite.Get(), m_dc.Get(), m_toastText, m_fBodyC.Get(),
         D2D1::RectF(r.left + 34.0f, r.top, r.right - 14.0f, r.bottom), tb.Get());
}

void Ui::DrawMain(App& app) {
    App* ap = &app;
    const float pad = 24.0f;
    const float tAppear = (float)(m_time - m_appearStart);

    /* 头部：标题 + 刷新/日志（状态提示统一走顶部灵动岛） */
    auto titleB = Brush(m_dc.Get(), m_theme.text);
    Text(m_dwrite.Get(), m_dc.Get(), L"希沃冰点还原控制", m_fTitle.Get(),
         D2D1::RectF(pad, pad - 4, m_w - pad - 240, pad + 28), titleB.Get());

    const float bw = 110.0f, bh = 36.0f;
    Button("btn_log", D2D1::RectF(m_w - pad - bw * 2 - 12, pad, m_w - pad - bw - 12, pad + bh),
           L"内核日志", Btn::Secondary, [ap]() { ap->OpenDialog(Dialog::Log); });
    Button("btn_refresh", D2D1::RectF(m_w - pad - bw, pad, m_w - pad, pad + bh),
           L"刷新", Btn::Primary, [ap]() { ap->Refresh(); });

    /* 操作栏（对选中卷生效） */
    const float barY = pad + 60.0f;
    const float barH = 40.0f;
    const float gap = 8.0f;
    const int nBtn = 5;
    float total = m_w - pad * 2;
    float bw2 = (total - gap * (nBtn - 1)) / nBtn;

    int sel = app.selected;
    bool has = app.HasVolume(sel);

    /* 按钮顺序：1 冰点配置 2 配置伪装 3 白名单管理 4 保护状态 5 MJFUNC */

    float x = pad;
    std::wstring cfgLabel = has ? L"冰点配置: 保护" : L"冰点配置";
    if (has && app.ConfigProtect(sel)) cfgLabel = L"冰点配置: 取消保护";
    Button("act_cfg", D2D1::RectF(x, barY, x + bw2, barY + barH), cfgLabel, Btn::Primary,
           [ap, has, sel]() { if (has) ap->SetConfigProtect(sel, !ap->ConfigProtect(sel)); else ap->status = L"请先选择一个卷"; },
           true /* 纯 R3 配置读写，驱动不可用时仍可用 */);

    x += bw2 + gap;
    std::wstring redLabel = has ? L"配置伪装: 保护" : L"配置伪装";
    if (has && app.RedirectProtect(sel)) redLabel = L"配置伪装: 取消保护";
    Button("act_red", D2D1::RectF(x, barY, x + bw2, barY + barH), redLabel, Btn::Primary,
           [ap, has, sel]() { if (has) ap->SetRedirect(sel, !ap->RedirectProtect(sel)); else ap->status = L"请先选择一个卷"; },
           ap->freezeReady);

    x += bw2 + gap;
    Button("act_wl", D2D1::RectF(x, barY, x + bw2, barY + barH), L"白名单管理", Btn::Primary,
           [ap]() { ap->OpenDialog(Dialog::WhiteList); }, ap->freezeReady);

    x += bw2 + gap;
    std::wstring protLabel = L"保护状态: --";
    if (has) {
        switch (app.ProtectStatus(sel)) {
            case SWFRZ_VOL_PROTECTED:  protLabel = L"保护状态: 保护"; break;
            case SWFRZ_VOL_UNPROTECTED: protLabel = L"保护状态: 未保护"; break;
            case SWFRZ_VOL_BYPASS:      protLabel = L"保护状态: 绕过"; break;
            default:                    protLabel = L"保护状态: 未知"; break;
        }
    }
    auto protAct = [ap, has, sel]() { if (has) ap->ToggleProtection(sel); else ap->status = L"请先选择一个卷"; };
    Button("act_prot", D2D1::RectF(x, barY, x + bw2, barY + barH), protLabel, Btn::Secondary, protAct, ap->freezeReady);

    x += bw2 + gap;
    Button("act_mj", D2D1::RectF(x, barY, x + bw2, barY + barH), L"MJFUNC", Btn::Secondary,
           [ap]() { ap->OpenDialog(Dialog::MjFunc); }, ap->freezeReady);

    /* 卷卡片网格（可滚动） */
    const float cardsTop = barY + barH + 34.0f;
    float avail = m_w - pad * 2;
    int cols = std::max(1, std::min(3, (int)((avail + gap) / (340.0f + gap))));
    float cardW = (avail - gap * (cols - 1)) / cols;
    const float cardH = 150.0f;

    int count = 0;
    for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
        if (app.HasVolume(i)) count++;
    }

    int rows = (count + cols - 1) / cols;
    if (rows < 1) rows = 1;
    float viewH = m_h - cardsTop - 12.0f;
    if (viewH < 120.0f) viewH = 120.0f;
    float contentH = rows * cardH + (rows > 0 ? (rows - 1) * gap : 0.0f);
    m_cardsScrollMax = contentH > viewH ? contentH - viewH : 0.0f;
    if (m_cardsScroll > m_cardsScrollMax) m_cardsScroll = m_cardsScrollMax;
    if (m_cardsScroll < 0.0f) m_cardsScroll = 0.0f;
    m_cardsView = D2D1::RectF(pad - 10.0f, cardsTop - 8.0f, m_w - pad + 10.0f, m_h - 12.0f);

    m_dc->PushAxisAlignedClip(m_cardsView, D2D1_ANTIALIAS_MODE_ALIASED);

    if (count == 0) {
        auto msgB = Brush(m_dc.Get(), m_theme.textMute);
        std::wstring msg = app.status;
        if (msg == L"就绪" || msg == L"刷新成功") {
            msg = app.ready ? L"未检测到固定磁盘" : L"驱动未安装或加载失败";
        }
        Text(m_dwrite.Get(), m_dc.Get(), msg, m_fBodyC.Get(),
             D2D1::RectF(pad, cardsTop, m_w - pad, cardsTop + 90), msgB.Get());
    }

    int col = 0, row = 0;
    int drawn = 0;
    for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
        if (!app.HasVolume(i)) continue;
        float baseY = cardsTop - m_cardsScroll + row * (cardH + gap);
        D2D1_RECT_F r = D2D1::RectF(
            pad + col * (cardW + gap),
            baseY,
            pad + col * (cardW + gap) + cardW,
            baseY + cardH);

        /* 卡片入场：逐个上浮 + 淡入（避免影响命中区域，动画结束后完全对齐） */
        float local = (tAppear - drawn * 0.05f) / 0.45f;
        local = local < 0.0f ? 0.0f : (local > 1.0f ? 1.0f : local);
        if (local > 0.0f) {
            D2D1_RECT_F drawR = r;
            const float offY = (1.0f - local) * 16.0f;
            drawR.top += offY;
            drawR.bottom += offY;
            DrawVolumeCard(app, i, drawR);
        }

        drawn++;
        col++;
        if (col >= cols) {
            col = 0;
            row++;
        }
    }

    m_dc->PopAxisAlignedClip();

    /* 卡片滚动条 */
    if (m_cardsScrollMax > 0.0f) {
        float trackTop = m_cardsView.top;
        float trackBottom = m_cardsView.bottom;
        float trackH = trackBottom - trackTop;
        float thumbH = std::max(30.0f, trackH * (viewH / (viewH + m_cardsScrollMax)));
        float thumbY = trackTop + (trackH - thumbH) * (m_cardsScroll / m_cardsScrollMax);
        D2D1_RECT_F track = D2D1::RectF(m_w - 14.0f, trackTop, m_w - 8.0f, trackBottom);
        auto trB = Brush(m_dc.Get(), WithAlpha(m_theme.textMute, 0.22f));
        FillRR(m_dc.Get(), track, 3.0f, trB.Get());
        D2D1_RECT_F thumb = D2D1::RectF(m_w - 14.0f, thumbY, m_w - 8.0f, thumbY + thumbH);
        auto thB = Brush(m_dc.Get(), WithAlpha(m_theme.textSub, 0.65f));
        FillRR(m_dc.Get(), thumb, 3.0f, thB.Get());
    }
}

void Ui::DrawVolumeCard(App& app, int idx, const D2D1_RECT_F& r) {
    const bool sel = (app.selected == idx);
    D2D1_RECT_F hitR = r;
    if (hitR.top < m_cardsView.top) hitR.top = m_cardsView.top;
    if (hitR.bottom > m_cardsView.bottom) hitR.bottom = m_cardsView.bottom;
    const float hov = Hover("card" + std::to_string(idx), hitR);
    const bool pressed = m_lDown && (hov > 0.5f);

    auto& selAnim = m_cardSelAnims["cardsel" + std::to_string(idx)];
    selAnim.To(sel ? 1.0f : 0.0f, m_time);
    float se = selAnim.Eval(m_time);

    /* 选中卡片的外发光 */
    if (se > 0.02f) {
        auto glowB = Brush(m_dc.Get(), WithAlpha(m_theme.accent, 0.22f * se));
        D2D1_RECT_F gr = D2D1::RectF(r.left - 3.0f, r.top - 3.0f, r.right + 3.0f, r.bottom + 3.0f);
        StrokeRR(m_dc.Get(), gr, m_theme.cardRadius + 3.0f, glowB.Get(), 6.0f);
    }

    /* 阴影 */
    if (hov > 0.02f || se > 0.02f) {
        float lift = 3.0f + 5.0f * hov + 2.0f * se;
        auto sh = Brush(m_dc.Get(), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f + 0.12f * hov + 0.10f * se));
        FillRR(m_dc.Get(),
               D2D1::RectF(r.left + 3, r.top + lift + 4, r.right + 3, r.bottom + lift + 4),
               m_theme.cardRadius, sh.Get());
    }

    /* 卡片主体 */
    D2D1_COLOR_F bg = ui::Lerp(m_theme.card, D2D1::ColorF(0x24 / 255.0f, 0x2A / 255.0f, 0x34 / 255.0f, 1.0f), hov * 0.6f);
    if (pressed) bg = Darken(bg, 0.92f);
    auto bgB = Brush(m_dc.Get(), bg);
    FillRR(m_dc.Get(), r, m_theme.cardRadius, bgB.Get());

    D2D1_COLOR_F border = ui::Lerp(m_theme.cardBorder, m_theme.accent, std::max(hov * 0.35f, se));
    auto bdB = Brush(m_dc.Get(), border);
    StrokeRR(m_dc.Get(), r, m_theme.cardRadius, bdB.Get(), 1.0f + 0.6f * se);

    const float pad = 16.0f;
    const float sq = 56.0f;

    /* 盘符方块 */
    D2D1_RECT_F sqr = D2D1::RectF(r.left + pad, r.top + (r.bottom - r.top) / 2 - sq / 2,
                                  r.left + pad + sq, r.top + (r.bottom - r.top) / 2 + sq / 2);
    auto sqBg = Brush(m_dc.Get(), WithAlpha(m_theme.accent, 0.16f));
    FillRR(m_dc.Get(), sqr, 14.0f, sqBg.Get());
    auto sqBd = Brush(m_dc.Get(), WithAlpha(m_theme.accent, 0.40f));
    StrokeRR(m_dc.Get(), sqr, 14.0f, sqBd.Get(), 1.0f);

    std::wstring letter(1, app.DriveLetter(idx));
    auto letterB = Brush(m_dc.Get(), m_theme.accent);
    Text(m_dwrite.Get(), m_dc.Get(), letter, m_fBigC.Get(), sqr, letterB.Get());

    /* 名称 + 大小 */
    float tx = r.left + pad + sq + 16.0f;
    std::wstring name = L"卷 " + letter + L":";
    auto nameB = Brush(m_dc.Get(), m_theme.text);
    Text(m_dwrite.Get(), m_dc.Get(), name, m_fTitle.Get(), D2D1::RectF(tx, r.top + pad + 2, r.right - 90, r.top + pad + 30), nameB.Get());

    auto sizeB = Brush(m_dc.Get(), m_theme.textSub);
    std::wstring size = FormatBytes(app.info.volumeInfoList[idx]->volumeTotalBytes);
    Text(m_dwrite.Get(), m_dc.Get(), size, m_fSmall.Get(), D2D1::RectF(tx, r.top + pad + 32, r.right - pad, r.top + pad + 50), sizeB.Get());

    /* 保护状态徽标 */
    const wchar_t* st = L"未知";
    D2D1_COLOR_F c = m_theme.unknown;
    switch (app.ProtectStatus(idx)) {
        case SWFRZ_VOL_PROTECTED: st = L"受保护"; c = m_theme.good; break;
        case SWFRZ_VOL_UNPROTECTED: st = L"未保护"; c = m_theme.bad; break;
        case SWFRZ_VOL_BYPASS: st = L"绕过保护"; c = m_theme.warn; break;
        default: break;
    }
    Pill(D2D1::RectF(r.right - pad - 76, r.top + pad, r.right - pad, r.top + pad + 24), st, c);

    /* 配置状态 */
    auto muteB = Brush(m_dc.Get(), m_theme.textMute);
    float ly = r.bottom - pad - 38;
    Text(m_dwrite.Get(), m_dc.Get(), L"真实配置:", m_fSmall.Get(), D2D1::RectF(tx, ly, tx + 62, ly + 18), muteB.Get());

    const wchar_t* cfgTxt;
    D2D1_COLOR_F cfgC;
    if (app.info.config == (DWORD64)-1) { cfgTxt = L"无效"; cfgC = m_theme.textMute; }
    else if (app.ConfigProtect(idx)) { cfgTxt = L"保护"; cfgC = m_theme.good; }
    else { cfgTxt = L"不保护"; cfgC = m_theme.bad; }
    auto cfgB = Brush(m_dc.Get(), cfgC);
    Text(m_dwrite.Get(), m_dc.Get(), cfgTxt, m_fSmall.Get(), D2D1::RectF(tx + 66, ly, tx + 110, ly + 18), cfgB.Get());

    ly += 18;
    Text(m_dwrite.Get(), m_dc.Get(), L"伪装配置:", m_fSmall.Get(), D2D1::RectF(tx, ly, tx + 62, ly + 18), muteB.Get());

    const wchar_t* redTxt;
    D2D1_COLOR_F redC;
    if (app.info.redirect == (DWORD64)-1) { redTxt = L"无效"; redC = m_theme.textMute; }
    else if (app.RedirectProtect(idx)) { redTxt = L"保护"; redC = m_theme.warn; }
    else { redTxt = L"不保护"; redC = m_theme.bad; }
    auto redB = Brush(m_dc.Get(), redC);
    Text(m_dwrite.Get(), m_dc.Get(), redTxt, m_fSmall.Get(), D2D1::RectF(tx + 66, ly, tx + 110, ly + 18), redB.Get());

    /* 点击选中 */
    App* ap = &app;
    if (r.bottom > m_cardsView.top && r.top < m_cardsView.bottom &&
        r.right > m_cardsView.left && r.left < m_cardsView.right) {
        m_hot.push_back({ r, [ap, idx]() { ap->selected = idx; } });
    }
}

void Ui::DrawModal(App& app) {
    m_hot.clear();   /* 打开对话框时屏蔽底层热区 */

    float a = m_dialogAnim.Eval(m_time);

    auto ov = Brush(m_dc.Get(), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f * a));
    m_dc->FillRectangle(D2D1::RectF(0, 0, m_w, m_h), ov.Get());

    float pw = 460.0f;
    float ph = 220.0f;
    switch (app.dialog) {
        case Dialog::MjFunc: ph = 230.0f; break;
        case Dialog::Filter: ph = 380.0f; break;
        case Dialog::WhiteList: ph = 470.0f; break;
        case Dialog::Log: ph = 440.0f; break;
        default: break;
    }

    float sc = 0.96f + 0.04f * a;
    pw *= sc;
    ph *= sc;
    float slide = (1.0f - a) * 24.0f;
    D2D1_RECT_F panel = D2D1::RectF((m_w - pw) / 2, (m_h - ph) / 2 + slide,
                                    (m_w + pw) / 2, (m_h + ph) / 2 + slide);

    auto sh = Brush(m_dc.Get(), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f * a));
    FillRR(m_dc.Get(), D2D1::RectF(panel.left + 4, panel.top + 10, panel.right + 4, panel.bottom + 10), 16.0f, sh.Get());

    auto pb = Brush(m_dc.Get(), m_theme.card);
    FillRR(m_dc.Get(), panel, 16.0f, pb.Get());
    auto pbd = Brush(m_dc.Get(), m_theme.cardBorder);
    StrokeRR(m_dc.Get(), panel, 16.0f, pbd.Get(), 1.0f);

    switch (app.dialog) {
        case Dialog::MjFunc: DrawMjFuncDialog(app, panel); break;
        case Dialog::Filter: DrawFilterDialog(app, panel); break;
        case Dialog::WhiteList: DrawWhiteListDialog(app, panel); break;
        case Dialog::Log: DrawLogDialog(app, panel); break;
        default: break;
    }
}

void Ui::DrawMjFuncDialog(App& app, const D2D1_RECT_F& panel) {
    App* ap = &app;
    const float pad = 20.0f;
    auto titleB = Brush(m_dc.Get(), m_theme.text);
    Text(m_dwrite.Get(), m_dc.Get(), L"磁盘读写分发例程", m_fTitle.Get(),
         D2D1::RectF(panel.left + pad, panel.top + pad, panel.right - pad, panel.top + pad + 30), titleB.Get());

    float y = panel.top + 64.0f;
    D2D1_RECT_F b1 = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 42);
    y += 50;
    D2D1_RECT_F b2 = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 42);

    Button("mj_orig", b1, L"恢复原始 disk.sys 分发例程", Btn::Secondary, [ap]() {
        ap->SetMjFunc(SWFRZ_MJFUNC_ORIGINAL);
        ap->CloseDialog();
    });
    Button("mj_sw", b2, L"恢复 SWFreeze 分发例程", Btn::Secondary, [ap]() {
        ap->SetMjFunc(SWFRZ_MJFUNC_SWFREEZE);
        ap->CloseDialog();
    });

    D2D1_RECT_F cancel = D2D1::RectF(panel.left + pad, panel.bottom - 58, panel.right - pad, panel.bottom - 18);
    Button("mj_cancel", cancel, L"取消", Btn::Ghost, [ap]() { ap->CloseDialog(); });
}

void Ui::DrawFilterDialog(App& app, const D2D1_RECT_F& panel) {
    App* ap = &app;
    const float pad = 20.0f;
    auto titleB = Brush(m_dc.Get(), m_theme.text);
    Text(m_dwrite.Get(), m_dc.Get(), L"状态伪装", m_fTitle.Get(),
         D2D1::RectF(panel.left + pad, panel.top + pad, panel.right - pad, panel.top + pad + 30), titleB.Get());

    float y = panel.top + 60.0f;
    std::wstring cur = app.filterEnabled ? L"状态伪装当前：已启用" : L"状态伪装当前：已禁用";
    auto subB = Brush(m_dc.Get(), m_theme.textSub);
    Text(m_dwrite.Get(), m_dc.Get(), cur, m_fBody.Get(), D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 22), subB.Get());

    y += 32;
    D2D1_RECT_F bOn = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 40);
    y += 48;
    D2D1_RECT_F bOff = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 40);
    y += 56;

    Button("flt_on", bOn, L"启用状态伪装", app.filterEnabled ? Btn::Secondary : Btn::Primary, [ap]() {
        ap->SetFilterStatus(true);
        ap->CloseDialog();
    });
    Button("flt_off", bOff, L"禁用状态伪装", app.filterEnabled ? Btn::Primary : Btn::Secondary, [ap]() {
        ap->SetFilterStatus(false);
        ap->CloseDialog();
    });

    /* 选中卷的伪装配置 */
    int sel = app.selected;
    if (app.HasVolume(sel)) {
        std::wstring vol = L"选中卷：卷 " + std::wstring(1, app.DriveLetter(sel)) + L":";
        Text(m_dwrite.Get(), m_dc.Get(), vol, m_fBody.Get(), D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 22), subB.Get());
        y += 30;
        D2D1_RECT_F rOn = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 38);
        y += 46;
        D2D1_RECT_F rOff = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 38);
        Button("red_on", rOn, L"伪装为受保护", app.RedirectProtect(sel) ? Btn::Secondary : Btn::Primary, [ap, sel]() {
            ap->SetRedirect(sel, true);
        });
        Button("red_off", rOff, L"伪装为未保护", app.RedirectProtect(sel) ? Btn::Primary : Btn::Secondary, [ap, sel]() {
            ap->SetRedirect(sel, false);
        });
    }

    D2D1_RECT_F cancel = D2D1::RectF(panel.left + pad, panel.bottom - 58, panel.right - pad, panel.bottom - 18);
    Button("flt_cancel", cancel, L"关闭", Btn::Ghost, [ap]() { ap->CloseDialog(); });
}

void Ui::DrawWhiteListDialog(App& app, const D2D1_RECT_F& panel) {
    App* ap = &app;
    const float pad = 20.0f;

    /* 后台计算完成后由 UI 线程统一收尾，避免跨线程写 UI 状态 */
    if (ap->wlDone) {
        ap->FinishWhiteList();
        if (app.dialog != Dialog::WhiteList) return;
    }

    auto titleB = Brush(m_dc.Get(), m_theme.text);
    Text(m_dwrite.Get(), m_dc.Get(), L"白名单", m_fTitle.Get(),
         D2D1::RectF(panel.left + pad, panel.top + pad, panel.right - pad, panel.top + pad + 30), titleB.Get());

    int sel = app.selected;
    if (!app.HasVolume(sel)) {
        auto subB = Brush(m_dc.Get(), m_theme.textSub);
        Text(m_dwrite.Get(), m_dc.Get(), L"请先选择一个卷", m_fBody.Get(),
             D2D1::RectF(panel.left + pad, panel.top + 70, panel.right - pad, panel.top + 94), subB.Get());
        D2D1_RECT_F cancel = D2D1::RectF(panel.left + pad, panel.bottom - 58, panel.right - pad, panel.bottom - 18);
        Button("wl_cancel_none", cancel, L"关闭", Btn::Ghost, [ap]() { ap->CloseDialog(); });
        return;
    }

    /* 三个页签 */
    const wchar_t* tabs[] = { L"按文件写入", L"按文件夹写入", L"按扇区范围" };
    float gap = 8.0f;
    float tabW = (panel.right - panel.left - pad * 2 - gap * 2) / 3.0f;
    float tabY = panel.top + 54.0f;
    float tabH = 34.0f;
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F tr = D2D1::RectF(panel.left + pad + i * (tabW + gap), tabY,
                                     panel.left + pad + i * (tabW + gap) + tabW, tabY + tabH);
        Button("wl_tab" + std::to_string(i), tr, tabs[i],
               (ap->wlTab == i) ? Btn::Primary : Btn::Secondary,
               [ap, this, i]() { m_activeInput = nullptr; m_caret = 0; ap->wlTab = i; });
    }

    float y = tabY + tabH + 14.0f;
    auto subB = Brush(m_dc.Get(), m_theme.textSub);
    auto labelB = Brush(m_dc.Get(), m_theme.textSub);
    auto opB = Brush(m_dc.Get(), m_theme.text);

    std::wstring vol = L"目标卷：卷 " + std::wstring(1, app.DriveLetter(sel)) + L":";
    Text(m_dwrite.Get(), m_dc.Get(), vol, m_fBody.Get(),
         D2D1::RectF(panel.left + pad, y, panel.right - pad, y + 22), subB.Get());
    y += 32;

    float opY = 0.0f;

    if (ap->wlTab == 2) {
        /* 第三页：原始按起始扇区 / 数量写入 */
        Text(m_dwrite.Get(), m_dc.Get(), L"起始扇区", m_fSmall.Get(),
             D2D1::RectF(panel.left + pad, y, panel.left + pad + 80, y + 18), labelB.Get());
        D2D1_RECT_F startR = D2D1::RectF(panel.left + pad + 86, y - 4, panel.right - pad, y + 26);
        TextBox("wl_start", startR, &app.wsStart, true, L"0");

        y += 42;
        Text(m_dwrite.Get(), m_dc.Get(), L"扇区数量", m_fSmall.Get(),
             D2D1::RectF(panel.left + pad, y, panel.left + pad + 80, y + 18), labelB.Get());
        D2D1_RECT_F countR = D2D1::RectF(panel.left + pad + 86, y - 4, panel.right - pad, y + 26);
        TextBox("wl_count", countR, &app.wsCount, true, L"0");

        y += 42;
        opY = y + 6;
    } else {
        /* 第一/二页：文件/文件夹路径 */
        const wchar_t* pathLabel = (ap->wlTab == 0) ? L"文件路径" : L"文件夹路径";
        Text(m_dwrite.Get(), m_dc.Get(), pathLabel, m_fSmall.Get(),
             D2D1::RectF(panel.left + pad, y, panel.left + pad + 80, y + 18), labelB.Get());

        D2D1_RECT_F pathR = D2D1::RectF(panel.left + pad + 86, y - 4, panel.right - pad - 76, y + 26);
        std::wstring* pathPtr = (ap->wlTab == 0) ? &app.wlFilePath : &app.wlFolderPath;
        TextBox("wl_path", pathR, pathPtr, false,
                (ap->wlTab == 0) ? L"C:\\...\\file.txt" : L"C:\\...\\folder");
        D2D1_RECT_F browse = D2D1::RectF(panel.right - pad - 66, y - 4, panel.right - pad, y + 26);
        Button("wl_browse", browse, L"浏览", Btn::Secondary, [ap, this]() {
            if (ap->wlTab == 0) BrowseOpenFile(m_hwnd, ap->wlFilePath);
            else BrowseFolder(m_hwnd, ap->wlFolderPath);
        });

        y += 42;
        opY = y + 6;
    }

    Text(m_dwrite.Get(), m_dc.Get(), L"操作", m_fSmall.Get(),
         D2D1::RectF(panel.left + pad, opY, panel.left + pad + 80, opY + 18), labelB.Get());
    if (ap->wlBusy) {
        Text(m_dwrite.Get(), m_dc.Get(), app.wlValue ? L"写入白名单（置位）" : L"移出白名单（清位）", m_fBody.Get(),
             D2D1::RectF(panel.left + pad + 86 + 54, opY - 4, panel.right - pad, opY + 24), opB.Get());
    } else {
        Toggle("wl_value", D2D1::RectF(panel.left + pad + 86, opY - 2, panel.left + pad + 86 + 44, opY + 18), &app.wlValue);
        Text(m_dwrite.Get(), m_dc.Get(), app.wlValue ? L"写入白名单（置位）" : L"移出白名单（清位）", m_fBody.Get(),
             D2D1::RectF(panel.left + pad + 86 + 54, opY - 4, panel.right - pad, opY + 24), opB.Get());
    }

    /* 文件夹页的进度条 */
    if (ap->wlTab == 1 && ap->wlBusy) {
        float py = opY + 40;
        D2D1_RECT_F track = D2D1::RectF(panel.left + pad, py, panel.right - pad, py + 8);
        auto trB = Brush(m_dc.Get(), ui::Rgb(0x14, 0x17, 0x1D));
        FillRR(m_dc.Get(), track, 4.0f, trB.Get());

        int pct = ap->wlProgress.load();
        D2D1_RECT_F fill = track;
        fill.right = track.left + (track.right - track.left) * (float)pct / 100.0f;
        if (fill.right > track.right) fill.right = track.right;
        auto fillB = Brush(m_dc.Get(), m_theme.accent);
        FillRR(m_dc.Get(), fill, 4.0f, fillB.Get());

        std::wstring ptext = (pct == 0) ? L"正在扫描文件..."
                                        : (L"正在计算扇区 " + std::to_wstring(pct) + L"%");
        auto ptB = Brush(m_dc.Get(), m_theme.textSub);
        Text(m_dwrite.Get(), m_dc.Get(), ptext, m_fSmall.Get(),
             D2D1::RectF(panel.left + pad, py + 12, panel.right - pad, py + 30), ptB.Get());
    }

    D2D1_RECT_F exec = D2D1::RectF(panel.left + pad, panel.bottom - 104, panel.right - pad, panel.bottom - 64);
    if (ap->wlBusy) {
        Button("wl_exec_busy", exec, L"计算中...", Btn::Secondary, []() {});
    } else if (ap->wlTab == 0) {
        Button("wl_exec_file", exec, L"执行", Btn::Primary, [ap, this]() { m_activeInput = nullptr; ap->ApplyFileWhiteList(); });
    } else if (ap->wlTab == 1) {
        Button("wl_exec_folder", exec, L"执行", Btn::Primary, [ap, this]() { m_activeInput = nullptr; ap->StartFolderWhiteList(); });
    } else {
        Button("wl_exec", exec, L"执行", Btn::Primary, [ap, this]() { m_activeInput = nullptr; ap->ApplyWhiteList(); });
    }

    D2D1_RECT_F cancel = D2D1::RectF(panel.left + pad, panel.bottom - 58, panel.right - pad, panel.bottom - 18);
    Button("wl_cancel", cancel, L"取消", Btn::Ghost, [ap]() { ap->CloseDialog(); });
}

void Ui::DrawLogDialog(App& app, const D2D1_RECT_F& panel) {
    App* ap = &app;
    const float pad = 20.0f;

    auto titleB = Brush(m_dc.Get(), m_theme.text);
    Text(m_dwrite.Get(), m_dc.Get(), L"内核日志", m_fTitle.Get(),
         D2D1::RectF(panel.left + pad, panel.top + pad, panel.right - pad, panel.top + pad + 30), titleB.Get());

    D2D1_RECT_F view = D2D1::RectF(panel.left + pad, panel.top + 54.0f,
                                   panel.right - pad - 12.0f, panel.bottom - 78.0f);
    float viewH = view.bottom - view.top;

    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = m_dwrite->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", fmt.GetAddressOf());
    if (SUCCEEDED(hr)) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        std::wstring wlog = Utf8ToWide(app.logText);
        if (wlog.empty()) wlog = L"(暂无内核日志)";
        ComPtr<IDWriteTextLayout> layout;
        float maxW = view.right - view.left;
        float maxH = 100000.0f;
        hr = m_dwrite->CreateTextLayout(wlog.c_str(), (UINT32)wlog.size(), fmt.Get(), maxW, maxH, layout.GetAddressOf());
        if (SUCCEEDED(hr)) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            float maxScroll = tm.height > viewH ? tm.height - viewH : 0.0f;
            if (m_logScroll > maxScroll) m_logScroll = maxScroll;
            if (m_logScroll < 0.0f) m_logScroll = 0.0f;

            m_dc->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
            auto tb = Brush(m_dc.Get(), m_theme.textSub);
            m_dc->DrawTextLayout(D2D1::Point2F(view.left, view.top - m_logScroll), layout.Get(), tb.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
            m_dc->PopAxisAlignedClip();

            if (maxScroll > 0.0f) {
                float thumbH = std::max(36.0f, viewH * (viewH / (viewH + maxScroll)));
                float thumbY = view.top + (viewH - thumbH) * (m_logScroll / maxScroll);
                D2D1_RECT_F track = D2D1::RectF(view.right + 4.0f, view.top, view.right + 10.0f, view.bottom);
                auto trB = Brush(m_dc.Get(), WithAlpha(m_theme.textMute, 0.25f));
                FillRR(m_dc.Get(), track, 3.0f, trB.Get());
                D2D1_RECT_F thumb = D2D1::RectF(view.right + 4.0f, thumbY, view.right + 10.0f, thumbY + thumbH);
                auto thB = Brush(m_dc.Get(), WithAlpha(m_theme.textSub, 0.70f));
                FillRR(m_dc.Get(), thumb, 3.0f, thB.Get());
            }
        }
    }

    D2D1_RECT_F copy = D2D1::RectF(panel.left + pad, panel.bottom - 58, panel.left + pad + 110, panel.bottom - 18);
    Button("log_copy", copy, L"复制日志", Btn::Secondary, [this, ap]() { CopyLog(ap->logText); });

    D2D1_RECT_F close = D2D1::RectF(panel.right - pad - 110, panel.bottom - 58, panel.right - pad, panel.bottom - 18);
    Button("log_close", close, L"关闭", Btn::Secondary, [ap]() { ap->CloseDialog(); });
}

/* ------------------------------------------------------------------ */
/* 控件                                                               */
/* ------------------------------------------------------------------ */

void Ui::Button(const std::string& id, const D2D1_RECT_F& r, const std::wstring& label, Btn kind,
                const std::function<void()>& action, bool enabled) {
    if (!enabled) {
        auto bgB = Brush(m_dc.Get(), ui::Rgb(0x16, 0x1A, 0x20));
        FillRR(m_dc.Get(), r, 10.0f, bgB.Get());
        auto bdB = Brush(m_dc.Get(), WithAlpha(m_theme.cardBorder, 0.8f));
        StrokeRR(m_dc.Get(), r, 10.0f, bdB.Get(), 1.0f);
        auto fgB = Brush(m_dc.Get(), m_theme.textMute);
        Text(m_dwrite.Get(), m_dc.Get(), label, m_fBodyC.Get(), r, fgB.Get());
        return;
    }

    const float h = Hover(id, r);
    const bool pressed = m_lDown && (h > 0.5f);

    D2D1_COLOR_F bg, border, fg;
    switch (kind) {
        case Btn::Primary:
            bg = ui::Lerp(m_theme.accent, m_theme.accentHover, h);
            border = bg;
            fg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            break;
        case Btn::Accent:
            bg = ui::Lerp(WithAlpha(m_theme.accent, 0.16f), WithAlpha(m_theme.accent, 0.28f), h);
            border = ui::Lerp(m_theme.accent, m_theme.accentHover, h);
            fg = ui::Lerp(m_theme.accentHover, D2D1::ColorF(0.82f, 0.90f, 1.0f, 1.0f), h);
            break;
        case Btn::Danger:
            bg = ui::Lerp(ui::Rgb(0x2A, 0x1D, 0x20), ui::Rgb(0x3C, 0x23, 0x28), h);
            border = m_theme.bad;
            fg = m_theme.bad;
            break;
        case Btn::Ghost:
            bg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f * h);
            border = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
            fg = m_theme.textSub;
            break;
        case Btn::Secondary:
        default:
            bg = ui::Lerp(m_theme.card, ui::Rgb(0x26, 0x2D, 0x39), h);
            border = ui::Lerp(m_theme.cardBorder, m_theme.accent, h * 0.5f);
            fg = m_theme.text;
            break;
    }

    if (pressed) bg = Darken(bg, 0.85f);

    auto bgB = Brush(m_dc.Get(), bg);
    FillRR(m_dc.Get(), r, 10.0f, bgB.Get());

    if (kind == Btn::Primary) {
        auto shine = Brush(m_dc.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
        m_dc->FillRectangle(D2D1::RectF(r.left + 3.0f, r.top + 1.5f, r.right - 3.0f, r.top + 3.5f), shine.Get());
    }

    if (kind != Btn::Ghost) {
        auto bdB = Brush(m_dc.Get(), border);
        StrokeRR(m_dc.Get(), r, 10.0f, bdB.Get(), 1.0f);
    }

    auto fgB = Brush(m_dc.Get(), fg);
    Text(m_dwrite.Get(), m_dc.Get(), label, m_fBodyC.Get(), r, fgB.Get());

    if (action) {
        m_hot.push_back({ r, action });
    }
}

void Ui::TextBox(const std::string& id, const D2D1_RECT_F& r, std::wstring* value, bool numeric,
                 const std::wstring& hint, bool editable) {
    const bool focused = editable && (m_activeInput == value);
    const float h = Hover(id, r);

    auto bgB = Brush(m_dc.Get(), ui::Rgb(0x14, 0x17, 0x1D));
    FillRR(m_dc.Get(), r, 8.0f, bgB.Get());

    D2D1_COLOR_F border = focused ? m_theme.accent : ui::Lerp(m_theme.cardBorder, m_theme.accent, h * 0.4f);
    auto bdB = Brush(m_dc.Get(), border);
    StrokeRR(m_dc.Get(), r, 8.0f, bdB.Get(), focused ? 1.4f : 1.0f);

    std::wstring show = value->empty() ? hint : *value;
    D2D1_COLOR_F tc = value->empty() ? m_theme.textMute : m_theme.text;
    auto tb = Brush(m_dc.Get(), tc);

    ComPtr<IDWriteTextLayout> layout;
    float maxW = r.right - r.left - 20.0f;
    float maxH = r.bottom - r.top;
    if (SUCCEEDED(m_dwrite->CreateTextLayout(show.c_str(), (UINT32)show.size(), m_fBody.Get(),
                                             maxW, maxH, layout.GetAddressOf()))) {
        m_dc->DrawTextLayout(D2D1::Point2F(r.left + 10.0f, r.top), layout.Get(), tb.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (focused) {
        /* 光标：根据当前插入位置计算 x 坐标 */
        size_t caret = value->empty() ? 0 : (m_caret > value->size() ? value->size() : m_caret);
        float caretX = 0.0f;
        if (layout) {
            float py = 0.0f;
            DWRITE_HIT_TEST_METRICS hit{};
            layout->HitTestTextPosition((UINT32)caret, FALSE, &caretX, &py, &hit);
        }
        auto cb = Brush(m_dc.Get(), m_theme.accent);
        m_dc->FillRectangle(D2D1::RectF(r.left + 10.0f + caretX, r.top + 6.0f,
                                        r.left + 11.5f + caretX, r.bottom - 6.0f), cb.Get());
    }

    if (editable) {
        m_hot.push_back({ r, [this, value, numeric, r]() {
            m_activeInput = value;
            m_activeNumeric = numeric;
            if (value->empty()) {
                m_caret = 0;
            } else {
                PlaceCaretAt(r, *value);
            }
        }});
    }
}

void Ui::Toggle(const std::string& id, const D2D1_RECT_F& r, bool* value) {
    const float h = Hover(id, r);

    D2D1_COLOR_F on = *value ? m_theme.accent : ui::Rgb(0x2A, 0x31, 0x3D);
    D2D1_COLOR_F onHover = *value ? m_theme.accentHover : ui::Rgb(0x33, 0x3B, 0x49);
    D2D1_COLOR_F bg = ui::Lerp(on, onHover, h);

    D2D1_RECT_F track = D2D1::RectF(r.left, r.top, r.left + 44, r.bottom);
    float trackH = track.bottom - track.top;
    auto bgB = Brush(m_dc.Get(), bg);
    FillRR(m_dc.Get(), track, trackH / 2, bgB.Get());

    const float knob = trackH - 4;
    float kx = *value ? (track.right - knob - 2) : (track.left + 2);
    D2D1_RECT_F kr = D2D1::RectF(kx, track.top + 2, kx + knob, track.bottom - 2);
    auto kb = Brush(m_dc.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    FillRR(m_dc.Get(), kr, knob / 2, kb.Get());

    m_hot.push_back({ track, [value]() { *value = !*value; } });
}

void Ui::Pill(const D2D1_RECT_F& r, const std::wstring& text, const D2D1_COLOR_F& color) {
    float rad = (r.bottom - r.top) / 2;
    auto bgB = Brush(m_dc.Get(), WithAlpha(color, 0.16f));
    FillRR(m_dc.Get(), r, rad, bgB.Get());
    auto fgB = Brush(m_dc.Get(), color);
    D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(r.left + 10.0f, (r.top + r.bottom) / 2.0f), 3.0f, 3.0f);
    m_dc->FillEllipse(dot, fgB.Get());
    Text(m_dwrite.Get(), m_dc.Get(), text, m_fSmallC.Get(),
         D2D1::RectF(r.left + 18.0f, r.top, r.right - 6.0f, r.bottom), fgB.Get());
}
