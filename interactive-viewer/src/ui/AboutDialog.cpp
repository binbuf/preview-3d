#include "framework.h"
#include "AboutDialog.h"
#include "Localization.h"
#include "Resource.h"
#include "Version.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <string>

namespace
{
constexpr wchar_t kAboutWindowClass[] = L"Preview3DAboutWindow";
constexpr int kCloseButtonId = 1;
constexpr int kGitHubLinkId = 2;
constexpr int kLicenseLinkId = 3;

constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 440;

constexpr wchar_t kProjectUrl[] = L"https://github.com/binbuf/preview-3d";
constexpr wchar_t kLicenseUrl[] = L"https://github.com/binbuf/preview-3d/blob/main/LICENSE";

struct Palette
{
    COLORREF background = RGB(22, 22, 26);
    COLORREF heroTop = RGB(32, 34, 43);
    COLORREF heroBottom = RGB(22, 22, 26);
    COLORREF surface = RGB(33, 34, 40);
    COLORREF elevated = RGB(45, 46, 54);
    COLORREF border = RGB(64, 65, 74);
    COLORREF separator = RGB(47, 48, 56);
    COLORREF text = RGB(245, 245, 248);
    COLORREF secondary = RGB(176, 177, 186);
    COLORREF muted = RGB(124, 125, 136);
    COLORREF accent = RGB(86, 156, 255);
    COLORREF accentAlt = RGB(168, 120, 255);
    COLORREF link = RGB(116, 176, 255);
};

struct AboutState
{
    HWND window = nullptr;
    HWND owner = nullptr;
    HWND closeButton = nullptr;
    UINT dpi = 96;
    bool highContrast = false;
    HFONT nameFont = nullptr;
    HFONT pillFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT valueFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT linkFont = nullptr;
    HICON icon = nullptr;
    bool iconOwned = false;
    RECT githubRect{};
    RECT licenseRect{};
    int hoveredLink = 0;
    bool trackingMouse = false;
    Palette colors;
};

int Scale(const AboutState& state, int value)
{
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

RECT ScaledRect(const AboutState& state, int left, int top, int right, int bottom)
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
    colors.heroTop = GetSysColor(COLOR_WINDOW);
    colors.heroBottom = GetSysColor(COLOR_WINDOW);
    colors.surface = GetSysColor(COLOR_WINDOW);
    colors.elevated = GetSysColor(COLOR_BTNFACE);
    colors.border = GetSysColor(COLOR_WINDOWTEXT);
    colors.separator = GetSysColor(COLOR_WINDOWTEXT);
    colors.text = GetSysColor(COLOR_WINDOWTEXT);
    colors.secondary = GetSysColor(COLOR_WINDOWTEXT);
    colors.muted = GetSysColor(COLOR_GRAYTEXT);
    colors.accent = GetSysColor(COLOR_HIGHLIGHT);
    colors.accentAlt = GetSysColor(COLOR_HIGHLIGHT);
    colors.link = GetSysColor(COLOR_HOTLIGHT);
    return colors;
}

HFONT MakeFont(UINT dpi, int points, int weight)
{
    return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void DeleteFonts(AboutState& state)
{
    if (state.nameFont) DeleteObject(state.nameFont);
    if (state.pillFont) DeleteObject(state.pillFont);
    if (state.bodyFont) DeleteObject(state.bodyFont);
    if (state.valueFont) DeleteObject(state.valueFont);
    if (state.smallFont) DeleteObject(state.smallFont);
    if (state.linkFont) DeleteObject(state.linkFont);
    state.nameFont = nullptr;
    state.pillFont = nullptr;
    state.bodyFont = nullptr;
    state.valueFont = nullptr;
    state.smallFont = nullptr;
    state.linkFont = nullptr;
}

void RecreateFonts(AboutState& state)
{
    DeleteFonts(state);
    state.nameFont = MakeFont(state.dpi, 23, FW_SEMIBOLD);
    state.pillFont = MakeFont(state.dpi, 9, FW_SEMIBOLD);
    state.bodyFont = MakeFont(state.dpi, 10, FW_NORMAL);
    state.valueFont = MakeFont(state.dpi, 10, FW_SEMIBOLD);
    state.smallFont = MakeFont(state.dpi, 9, FW_NORMAL);
    state.linkFont = MakeFont(state.dpi, 10, FW_NORMAL);
}

void LoadAppIcon(AboutState& state, HINSTANCE instance)
{
    if (state.icon && state.iconOwned) DestroyIcon(state.icon);
    state.icon = nullptr;
    state.iconOwned = false;

    const int size = Scale(state, 64);
    state.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D), IMAGE_ICON,
        size, size, LR_DEFAULTCOLOR));
    if (state.icon)
    {
        state.iconOwned = true;
    }
    else
    {
        state.icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D));
    }
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

