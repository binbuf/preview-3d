#include "framework.h"
#include "ControlsDialog.h"
#include "Resource.h"

#include <dwmapi.h>

#include <array>

namespace
{
constexpr wchar_t kControlsWindowClass[] = L"Preview3DControlsWindow";
constexpr int kCloseButtonId = 1;
constexpr int kDialogWidth = 1040;
constexpr int kDialogHeight = 650;

struct Palette
{
    COLORREF background = RGB(24, 24, 27);
    COLORREF surface = RGB(34, 34, 38);
    COLORREF elevated = RGB(45, 45, 50);
    COLORREF border = RGB(67, 67, 73);
    COLORREF text = RGB(244, 244, 246);
    COLORREF secondary = RGB(174, 174, 181);
    COLORREF muted = RGB(112, 112, 120);
    COLORREF flight = RGB(73, 156, 255);
    COLORREF orbit = RGB(179, 126, 255);
    COLORREF views = RGB(255, 178, 79);
    COLORREF actions = RGB(74, 205, 145);
    COLORREF zoom = RGB(65, 202, 214);
};

struct DialogState
{
    HWND window = nullptr;
    HWND owner = nullptr;
    HWND closeButton = nullptr;
    UINT dpi = 96;
    bool highContrast = false;
    HFONT titleFont = nullptr;
    HFONT sectionFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT keyFont = nullptr;
    Palette colors;
};

int Scale(const DialogState& state, int value)
{
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

RECT ScaledRect(const DialogState& state, int left, int top, int right, int bottom)
{
    return { Scale(state, left), Scale(state, top), Scale(state, right), Scale(state, bottom) };
}

COLORREF Blend(COLORREF first, COLORREF second, int secondWeight)
{
    const int firstWeight = 255 - secondWeight;
    return RGB(
        (GetRValue(first) * firstWeight + GetRValue(second) * secondWeight) / 255,
        (GetGValue(first) * firstWeight + GetGValue(second) * secondWeight) / 255,
        (GetBValue(first) * firstWeight + GetBValue(second) * secondWeight) / 255);
}

bool HighContrastEnabled()
{
    HIGHCONTRASTW contrast{ sizeof(contrast) };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

Palette CurrentPalette(bool highContrast)
{
    if (!highContrast) return {};

    Palette colors;
    colors.background = GetSysColor(COLOR_WINDOW);
    colors.surface = GetSysColor(COLOR_WINDOW);
    colors.elevated = GetSysColor(COLOR_BTNFACE);
    colors.border = GetSysColor(COLOR_WINDOWTEXT);
    colors.text = GetSysColor(COLOR_WINDOWTEXT);
    colors.secondary = GetSysColor(COLOR_WINDOWTEXT);
    colors.muted = GetSysColor(COLOR_GRAYTEXT);
    colors.flight = GetSysColor(COLOR_HIGHLIGHT);
    colors.orbit = GetSysColor(COLOR_HIGHLIGHT);
    colors.views = GetSysColor(COLOR_HIGHLIGHT);
    colors.actions = GetSysColor(COLOR_HIGHLIGHT);
    colors.zoom = GetSysColor(COLOR_HIGHLIGHT);
    return colors;
}

HFONT MakeFont(UINT dpi, int points, int weight)
{
    return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void DeleteFonts(DialogState& state)
{
    if (state.titleFont) DeleteObject(state.titleFont);
    if (state.sectionFont) DeleteObject(state.sectionFont);
    if (state.bodyFont) DeleteObject(state.bodyFont);
    if (state.smallFont) DeleteObject(state.smallFont);
    if (state.keyFont) DeleteObject(state.keyFont);
    state.titleFont = nullptr;
    state.sectionFont = nullptr;
    state.bodyFont = nullptr;
    state.smallFont = nullptr;
    state.keyFont = nullptr;
}

void RecreateFonts(DialogState& state)
{
    DeleteFonts(state);
    state.titleFont = MakeFont(state.dpi, 22, FW_SEMIBOLD);
    state.sectionFont = MakeFont(state.dpi, 11, FW_SEMIBOLD);
    state.bodyFont = MakeFont(state.dpi, 10, FW_NORMAL);
    state.smallFont = MakeFont(state.dpi, 9, FW_NORMAL);
    state.keyFont = MakeFont(state.dpi, 8, FW_SEMIBOLD);
    if (state.closeButton) SendMessageW(state.closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(state.bodyFont), TRUE);
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, int borderWidth = 1)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    const HGDIOBJ previousBrush = SelectObject(dc, brush);
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawTextLine(HDC dc, HFONT font, COLORREF color, const wchar_t* text, RECT rect, UINT format)
{
    const HGDIOBJ previousFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, format | DT_NOPREFIX);
    SelectObject(dc, previousFont);
}

void DrawSectionTitle(HDC dc, const DialogState& state, const wchar_t* text, int x, int y, COLORREF accent)
{
    RECT marker = ScaledRect(state, x, y + 2, x + 3, y + 17);
    HBRUSH markerBrush = CreateSolidBrush(accent);
    FillRect(dc, &marker, markerBrush);
    DeleteObject(markerBrush);
    DrawTextLine(dc, state.sectionFont, state.colors.text, text,
        ScaledRect(state, x + 12, y, x + 260, y + 22), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

RECT DrawKey(HDC dc, const DialogState& state, int x, int y, int width, const wchar_t* label,
    COLORREF accent = CLR_INVALID, int height = 34)
{
    const RECT bounds = ScaledRect(state, x, y, x + width, y + height);
    const bool highlighted = accent != CLR_INVALID;
    const COLORREF fill = highlighted ? Blend(state.colors.elevated, accent, 75) : state.colors.elevated;
    const COLORREF border = highlighted ? accent : state.colors.border;
    FillRoundedRect(dc, bounds, Scale(state, 7), fill, border, highlighted ? Scale(state, 2) : 1);
    DrawTextLine(dc, state.keyFont, highlighted ? state.colors.text : state.colors.secondary,
        label, bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return bounds;
}

POINT RightCenter(const RECT& rect)
{
    return { rect.right, (rect.top + rect.bottom) / 2 };
}

void DrawConnector(HDC dc, const DialogState& state, POINT start, int labelY, COLORREF color,
    int verticalRouteX = 0)
{
    const int bendX = Scale(state, 690);
    const int endX = Scale(state, 716);
    const int endY = Scale(state, labelY + 13);
    std::array<POINT, 4> points{};
    if (verticalRouteX > 0)
    {
        const int routeX = Scale(state, verticalRouteX);
        points = { start, POINT{ routeX, start.y }, POINT{ routeX, endY }, POINT{ endX, endY } };
    }
    else
    {
        points = { start, POINT{ bendX - Scale(state, 12), start.y },
            POINT{ bendX, endY }, POINT{ endX, endY } };
    }
    HPEN pen = CreatePen(PS_SOLID, Scale(state, 2), color);
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, previousPen);
    DeleteObject(pen);

    HBRUSH dot = CreateSolidBrush(color);
    HGDIOBJ previousBrush = SelectObject(dc, dot);
    HPEN nullPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
    const HGDIOBJ previousDotPen = SelectObject(dc, nullPen);
    const int radius = Scale(state, 4);
    Ellipse(dc, endX - radius, endY - radius, endX + radius, endY + radius);
    SelectObject(dc, previousDotPen);
    SelectObject(dc, previousBrush);
    DeleteObject(dot);
}

void DrawCallout(HDC dc, const DialogState& state, int y, COLORREF accent,
    const wchar_t* title, const wchar_t* detail)
{
    RECT titleRect = ScaledRect(state, 731, y, 996, y + 20);
    DrawTextLine(dc, state.sectionFont, accent, title, titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT detailRect = ScaledRect(state, 731, y + 22, 996, y + 55);
    DrawTextLine(dc, state.smallFont, state.colors.secondary, detail, detailRect,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void DrawKeyboard(HDC dc, const DialogState& state)
{
    const auto& colors = state.colors;
    DrawSectionTitle(dc, state, L"KEYBOARD", 48, 119, colors.flight);
    DrawTextLine(dc, state.smallFont, colors.muted, L"Highlighted keys are grouped by what they control",
        ScaledRect(state, 151, 118, 600, 140), DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    DrawKey(dc, state, 50, 153, 43, L"Esc");
    DrawKey(dc, state, 363, 153, 48, L"Home", colors.actions);
    DrawKey(dc, state, 423, 153, 39, L"F11");
    DrawKey(dc, state, 497, 153, 39, L"−", colors.zoom);
    DrawKey(dc, state, 541, 153, 39, L"+", colors.zoom);

    constexpr std::array<const wchar_t*, 10> numberLabels{ L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"0" };
    for (std::size_t index = 0; index < numberLabels.size(); ++index)
        DrawKey(dc, state, 50 + static_cast<int>(index) * 39, 198, 34, numberLabels[index]);

    constexpr std::array<const wchar_t*, 10> topLabels{ L"Q", L"W", L"E", L"R", L"T", L"Y", L"U", L"I", L"O", L"P" };
    RECT flightAnchor{};
    for (std::size_t index = 0; index < topLabels.size(); ++index)
    {
        const bool flight = index <= 2;
        const bool action = index == 3;
        const COLORREF accent = flight ? colors.flight : (action ? colors.actions : CLR_INVALID);
        const RECT key = DrawKey(dc, state, 62 + static_cast<int>(index) * 39, 239, 34, topLabels[index], accent);
        if (index == 2) flightAnchor = key;
    }

    constexpr std::array<const wchar_t*, 9> middleLabels{ L"A", L"S", L"D", L"F", L"G", L"H", L"J", L"K", L"L" };
    RECT actionAnchor{};
    for (std::size_t index = 0; index < middleLabels.size(); ++index)
    {
        const bool flight = index <= 2;
        const bool action = index == 3 || index == 4;
        const COLORREF accent = flight ? colors.flight : (action ? colors.actions : CLR_INVALID);
        const RECT key = DrawKey(dc, state, 76 + static_cast<int>(index) * 39, 280, 34, middleLabels[index], accent);
        if (index == 4) actionAnchor = key;
    }

    DrawKey(dc, state, 50, 321, 70, L"Shift", colors.flight);
    constexpr std::array<const wchar_t*, 7> bottomLabels{ L"Z", L"X", L"C", L"V", L"B", L"N", L"M" };
    for (std::size_t index = 0; index < bottomLabels.size(); ++index)
    {
        const bool flight = index == 0 || index == 2;
        DrawKey(dc, state, 125 + static_cast<int>(index) * 39, 321, 34, bottomLabels[index],
            flight ? colors.flight : CLR_INVALID);
    }
    DrawKey(dc, state, 125, 362, 54, L"Ctrl");
    DrawKey(dc, state, 184, 362, 210, L"Space");

    DrawTextLine(dc, state.keyFont, colors.muted, L"ARROWS",
        ScaledRect(state, 420, 323, 488, 339), DT_CENTER | DT_SINGLELINE);
    const RECT up = DrawKey(dc, state, 441, 341, 34, L"↑", colors.orbit, 28);
    DrawKey(dc, state, 402, 374, 34, L"←", colors.orbit, 28);
    DrawKey(dc, state, 441, 374, 34, L"↓", colors.orbit, 28);
    DrawKey(dc, state, 480, 374, 34, L"→", colors.orbit, 28);

    DrawTextLine(dc, state.keyFont, colors.muted, L"NUMPAD",
        ScaledRect(state, 538, 323, 616, 339), DT_CENTER | DT_SINGLELINE);
    DrawKey(dc, state, 540, 342, 32, L"7", colors.views, 28);
    DrawKey(dc, state, 576, 342, 32, L"/", CLR_INVALID, 28);
    DrawKey(dc, state, 540, 374, 32, L"1", colors.views, 28);
    DrawKey(dc, state, 576, 374, 32, L"3", colors.views, 28);
    DrawKey(dc, state, 540, 406, 32, L"5", colors.views, 28);
    const RECT viewAnchor = DrawKey(dc, state, 576, 406, 32, L".", colors.views, 28);

    DrawConnector(dc, state, RightCenter(flightAnchor), 137, colors.flight);
    // Route upward through the gap between the arrows and numpad so the line
    // does not cross either control group now that they share a baseline.
    DrawConnector(dc, state, RightCenter(up), 211, colors.orbit, 524);
    DrawConnector(dc, state, RightCenter(actionAnchor), 285, colors.actions);
    DrawConnector(dc, state, RightCenter(viewAnchor), 359, colors.views);

    DrawCallout(dc, state, 137, colors.flight, L"FLY", L"Hold right mouse · W A S D move\nQ / E rise · Z / C roll · Shift boosts");
    DrawCallout(dc, state, 211, colors.orbit, L"ORBIT & PAN", L"Arrow keys orbit\nShift + arrows pan along the ground");
    DrawCallout(dc, state, 285, colors.actions, L"FRAME & RESET", L"F frames selection · G toggles grid\nR or Home resets the view");
    DrawCallout(dc, state, 359, colors.views, L"VIEW SNAP", L"Num 1 / 3 / 7 · Ctrl reverses\nNum 5 toggles perspective · Num . frames");
    DrawTextLine(dc, state.smallFont, colors.muted,
        L"Gizmo: click an axis ball to snap\nToolbar: Ground axis cycles · Snap locks pan",
        ScaledRect(state, 731, 417, 998, 451), DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void DrawMouseCard(HDC dc, const DialogState& state)
{
    const auto& colors = state.colors;
    const RECT card = ScaledRect(state, 24, 470, 548, 591);
    FillRoundedRect(dc, card, Scale(state, 12), colors.surface, colors.border);
    DrawSectionTitle(dc, state, L"MOUSE", 44, 486, colors.zoom);

    const RECT mouse = ScaledRect(state, 45, 519, 91, 575);
    FillRoundedRect(dc, mouse, Scale(state, 20), colors.elevated, colors.border);
    HPEN divider = CreatePen(PS_SOLID, 1, colors.border);
    HGDIOBJ previousPen = SelectObject(dc, divider);
    MoveToEx(dc, Scale(state, 68), Scale(state, 519), nullptr);
    LineTo(dc, Scale(state, 68), Scale(state, 541));
    MoveToEx(dc, Scale(state, 45), Scale(state, 541), nullptr);
    LineTo(dc, Scale(state, 91), Scale(state, 541));
    SelectObject(dc, previousPen);
    DeleteObject(divider);
    const RECT wheel = ScaledRect(state, 64, 526, 72, 538);
    FillRoundedRect(dc, wheel, Scale(state, 4), colors.zoom, colors.zoom);

    DrawTextLine(dc, state.bodyFont, colors.text, L"Left", ScaledRect(state, 112, 515, 162, 536), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextLine(dc, state.smallFont, colors.secondary, L"click select / clear · drag orbit · double-click frame",
        ScaledRect(state, 162, 515, 518, 536), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextLine(dc, state.bodyFont, colors.text, L"Middle", ScaledRect(state, 112, 538, 172, 559), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextLine(dc, state.smallFont, colors.secondary, L"drag pan · Ctrl + drag zoom",
        ScaledRect(state, 172, 538, 518, 559), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextLine(dc, state.bodyFont, colors.text, L"Right", ScaledRect(state, 112, 561, 165, 582), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextLine(dc, state.smallFont, colors.secondary, L"hold for fly look · wheel changes fly speed",
        ScaledRect(state, 165, 561, 518, 582), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void DrawShortcut(HDC dc, const DialogState& state, int x, int y, int keyWidth,
    const wchar_t* key, const wchar_t* action)
{
    const RECT keyRect = DrawKey(dc, state, x, y, keyWidth, key, state.colors.zoom, 27);
    RECT actionRect{ keyRect.right + Scale(state, 10), keyRect.top,
        Scale(state, x + 205), keyRect.bottom };
    DrawTextLine(dc, state.smallFont, state.colors.secondary, action, actionRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void DrawAppCard(HDC dc, const DialogState& state)
{
    const RECT card = ScaledRect(state, 562, 470, 1016, 591);
    FillRoundedRect(dc, card, Scale(state, 12), state.colors.surface, state.colors.border);
    DrawSectionTitle(dc, state, L"VIEWER", 582, 486, state.colors.actions);
    DrawShortcut(dc, state, 582, 520, 60, L"Ctrl O", L"Open model");
    DrawShortcut(dc, state, 762, 520, 47, L"F11", L"Fullscreen");
    DrawShortcut(dc, state, 582, 554, 47, L"Esc", L"Cancel / exit");
    DrawShortcut(dc, state, 762, 554, 47, L"?", L"This guide");
}

void PaintDialog(DialogState& state, HDC target, const RECT& client)
{
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    const HGDIOBJ previousBitmap = SelectObject(buffer, bitmap);

    HBRUSH background = CreateSolidBrush(state.colors.background);
    FillRect(buffer, &client, background);
    DeleteObject(background);

    DrawTextLine(buffer, state.titleFont, state.colors.text, L"Controls",
        ScaledRect(state, 24, 20, 300, 56), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextLine(buffer, state.bodyFont, state.colors.secondary,
        L"A quick map for navigating your model",
        ScaledRect(state, 24, 58, 500, 82), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    DrawTextLine(buffer, state.smallFont, state.colors.muted,
        L"Drag gestures wrap at the screen edge and glide to a smooth stop.",
        ScaledRect(state, 540, 36, 1016, 60), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    const RECT keyboardCard = ScaledRect(state, 24, 98, 1016, 456);
    FillRoundedRect(buffer, keyboardCard, Scale(state, 12), state.colors.surface, state.colors.border);
    DrawKeyboard(buffer, state);
    DrawMouseCard(buffer, state);
    DrawAppCard(buffer, state);

    DrawTextLine(buffer, state.smallFont, state.colors.muted, L"Press Esc to close",
        ScaledRect(state, 24, 606, 250, 635), DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void LayoutCloseButton(DialogState& state)
{
    if (!state.closeButton) return;
    SetWindowPos(state.closeButton, nullptr, Scale(state, 926), Scale(state, 604),
        Scale(state, 90), Scale(state, 32), SWP_NOZORDER | SWP_NOACTIVATE);
}

void DrawCloseButton(DialogState& state, const DRAWITEMSTRUCT& item)
{
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    COLORREF fill = pressed ? Blend(state.colors.elevated, state.colors.zoom, 80) : state.colors.elevated;
    COLORREF border = focused ? state.colors.zoom : state.colors.border;
    FillRoundedRect(item.hDC, item.rcItem, Scale(state, 7), fill, border, focused ? Scale(state, 2) : 1);
    DrawTextLine(item.hDC, state.bodyFont, state.colors.text, L"Done", item.rcItem,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK ControlsWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);

    switch (message)
    {
    case WM_CREATE:
    {
        state->dpi = GetDpiForWindow(window);
        state->highContrast = HighContrastEnabled();
        state->colors = CurrentPalette(state->highContrast);
        RecreateFonts(*state);
        state->closeButton = CreateWindowExW(0, L"BUTTON", L"Done",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)),
            GetModuleHandleW(nullptr), nullptr);
        if (state->closeButton)
        {
            SendMessageW(state->closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(state->bodyFont), TRUE);
            LayoutCloseButton(*state);
        }

        const BOOL dark = state->highContrast ? FALSE : TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const DWORD cornerPreference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(window, 33, &cornerPreference, sizeof(cornerPreference));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kCloseButtonId || LOWORD(wParam) == IDCANCEL) DestroyWindow(window);
        return 0;
    case WM_DRAWITEM:
        if (wParam == kCloseButtonId)
        {
            DrawCloseButton(*state, *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        break;
    case WM_DPICHANGED:
    {
        state->dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateFonts(*state);
        LayoutCloseButton(*state);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        state->highContrast = HighContrastEnabled();
        state->colors = CurrentPalette(state->highContrast);
        InvalidateRect(window, nullptr, TRUE);
        if (state->closeButton) InvalidateRect(state->closeButton, nullptr, TRUE);
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        PaintDialog(*state, dc, client);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        DeleteFonts(*state);
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    if (GetClassInfoExW(instance, kControlsWindowClass, &windowClass)) return true;

    windowClass = { sizeof(windowClass) };
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ControlsWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kControlsWindowClass;
    return RegisterClassExW(&windowClass) != 0;
}
}

void ShowControlsDialog(HWND owner)
{
    if (!owner || !IsWindow(owner)) return;
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!EnsureWindowClass(instance)) return;

    DialogState state;
    state.owner = owner;
    state.dpi = GetDpiForWindow(owner);

    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT bounds{ 0, 0, Scale(state, kDialogWidth), Scale(state, kDialogHeight) };
    AdjustWindowRectExForDpi(&bounds, style, FALSE, extendedStyle, state.dpi);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;

    RECT ownerBounds{};
    GetWindowRect(owner, &ownerBounds);
    const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    GetMonitorInfoW(monitor, &monitorInfo);
    int x = ownerBounds.left + ((ownerBounds.right - ownerBounds.left) - width) / 2;
    int y = ownerBounds.top + ((ownerBounds.bottom - ownerBounds.top) - height) / 2;
    x = std::clamp(x, static_cast<int>(monitorInfo.rcWork.left),
        std::max(static_cast<int>(monitorInfo.rcWork.left), static_cast<int>(monitorInfo.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(monitorInfo.rcWork.top),
        std::max(static_cast<int>(monitorInfo.rcWork.top), static_cast<int>(monitorInfo.rcWork.bottom) - height));

    HWND dialog = CreateWindowExW(extendedStyle, kControlsWindowClass, L"Preview 3D controls",
        style, x, y, width, height, owner, nullptr, instance, &state);
    if (!dialog) return;

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    if (state.closeButton) SetFocus(state.closeButton);

    bool receivedQuit = false;
    int quitCode = 0;
    MSG message{};
    while (IsWindow(dialog))
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0)
        {
            receivedQuit = result == 0;
            quitCode = static_cast<int>(message.wParam);
            break;
        }
        if (!IsDialogMessageW(dialog, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (receivedQuit) PostQuitMessage(quitCode);
}
