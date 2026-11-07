#pragma once
#include <d2d1.h>
#include <cmath>

namespace ui {

// 颜色（深色主题）
struct Theme {
    D2D1_COLOR_F bgTop;
    D2D1_COLOR_F bgBottom;
    D2D1_COLOR_F card;
    D2D1_COLOR_F cardBorder;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F text;
    D2D1_COLOR_F textSub;
    D2D1_COLOR_F textMute;
    D2D1_COLOR_F good;
    D2D1_COLOR_F warn;
    D2D1_COLOR_F bad;
    D2D1_COLOR_F unknown;
    D2D1_COLOR_F overlay;
    float radius = 12.0f;
    float cardRadius = 16.0f;
};

inline D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

inline D2D1_COLOR_F Lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return D2D1::ColorF(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t);
}

inline Theme MakeTheme() {
    Theme t{};
    t.bgTop = Rgb(0x16, 0x19, 0x1F);
    t.bgBottom = Rgb(0x0C, 0x0E, 0x12);
    t.card = Rgb(0x1E, 0x23, 0x2B);
    t.cardBorder = Rgb(0x2A, 0x31, 0x3D);
    t.accent = Rgb(0x3B, 0x82, 0xF6);
    t.accentHover = Rgb(0x60, 0x9C, 0xFA);
    t.text = Rgb(0xEC, 0xEF, 0xF4);
    t.textSub = Rgb(0xA6, 0xAE, 0xBA);
    t.textMute = Rgb(0x6E, 0x77, 0x85);
    t.good = Rgb(0x22, 0xC5, 0x5E);
    t.warn = Rgb(0xF5, 0x9E, 0x0B);
    t.bad = Rgb(0xEF, 0x44, 0x44);
    t.unknown = Rgb(0x8B, 0x93, 0xA0);
    t.overlay = Rgb(0x00, 0x00, 0x00, 0.55f);
    return t;
}

// 简单缓动动画值（smoothstep）
struct Anim {
    float from = 0.0f;
    float to = 0.0f;
    float current = 0.0f;
    double start = 0.0;
    float dur = 0.18f;

    void To(float target, double now) {
        if (target != to) {
            from = current;
            to = target;
            start = now;
        }
    }

    float Eval(double now) {
        float t = float((now - start) / dur);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float e = t * t * (3.0f - 2.0f * t);
        current = from + (to - from) * e;
        if (t >= 1.0f) { from = current; to = current; }
        return current;
    }
};

} // namespace ui