void FillVerticalGradient(HDC dc, const RECT& rect, COLORREF top, COLORREF bottom)
{
    const int height = rect.bottom - rect.top;
    if (height <= 0) return;
    const int bands = std::min(48, height);
    for (int band = 0; band < bands; ++band)
    {
        const int bandTop = rect.top + height * band / bands;
        const int bandBottom = rect.top + height * (band + 1) / bands;
        const COLORREF color = Blend(top, bottom, (band + 1) * 255 / bands);
        HBRUSH brush = CreateSolidBrush(color);
        RECT bandRect{ rect.left, bandTop, rect.right, bandBottom };
        FillRect(dc, &bandRect, brush);
        DeleteObject(brush);
    }
}

void DrawTextLine(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT rect, UINT format)
{
    const HGDIOBJ previousFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
    SelectObject(dc, previousFont);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text)
{
    const HGDIOBJ previousFont = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previousFont);
    return size.cx;
}

std::wstring GitHubLabel()
{
    return Loc("about.github", L"GitHub") + L"  \u2197";
}

std::wstring LicenseLabel()
{
    return Loc("about.licenseLabel", L"License") + L"  \u2197";
}

void LayoutLinks(AboutState& state)
{
    if (!state.window) return;
    HDC dc = GetDC(state.window);
    if (!dc) return;
    const int githubWidth = TextWidth(dc, state.linkFont, GitHubLabel());
    const int licenseWidth = TextWidth(dc, state.linkFont, LicenseLabel());
    ReleaseDC(state.window, dc);

    const int top = Scale(state, 380);
    const int bottom = Scale(state, 414);
    state.githubRect = { Scale(state, 26), top, Scale(state, 26) + githubWidth + Scale(state, 8), bottom };
    const int licenseLeft = state.githubRect.right + Scale(state, 22);
    state.licenseRect = { licenseLeft, top, licenseLeft + licenseWidth + Scale(state, 8), bottom };
}

int HitTestLink(const AboutState& state, POINT point)
{
    if (PtInRect(&state.githubRect, point)) return kGitHubLinkId;
    if (PtInRect(&state.licenseRect, point)) return kLicenseLinkId;
    return 0;
}

