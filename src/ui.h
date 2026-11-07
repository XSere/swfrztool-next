#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

#include "app.h"
#include "theme.h"

using Microsoft::WRL::ComPtr;

class Ui {
public:
    bool Init(HWND hwnd);
    void Resize(UINT wPx, UINT hPx);
    void ScrollLog(float delta);
    void ScrollCards(float delta);
    void CopyLog(const std::string& utf8);
    void Render(App& app);

    void OnMouseMove(int xPx, int yPx);
    void OnMouseDown();
    void OnMouseUp();
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM key);
    void OnEscape();

private:
    struct HotRegion {
        D2D1_RECT_F rect;
        std::function<void()> action;
    };

    enum class Btn {
        Primary,
        Accent,
        Secondary,
        Danger,
        Ghost
    };

    HWND m_hwnd = nullptr;

    ComPtr<ID3D11Device> m_d3d;
    ComPtr<ID3D11DeviceContext> m_d3dCtx;
    ComPtr<IDXGISwapChain1> m_swap;
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_dc;
    ComPtr<ID2D1Bitmap1> m_target;
    ComPtr<ID2D1LinearGradientBrush> m_bgBrush;

    /* 日志窗口滚动与提示 */
    float m_logScroll = 0.0f;
    /* 卷卡片区域滚动 */
    float m_cardsScroll = 0.0f;
    float m_cardsScrollMax = 0.0f;
    D2D1_RECT_F m_cardsView{};
    std::wstring m_toastText;
    double m_toastStart = -1.0;
    unsigned long long m_lastToastSeq = 0;
    ComPtr<IDWriteFactory> m_dwrite;

    ComPtr<IDWriteTextFormat> m_fBigC;    // 30 大号居中（盘符）
    ComPtr<IDWriteTextFormat> m_fTitle;   // 22 标题
    ComPtr<IDWriteTextFormat> m_fTitleC;  // 22 标题居中
    ComPtr<IDWriteTextFormat> m_fBody;    // 14 正文
    ComPtr<IDWriteTextFormat> m_fBodyC;   // 14 正文居中
    ComPtr<IDWriteTextFormat> m_fSmall;   // 12 小字
    ComPtr<IDWriteTextFormat> m_fSmallC;  // 12 小字居中

    float m_dpi = 96.0f;
    float m_scale = 1.0f;
    UINT m_wPx = 0;
    UINT m_hPx = 0;
    float m_w = 0.0f;   // DIP
    float m_h = 0.0f;   // DIP

    POINT m_mouse{};    // DIP
    bool m_lDown = false;

    std::vector<HotRegion> m_hot;
    std::unordered_map<std::string, ui::Anim> m_hovers;
    std::unordered_map<std::string, ui::Anim> m_cardSelAnims;
    ui::Anim m_dialogAnim;
    double m_appearStart = 0.0;
    Dialog m_prevDialog = Dialog::None;
    std::wstring* m_activeInput = nullptr;
    bool m_activeNumeric = false;
    size_t m_caret = 0;

    double m_time = 0.0;
    double m_qpcFreq = 1.0;
    ui::Theme m_theme;

    bool CreateDeviceResources();
    bool CreateTextFormats();
    double Now();
    float Hover(const std::string& id, const D2D1_RECT_F& r);
    bool PtIn(const D2D1_RECT_F& r) const;

    void DrawBackground();
    void DrawToast();
    void DrawMain(App& app);
    void DrawVolumeCard(App& app, int idx, const D2D1_RECT_F& r);
    void DrawModal(App& app);
    void DrawMjFuncDialog(App& app, const D2D1_RECT_F& panel);
    void DrawFilterDialog(App& app, const D2D1_RECT_F& panel);
    void DrawWhiteListDialog(App& app, const D2D1_RECT_F& panel);
    void DrawLogDialog(App& app, const D2D1_RECT_F& panel);

    void Button(const std::string& id, const D2D1_RECT_F& r, const std::wstring& label, Btn kind, const std::function<void()>& action, bool enabled = true);
    void TextBox(const std::string& id, const D2D1_RECT_F& r, std::wstring* value, bool numeric, const std::wstring& hint, bool editable = true);
    void InsertInputText(const std::wstring& text);
    void PlaceCaretAt(const D2D1_RECT_F& r, const std::wstring& text);
    std::wstring GetClipboardText();
    void SetClipboardText(const std::wstring& text);
    void Toggle(const std::string& id, const D2D1_RECT_F& r, bool* value);
    void Pill(const D2D1_RECT_F& r, const std::wstring& text, const D2D1_COLOR_F& color);
};
