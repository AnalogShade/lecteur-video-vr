#pragma once

#include "platform.h"
#include <algorithm>
#include <commctrl.h>

#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace UiTheme {
constexpr COLORREF background = RGB(11, 13, 31);
constexpr COLORREF panelColor = RGB(20, 24, 53);
constexpr COLORREF raised = RGB(37, 29, 76);
constexpr COLORREF selected = RGB(55, 39, 99);
constexpr COLORREF edge = RGB(74, 66, 129);
constexpr COLORREF track = RGB(54, 50, 94);
constexpr COLORREF textColor = RGB(240, 243, 255);
constexpr COLORREF muted = RGB(172, 178, 216);
constexpr COLORREF disabled = RGB(125, 132, 171);
constexpr COLORREF accent = RGB(116, 211, 255);
constexpr COLORREF violet = RGB(175, 145, 255);
constexpr COLORREF coral = RGB(255, 94, 119);

inline void fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void outline(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void gradient(HDC dc, RECT rect, COLORREF top, COLORREF bottom) {
    const auto vertex = [](LONG x, LONG y, COLORREF color) -> TRIVERTEX {
        return {x, y, COLOR16(GetRValue(color) << 8), COLOR16(GetGValue(color) << 8),
            COLOR16(GetBValue(color) << 8), 0};
    };
    TRIVERTEX vertices[]{vertex(rect.left, rect.top, top), vertex(rect.right, rect.bottom, bottom)};
    GRADIENT_RECT mesh{0, 1};
    GradientFill(dc, vertices, 2, &mesh, 1, GRADIENT_FILL_RECT_V);
}

inline void rule(HDC dc, LONG left, LONG right, LONG y, LONG thickness = 2) {
    fill(dc, {left, y, right, y + thickness}, edge);
    const LONG tip = std::min(right, left + 52);
    fill(dc, {left, y, tip, y + thickness}, coral);
    fill(dc, {tip + 4, y, std::min(right, tip + 20), y + thickness}, accent);
}

inline void tile(HDC dc, RECT rect, bool focused, bool active = false, bool enabled = true) {
    gradient(dc, rect, focused ? selected : active ? raised : panelColor, background);
    outline(dc, rect, focused ? accent : active ? violet : edge);
    if (focused || active) {
        fill(dc, {rect.left + 1, rect.top + 1, rect.left + 4, rect.bottom - 1},
            focused ? accent : coral);
    }
    SetTextColor(dc, enabled ? textColor : disabled);
    SetBkMode(dc, TRANSPARENT);
}

inline HFONT font(int height, int weight, const wchar_t* face = L"Bahnschrift") {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

void styleButton(HWND button, bool primary, bool checkbox);
void styleViewButton(HWND button, bool grid, bool active);
void styleQuitButton(HWND button);
void drawViewIcon(HDC dc, LONG x, LONG y, bool grid, COLORREF color);
void drawQuitIcon(HDC dc, LONG x, LONG y, bool focused);
void styleCombo(HWND combo);
void paintButton(const DRAWITEMSTRUCT& item);
void paintChoice(const DRAWITEMSTRUCT& item);
LRESULT paintTrack(const NMCUSTOMDRAW& trackDraw, COLORREF color);
}