void OpenLink(const AboutState& state, int link)
{
    const wchar_t* url = link == kGitHubLinkId ? kProjectUrl : (link == kLicenseLinkId ? kLicenseUrl : nullptr);
    if (!url) return;
    ShellExecuteW(state.window, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void DrawCloseButton(AboutState& state, const DRAWITEMSTRUCT& item)
{
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    COLORREF fill = pressed ? Blend(state.colors.elevated, state.colors.accent, 70) : state.colors.elevated;
    COLORREF border = focused ? state.colors.accent : state.colors.border;
    FillRoundedRect(item.hDC, item.rcItem, Scale(state, 8), fill, border, focused ? Scale(state, 2) : 1);
    DrawTextLine(item.hDC, state.bodyFont, state.colors.text, Loc("action.close", L"Close"), item.rcItem,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawInfoRow(HDC dc, const AboutState& state, int top, int bottom,
    const std::wstring& label, const std::wstring& value)
{
    DrawTextLine(dc, state.smallFont, state.colors.muted, label,
        ScaledRect(state, 44, top, 250, bottom), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextLine(dc, state.valueFont, state.colors.text, value,
        ScaledRect(state, 250, top, 516, bottom), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void DrawLink(HDC dc, const AboutState& state, const RECT& rect, const std::wstring& label, bool hovered)
{
    const COLORREF color = hovered ? state.colors.accent : state.colors.link;
    DrawTextLine(dc, state.linkFont, color, label, rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!hovered) return;

    const int width = TextWidth(dc, state.linkFont, label);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ previousPen = SelectObject(dc, pen);
    const int underlineY = rect.bottom - Scale(state, 7);
    MoveToEx(dc, rect.left, underlineY, nullptr);
    LineTo(dc, rect.left + width, underlineY);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

void PaintDialog(AboutState& state, HDC target, const RECT& client)
{
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    const HGDIOBJ previousBitmap = SelectObject(buffer, bitmap);

    HBRUSH background = CreateSolidBrush(state.colors.background);
    FillRect(buffer, &client, background);
    DeleteObject(background);

    const RECT hero = ScaledRect(state, 0, 0, kDialogWidth, 168);
    FillVerticalGradient(buffer, hero, state.colors.heroTop, state.colors.heroBottom);

    const RECT accentBar = ScaledRect(state, 0, 0, kDialogWidth, 3);
    HBRUSH accentBrush = CreateSolidBrush(state.colors.accent);
    FillRect(buffer, &accentBar, accentBrush);
    DeleteObject(accentBrush);

    const RECT heroRule = ScaledRect(state, 0, 166, kDialogWidth, 168);
    HBRUSH ruleBrush = CreateSolidBrush(state.colors.separator);
    FillRect(buffer, &heroRule, ruleBrush);
    DeleteObject(ruleBrush);

    const RECT iconTile = ScaledRect(state, 26, 30, 94, 98);
    FillRoundedRect(buffer, iconTile, Scale(state, 15), state.colors.surface, state.colors.border);
    if (state.icon)
    {
        const int iconSize = Scale(state, 52);
        const int iconX = Scale(state, 34);
        const int iconY = Scale(state, 38);
        DrawIconEx(buffer, iconX, iconY, state.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    }

    DrawTextLine(buffer, state.nameFont, state.colors.text, L"Preview 3D",
        ScaledRect(state, 110, 24, 534, 62), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const std::wstring version = Loc("about.versionLabel", L"Version") + L" " + PREVIEW3D_VERSION;
    const int pillTextWidth = TextWidth(buffer, state.pillFont, version);
    const RECT pill{ Scale(state, 110), Scale(state, 66),
        Scale(state, 110) + pillTextWidth + Scale(state, 22), Scale(state, 88) };
    FillRoundedRect(buffer, pill, Scale(state, 11),
        Blend(state.colors.surface, state.colors.accent, 60),
        Blend(state.colors.border, state.colors.accent, 130));
    DrawTextLine(buffer, state.pillFont, state.colors.text, version, pill,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawTextLine(buffer, state.bodyFont, state.colors.secondary,
        Loc("about.tagline", L"A native 3D model viewer for Windows"),
        ScaledRect(state, 110, 96, 534, 122), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextLine(buffer, state.bodyFont, state.colors.secondary,
        Loc("about.summary", L"A fast, native viewer for glTF, OBJ, FBX, STL, PLY, 3MF, USD, and STEP models. Nothing is uploaded \u2014 your files stay on this PC."),
        ScaledRect(state, 26, 182, 534, 238), DT_LEFT | DT_TOP | DT_WORDBREAK);

    const RECT card = ScaledRect(state, 26, 246, 534, 330);
    FillRoundedRect(buffer, card, Scale(state, 12), state.colors.surface, state.colors.border);

    DrawInfoRow(buffer, state, 246, 288, Loc("about.licenseLabel", L"License"), L"Apache License 2.0");
    DrawInfoRow(buffer, state, 288, 330, Loc("about.copyrightLabel", L"Copyright"), L"\u00A9 2026 Binbuf");

    HPEN divider = CreatePen(PS_SOLID, 1, state.colors.separator);
    const HGDIOBJ previousPen = SelectObject(buffer, divider);
    MoveToEx(buffer, Scale(state, 44), Scale(state, 288), nullptr);
    LineTo(buffer, Scale(state, 516), Scale(state, 288));
    MoveToEx(buffer, Scale(state, 26), Scale(state, 364), nullptr);
    LineTo(buffer, Scale(state, 534), Scale(state, 364));
    SelectObject(buffer, previousPen);
    DeleteObject(divider);

    DrawLink(buffer, state, state.githubRect, GitHubLabel(), state.hoveredLink == kGitHubLinkId);
    DrawLink(buffer, state, state.licenseRect, LicenseLabel(), state.hoveredLink == kLicenseLinkId);

    BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void LayoutCloseButton(AboutState& state)
{
    if (!state.closeButton) return;
    SetWindowPos(state.closeButton, nullptr, Scale(state, 432), Scale(state, 378),
        Scale(state, 102), Scale(state, 34), SWP_NOZORDER | SWP_NOACTIVATE);
}

void UpdateHover(AboutState& state, POINT point)
{
    const int hovered = HitTestLink(state, point);
    if (hovered == state.hoveredLink) return;
    state.hoveredLink = hovered;
    if (hovered)
    {
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, state.window, 0 };
        if (!state.trackingMouse && TrackMouseEvent(&track)) state.trackingMouse = true;
    }
    InvalidateRect(state.window, nullptr, FALSE);
}

LRESULT CALLBACK AboutWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<AboutState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<AboutState*>(create->lpCreateParams);
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
        const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
        LoadAppIcon(*state, instance);
        if (state->icon)
        {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(state->icon));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(state->icon));
        }
        state->closeButton = CreateWindowExW(0, L"BUTTON", Loc("action.close", L"Close").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)),
            instance, nullptr);
        if (state->closeButton)
        {
            SendMessageW(state->closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(state->bodyFont), TRUE);
            LayoutCloseButton(*state);
        }

        const BOOL dark = state->highContrast ? FALSE : TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const DWORD cornerPreference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(window, 33, &cornerPreference, sizeof(cornerPreference));
        LayoutLinks(*state);
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
    case WM_MOUSEMOVE:
        UpdateHover(*state, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_MOUSELEAVE:
        state->trackingMouse = false;
        if (state->hoveredLink != 0)
        {
            state->hoveredLink = 0;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        if (HitTestLink(*state, point))
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONUP:
    {
        const int link = HitTestLink(*state, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        if (link) OpenLink(*state, link);
        return 0;
    }
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
        const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
        LoadAppIcon(*state, instance);
        if (state->icon)
        {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(state->icon));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(state->icon));
        }
        LayoutCloseButton(*state);
        LayoutLinks(*state);
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
        if (state->icon && state->iconOwned) DestroyIcon(state->icon);
        state->icon = nullptr;
        state->iconOwned = false;
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    if (GetClassInfoExW(instance, kAboutWindowClass, &windowClass)) return true;

    windowClass = { sizeof(windowClass) };
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = AboutWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_PREVIEW3D), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kAboutWindowClass;
    return RegisterClassExW(&windowClass) != 0;
}
}

void ShowAboutDialog(HWND owner)
{
    if (!owner || !IsWindow(owner)) return;
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!EnsureWindowClass(instance)) return;

    AboutState state;
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

    HWND dialog = CreateWindowExW(extendedStyle, kAboutWindowClass, Loc("about.title", L"About Preview 3D").c_str(),
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
