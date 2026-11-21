#include "ui-theme.h"

namespace UiTheme {

void drawViewIcon(HDC dc, LONG x, LONG y, bool grid, COLORREF color) {
    if (grid) {
        for (LONG dy : {0, 7})
            for (LONG dx : {0, 7})
                fill(dc, {x + dx, y + dy, x + dx + 5, y + dy + 5}, color);
    } else {
        for (LONG dy : {0, 5, 10}) {
            fill(dc, {x, y + dy, x + 2, y + dy + 2}, color);
            fill(dc, {x + 4, y + dy, x + 12, y + dy + 2}, color);
        }
    }
}

void drawQuitIcon(HDC dc, LONG x, LONG y, bool focused) {
    outline(dc, {x + 4, y + 2, x + 17, y + 16}, violet);
    const POINT door[]{{x + 1, y + 3}, {x + 10, y}, {x + 10, y + 18}, {x + 1, y + 15}};
    const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, selected);
    SetDCPenColor(dc, focused ? accent : coral);
    Polygon(dc, door, ARRAYSIZE(door));
    fill(dc, {x + 6, y + 8, x + 8, y + 10}, focused ? textColor : coral);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

namespace {
constexpr UINT_PTR buttonStyle = 1;
constexpr DWORD_PTR primaryButton = 1;
constexpr DWORD_PTR checkButton = 2;
constexpr DWORD_PTR hoveredButton = 4;
constexpr DWORD_PTR viewButton = 8;
constexpr DWORD_PTR gridButton = 16;
constexpr DWORD_PTR quitButton = 32;

void drawButton(HWND button, HDC dc, RECT area, DWORD_PTR appearance, bool pressed, bool focused) {
    const bool enabled = IsWindowEnabled(button) != FALSE;
    const bool checkbox = (appearance & checkButton) != 0;
    const bool checked = checkbox && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool highlight = enabled && (focused || (appearance & hoveredButton) != 0);
    const bool active = enabled && (pressed || checked || (appearance & primaryButton) != 0);
    tile(dc, area, highlight, active, enabled);
    if (pressed && enabled) fill(dc, {area.left + 5, area.top + 2, area.right - 2, area.bottom - 2}, selected);
    if (checkbox) {
        const LONG y = (area.top + area.bottom) / 2;
        RECT box{area.left + 9, y - 6, area.left + 21, y + 6};
        outline(dc, box, enabled ? violet : edge);
        InflateRect(&box, -3, -3);
        if (checked) fill(dc, box, enabled ? coral : disabled);
        area.left += 27;
    } else {
        InflateRect(&area, -3, 0);
    }
    const bool view = (appearance & viewButton) != 0;
    const bool quit = (appearance & quitButton) != 0;
    if (view) {
        drawViewIcon(dc, area.left + 5, (area.top + area.bottom) / 2 - 6,
            (appearance & gridButton) != 0, active || highlight ? accent : violet);
        area.left += 20;
    }
    if (quit) {
        drawQuitIcon(dc, area.left + 8, (area.top + area.bottom) / 2 - 9, highlight || pressed);
        area.left += 32;
    }
    area.right -= 2;
    wchar_t label[128]{};
    GetWindowTextW(button, label, ARRAYSIZE(label));
    const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
    DrawTextW(dc, label, -1, &area, (checkbox || view || quit ? DT_LEFT : DT_CENTER) |
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK buttonProc(HWND button, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR appearance) {
    const bool checkbox = (appearance & checkButton) != 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(button, buttonProc, id);
    if (message == WM_MOUSEMOVE && !(appearance & hoveredButton)) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, button, 0};
        if (TrackMouseEvent(&tracking)) {
            SetWindowSubclass(button, buttonProc, id, appearance | hoveredButton);
            InvalidateRect(button, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE) {
        SetWindowSubclass(button, buttonProc, id, appearance & ~hoveredButton);
        InvalidateRect(button, nullptr, FALSE);
    }
    if (checkbox && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
        PAINTSTRUCT paint{};
        auto dc = message == WM_PAINT ? BeginPaint(button, &paint) : reinterpret_cast<HDC>(w);
        RECT area{};
        GetClientRect(button, &area);
        const auto state = SendMessageW(button, BM_GETSTATE, 0, 0);
        drawButton(button, dc, area, appearance, (state & BST_PUSHED) != 0, (state & BST_FOCUS) != 0);
        if (message == WM_PAINT) EndPaint(button, &paint);
        return 0;
    }
    const auto result = DefSubclassProc(button, message, w, l);
    if (checkbox && (message == BM_SETCHECK || message == BM_SETSTATE || message == WM_ENABLE ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS)) {
        InvalidateRect(button, nullptr, FALSE);
    }
    return result;
}

void drawComboArrow(HWND combo, HDC dc) {
    COMBOBOXINFO info{sizeof(info)};
    RECT area{};
    if (!GetComboBoxInfo(combo, &info) || !GetClientRect(combo, &area)) return;
    const bool focused = GetFocus() == combo || SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0);
    outline(dc, area, focused ? accent : edge);
    RECT button{info.rcButton.left - 1, 1, area.right - 1, area.bottom - 1};
    gradient(dc, button, focused ? selected : raised, background);
    fill(dc, {button.left, button.top, button.left + 1, button.bottom}, edge);
    const LONG x = (button.left + button.right) / 2;
    const LONG y = (button.top + button.bottom) / 2;
    for (LONG i = 0; i < 4; ++i) {
        fill(dc, {x - 4 + i, y - 1 + i, x + 5 - i, y + i}, focused ? accent : violet);
    }
}

LRESULT CALLBACK comboProc(HWND combo, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(combo, comboProc, id);
    const auto result = DefSubclassProc(combo, message, w, l);
    if (message == WM_PAINT) {
        if (const auto dc = GetDC(combo)) {
            drawComboArrow(combo, dc);
            ReleaseDC(combo, dc);
        }
    } else if (message == WM_PRINTCLIENT) {
        drawComboArrow(combo, reinterpret_cast<HDC>(w));
    }
    return result;
}
}

void styleButton(HWND button, bool primary, bool checkbox) {
    SetWindowSubclass(button, buttonProc, buttonStyle, primary ? primaryButton : checkbox ? checkButton : 0);
}

void styleViewButton(HWND button, bool grid, bool active) {
    DWORD_PTR appearance = 0;
    GetWindowSubclass(button, buttonProc, buttonStyle, &appearance);
    SetWindowSubclass(button, buttonProc, buttonStyle,
        (appearance & hoveredButton) | viewButton | (grid ? gridButton : 0) | (active ? primaryButton : 0));
    InvalidateRect(button, nullptr, FALSE);
}

void styleQuitButton(HWND button) {
    SetWindowSubclass(button, buttonProc, buttonStyle, quitButton);
}

void styleCombo(HWND combo) {
    SetWindowSubclass(combo, comboProc, 1, 0);
}

void paintButton(const DRAWITEMSTRUCT& item) {
    DWORD_PTR appearance = 0;
    GetWindowSubclass(item.hwndItem, buttonProc, buttonStyle, &appearance);
    drawButton(item.hwndItem, item.hDC, item.rcItem, appearance, (item.itemState & ODS_SELECTED) != 0,
        (item.itemState & ODS_FOCUS) != 0);
}

void paintChoice(const DRAWITEMSTRUCT& item) {
    RECT area = item.rcItem;
    const bool combo = item.CtlType == ODT_COMBOBOX;
    const bool field = combo && (item.itemState & ODS_COMBOBOXEDIT);
    const bool highlighted = (item.itemState & ODS_SELECTED) != 0;
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    if (highlighted || field) {
        tile(item.hDC, area, highlighted, field, enabled);
    } else {
        fill(item.hDC, area, panelColor);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, enabled ? textColor : disabled);
    }
    if (item.itemID == static_cast<UINT>(-1)) return;
    const UINT lengthMessage = combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
    const UINT textMessage = combo ? CB_GETLBTEXT : LB_GETTEXT;
    const auto length = SendMessageW(item.hwndItem, lengthMessage, item.itemID, 0);
    if (length < 0) return;
    std::wstring label(length + 1, L'\0');
    SendMessageW(item.hwndItem, textMessage, item.itemID, reinterpret_cast<LPARAM>(label.data()));
    area.left += 8;
    area.right -= 5;
    DrawTextW(item.hDC, label.c_str(), -1, &area, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

LRESULT paintTrack(const NMCUSTOMDRAW& trackDraw, COLORREF color) {
    if (trackDraw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const auto window = trackDraw.hdr.hwndFrom;
    RECT area{}, channel{}, thumb{};
    GetClientRect(window, &area);
    SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const LONG x = (thumb.left + thumb.right) / 2;
    const LONG y = (channel.top + channel.bottom) / 2;
    fill(trackDraw.hdc, area, background);
    if (GetFocus() == window) outline(trackDraw.hdc, area, accent);
    fill(trackDraw.hdc, {channel.left, y - 3, channel.right, y + 3}, track);
    fill(trackDraw.hdc, {channel.left, y - 1, x, y + 2}, color);
    fill(trackDraw.hdc, {x - 5, y - 7, x + 5, y + 7}, color);
    fill(trackDraw.hdc, {x - 1, y - 5, x + 1, y + 5}, textColor);
    return CDRF_SKIPDEFAULT;
}
}
