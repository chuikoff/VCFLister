// vcf_view.cpp - VCF Lister view with right-click "Copy" (no Ctrl+C), clean values (no type tails)
#define UNICODE
#define _UNICODE
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#pragma comment(lib, "Gdiplus.lib")

#include <vector>
#include <string>
#include <algorithm>
#include <cwctype>
#include <memory>

#include "vcf_view.hpp"

// ----- forward decls for new fields (so it compiles even before you add them in the header) -----
struct AndroidCustom { std::wstring rawType; std::vector<std::wstring> slots; };
namespace detail_detect {
    template<typename T> struct has_notes {
        template<typename U> static auto test(int) -> decltype(std::declval<U>().notes, std::true_type{});
        template<typename>  static auto test(...) -> std::false_type;
        static constexpr bool value = std::is_same<decltype(test<T>(0)), std::true_type>::value;
    };
    template<typename T> struct has_android {
        template<typename U> static auto test(int) -> decltype(std::declval<U>().androidCustoms, std::true_type{});
        template<typename>  static auto test(...) -> std::false_type;
        static constexpr bool value = std::is_same<decltype(test<T>(0)), std::true_type>::value;
    };
}

// Надёжное понижение регистра (Unicode, независимо от локали)
static std::wstring LowerInvariant(const std::wstring& s) {
    if (s.empty()) return s;
    int need = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(), -1, nullptr, 0);
    if (need <= 0) {
        std::wstring t = s;
        std::transform(t.begin(), t.end(), t.begin(), ::towlower);
        return t;
    }
    std::wstring out;
    out.resize(need);
    LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(), -1, &out[0], need);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

using namespace Gdiplus;

// ---------- DPI helpers ----------
static int Dpi(HWND h) {
    HDC dc = GetDC(h);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(h, dc);
    return dpi ? dpi : 96;
}
static int S(HWND h, int px) { return MulDiv(px, Dpi(h), 96); }

// ---------- Fonts ----------
struct Fonts {
    HFONT hTitle = nullptr;
    HFONT hBold = nullptr;
    HFONT hNorm = nullptr;
    HFONT hSmall = nullptr;
};

static HFONT MakeFont(HWND h, int px, int weight, const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfHeight = -S(h, px);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

static void MakeFonts(HWND h, Fonts& f) {
    f.hTitle = MakeFont(h, 18, FW_SEMIBOLD, L"Segoe UI");
    f.hBold = MakeFont(h, 14, FW_SEMIBOLD, L"Segoe UI");
    f.hNorm = MakeFont(h, 14, FW_NORMAL, L"Segoe UI");
    f.hSmall = MakeFont(h, 12, FW_NORMAL, L"Segoe UI");
}

static void FreeFonts(Fonts& f) {
    if (f.hTitle) DeleteObject(f.hTitle);
    if (f.hBold)  DeleteObject(f.hBold);
    if (f.hNorm)  DeleteObject(f.hNorm);
    if (f.hSmall) DeleteObject(f.hSmall);
    f = {};
}

// ---------- hit testing for right pane fields ----------
struct FieldHit {
    RECT rc{};
    std::wstring label;
    std::wstring value;
};

// ---------- State ----------
struct ViewState {
    std::vector<Contact> contacts;
    size_t sel = 0;

    int listScroll = 0;
    int listItemH = 0;
    RECT listRc{};
    int  perPage = 1;

    HWND hScroll = nullptr;

    std::vector<FieldHit> fields;
    int contextField = -1;

    Fonts fonts;
};

static void EnsureSelVisible(HWND h, ViewState* st) {
    RECT rc; GetClientRect(h, &rc);
    int innerH = rc.bottom - rc.top - S(h, 16);
    int rowH = st->listItemH ? st->listItemH : S(h, 52);
    int per = std::max(1, innerH / rowH);
    st->perPage = per;

    int sel = (int)st->sel;
    if (sel < st->listScroll) st->listScroll = sel;
    else if (sel >= st->listScroll + per) st->listScroll = sel - (per - 1);

    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max(0, (int)st->contacts.size() - per);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;
}

static void SetSelectionAndReveal(HWND h, ViewState* st, size_t idx) {
    if (!st || st->contacts.empty()) return;
    if (idx >= st->contacts.size()) idx = st->contacts.size() - 1;

    st->sel = idx;
    EnsureSelVisible(h, st);

    if (st->hScroll) {
        SCROLLINFO si{}; si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        si.nPos = st->listScroll;
        SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
    }
    st->contextField = -1;
    InvalidateRect(h, nullptr, TRUE);
}

// hit-test helper
static inline bool PtIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---------- Utils ----------
static std::wstring PrimaryPhone(const Contact& c) {
    return c.phones.empty() ? L"" : c.phones[0].number;
}
static std::wstring PrimaryEmail(const Contact& c) {
    for (auto& e : c.emails) { if (!e.addr.empty()) return e.addr; }
    return L"";
}

static bool IsEmailChar(wchar_t ch) {
    return iswalnum(ch) || ch == L'.' || ch == L'_' || ch == L'-' || ch == L'+';
}
static std::wstring ExtractEmailFromText(const std::wstring& txt) {
    size_t at = txt.find(L'@');
    if (at == std::wstring::npos) return L"";
    size_t l = at, r = at;
    while (l > 0 && IsEmailChar(txt[l - 1])) --l;
    while (r + 1 < txt.size() && IsEmailChar(txt[r + 1])) ++r;
    if (l<at && r>at) return txt.substr(l, r - l + 1);
    return L"";
}
static std::wstring FallbackEmail_NotesAware(const Contact& c) {
    if (!c.url.empty()) {
        const std::wstring m = L"mailto:";
        if (c.url.size() > m.size()) {
            std::wstring low = c.url; std::transform(low.begin(), low.end(), low.begin(), ::towlower);
            if (low.rfind(m, 0) == 0) {
                std::wstring e = c.url.substr(m.size());
                size_t q = e.find(L'?'); if (q != std::wstring::npos) e = e.substr(0, q);
                return e;
            }
        }
        std::wstring f = ExtractEmailFromText(c.url);
        if (!f.empty()) return f;
    }
    if (!c.note.empty()) {
        std::wstring f = ExtractEmailFromText(c.note);
        if (!f.empty()) return f;
    }
    // если у контакта есть vector<notes> — ищем и там
    if constexpr (detail_detect::has_notes<Contact>::value) {
        if (!c.notes.empty()) {
            for (const auto& n : c.notes) {
                std::wstring f = ExtractEmailFromText(n);
                if (!f.empty()) return f;
            }
        }
    }
    return L"";
}

// ---------- clipboard ----------
static void SetClipboardTextW(HWND h, const std::wstring& text) {
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hmem) {
        void* p = GlobalLock(hmem);
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(hmem);
        SetClipboardData(CF_UNICODETEXT, hmem);
    }
    CloseClipboard();
}

// ---------- GDI+ helper ----------
static std::unique_ptr<Image> ImageFromBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hMem) return {};
    void* p = GlobalLock(hMem);
    memcpy(p, bytes.data(), bytes.size());
    GlobalUnlock(hMem);
    IStream* stm = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stm) != S_OK) { GlobalFree(hMem); return {}; }
    auto img = std::make_unique<Image>(stm);
    stm->Release();
    if (img->GetLastStatus() != Ok) return {};
    return img;
}

// ---------- text drawing helpers ----------
static int DrawMeasuredText(HDC dc, HFONT f, const std::wstring& txt, RECT rc, UINT fmt, COLORREF col) {
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);

    RECT calc = rc;
    DrawTextW(dc, txt.c_str(), (int)txt.size(), &calc, fmt | DT_CALCRECT);

    RECT draw = rc;
    draw.bottom = rc.top + (calc.bottom - calc.top);
    DrawTextW(dc, txt.c_str(), (int)txt.size(), &draw, fmt);

    SelectObject(dc, old);
    return (calc.bottom - calc.top);
}
static void DrawLabel(HDC dc, HFONT f, int x, int y, const std::wstring& label, COLORREF labCol) {
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, labCol);
    TextOutW(dc, x, y, label.c_str(), (int)label.size());
    SelectObject(dc, old);
}
static void DrawNameField(HDC dc, Fonts& f, HWND h, int x, int& y, int w, const std::wstring& name) {
    DrawLabel(dc, f.hBold, x, y, L"Name:", RGB(90, 90, 90));
    y += S(h, 22);
    RECT rc{ x, y, x + w, y + 10000 };
    int used = DrawMeasuredText(dc, f.hTitle, name, rc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX, RGB(20, 20, 20));
    y += used + S(h, 10);
}
// Нарисовать пару "Label: value" и вернуть прямоугольник value
static RECT DrawLabelValueWithRect(HDC dc, Fonts& f, int x, int& y, int w,
    const std::wstring& label, const std::wstring& value,
    COLORREF labCol, COLORREF valCol,
    HWND hWnd)
{
    RECT empty{ 0,0,0,0 };
    if (value.empty()) return empty;

    std::wstring lab = label + L": ";
    HFONT old = (HFONT)SelectObject(dc, f.hBold);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, labCol);
    TextOutW(dc, x, y, lab.c_str(), (int)lab.size());

    SIZE szLab{}; GetTextExtentPoint32W(dc, lab.c_str(), (int)lab.size(), &szLab);

    RECT rcVal{ x + szLab.cx, y, x + w, y + 10000 };
    SelectObject(dc, f.hNorm);
    int hUsed = DrawMeasuredText(dc, f.hNorm, value, rcVal, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX, valCol);

    y += std::max(S(hWnd, 22), hUsed) + S(hWnd, 6);
    SelectObject(dc, old);

    RECT calc = rcVal;
    HFONT oldN = (HFONT)SelectObject(dc, f.hNorm);
    DrawTextW(dc, value.c_str(), (int)value.size(), &calc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, oldN);
    return calc;
}

// ---------- left list ----------
static void EnsureListMetrics(HWND h, ViewState* st) {
    if (!st->listItemH) st->listItemH = S(h, 52);
}
static void UpdateListScrollbar(ViewState* st, int total) {
    if (!st->hScroll) return;
    SCROLLINFO si{}; si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, total - 1);
    si.nPage = std::max(1, st->perPage);
    si.nPos = std::min(st->listScroll, std::max(0, total - st->perPage));
    SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
    ShowWindow(st->hScroll, (total > st->perPage) ? SW_SHOW : SW_HIDE);
}
static int ListPaneWidth(HWND h) { return S(h, 260); }
static int ScrollbarWidth() { return GetSystemMetrics(SM_CXVSCROLL); }

static void RenderList(HDC dc, HWND h, ViewState* st,
    int x, int y, int w, int hgt, RECT& outListRc)
{
    EnsureListMetrics(h, st);

    int sbw = ScrollbarWidth();
    int wList = w - sbw; if (wList < S(h, 120)) wList = w;

    HBRUSH bg = CreateSolidBrush(RGB(248, 248, 248));
    RECT rbg{ x,y,x + wList,y + hgt }; FillRect(dc, &rbg, bg); DeleteObject(bg);

    int pad = S(h, 8);
    int innerTop = y + pad;
    int innerH = hgt - pad - pad;
    st->perPage = std::max(1, innerH / st->listItemH);

    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max(0, (int)st->contacts.size() - st->perPage);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;

    int ycur = innerTop;
    for (int row = 0; row < st->perPage && st->listScroll + row < (int)st->contacts.size(); ++row) {
        size_t idx = (size_t)(st->listScroll + row);
        const Contact& c = st->contacts[idx];

        std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
        if (name.empty()) name = L"(no name)";

        std::wstring sub;
        std::wstring pv = PrimaryPhone(c);
        if (!pv.empty()) sub = L"Tel: " + pv;
        else {
            std::wstring em = PrimaryEmail(c);
            if (!em.empty()) sub = L"Email: " + em;
        }

        RECT item{ x + pad, ycur, x + wList - pad, ycur + st->listItemH - S(h,2) };
        HBRUSH ibg = CreateSolidBrush(idx == st->sel ? RGB(219, 234, 254) : RGB(248, 248, 248));
        FillRect(dc, &item, ibg); DeleteObject(ibg);

        HPEN pen = CreatePen(PS_SOLID, 1, RGB(230, 230, 230));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, item.left, item.bottom, nullptr);
        LineTo(dc, item.right, item.bottom);
        SelectObject(dc, oldPen); DeleteObject(pen);

        HFONT old = (HFONT)SelectObject(dc, st->fonts.hNorm);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(30, 30, 30));
        RECT nameRc = item; nameRc.left += S(h, 8); nameRc.top += S(h, 6); nameRc.right -= S(h, 6);
        DrawTextW(dc, name.c_str(), (int)name.size(), &nameRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, st->fonts.hSmall);
        SetTextColor(dc, RGB(110, 110, 110));
        std::wstring subShow = sub;
        if (subShow.empty()) {
            std::wstring fb = FallbackEmail_NotesAware(c);
            if (!fb.empty()) subShow = L"Email: " + fb;
        }
        RECT subRc = nameRc; subRc.top = nameRc.top + S(h, 20);
        DrawTextW(dc, subShow.c_str(), (int)subShow.size(), &subRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        ycur += st->listItemH;
    }

    outListRc = RECT{ x, y, x + wList, y + hgt };
    UpdateListScrollbar(st, (int)st->contacts.size());
}

// ---------- Window procedure ----------
static const wchar_t* kClass = L"VCF_VIEW_CLASS";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ViewState* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_CREATE: {
        st = new ViewState();
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        MakeFonts(h, st->fonts);

        static ULONG_PTR gdipToken = 0;
        if (!gdipToken) { GdiplusStartupInput gi; GdiplusStartup(&gdipToken, &gi, nullptr); }

        st->hScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100,
            h, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetFocus(h);
        return 0;
    }
    case WM_DESTROY: {
        if (st) {
            if (st->hScroll && IsWindow(st->hScroll)) DestroyWindow(st->hScroll);
            FreeFonts(st->fonts);
            delete st;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        return 0;
    }
    case WM_SIZE: {
        if (!st) break;
        st->listItemH = 0;
        int listW = ListPaneWidth(h);
        int sbw = ScrollbarWidth();
        int cy = HIWORD(l);
        MoveWindow(st->hScroll, listW - sbw, 0, sbw, cy, TRUE);
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_VSCROLL: {
        if (!st) break;
        if ((HWND)l == st->hScroll) {
            int total = (int)st->contacts.size();
            if (total <= 0) return 0;

            int maxScroll = std::max(0, total - st->perPage);
            int pos = st->listScroll;
            switch (LOWORD(w)) {
            case SB_LINEUP:   pos -= 1; break;
            case SB_LINEDOWN: pos += 1; break;
            case SB_PAGEUP:   pos -= std::max(1, st->perPage - 1); break;
            case SB_PAGEDOWN: pos += std::max(1, st->perPage - 1); break;
            case SB_TOP:      pos = 0; break;
            case SB_BOTTOM:   pos = maxScroll; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(st->hScroll, SB_CTL, &si);
                pos = si.nTrackPos;
                break;
            }
            default: break;
            }
            pos = std::max(0, std::min(maxScroll, pos));
            if (pos != st->listScroll) { st->listScroll = pos; InvalidateRect(h, nullptr, TRUE); }
            SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = st->listScroll;
            SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (!st) break;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &pt);
        if (PtIn(st->listRc, pt.x, pt.y)) {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            int step = (delta > 0) ? -1 : +1;
            int maxScroll = std::max(0, (int)st->contacts.size() - st->perPage);
            st->listScroll = std::max(0, std::min(maxScroll, st->listScroll + step));
            SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = st->listScroll;
            SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!st || st->contacts.empty()) return 0;
        size_t sel = st->sel, count = st->contacts.size();
        bool handled = false;
        switch (w) {
        case VK_UP:    if (sel > 0) sel--, handled = true; break;
        case VK_DOWN:  if (sel + 1 < count) sel++, handled = true; break;
        case VK_PRIOR: if (st->perPage > 0) sel = (sel > (size_t)st->perPage) ? (sel - (size_t)st->perPage) : 0, handled = true; break;
        case VK_NEXT:  if (st->perPage > 0) sel = std::min(count - 1, sel + (size_t)st->perPage), handled = true; break;
        case VK_HOME:  sel = 0; handled = true; break;
        case VK_END:   sel = count ? count - 1 : 0; handled = true; break;
        default: break;
        }
        if (handled) { SetSelectionAndReveal(h, st, sel); return 0; }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int pad = S(h, 8);
        int listW = ListPaneWidth(h) - ScrollbarWidth();
        if (x >= pad && x < listW - pad) {
            int rowH = st->listItemH ? st->listItemH : S(h, 52);
            int row = (y - pad) / rowH;
            if (row >= 0) {
                size_t idx = (size_t)(st->listScroll + row);
                if (idx < st->contacts.size()) { st->sel = idx; InvalidateRect(h, nullptr, TRUE); return 0; }
            }
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        st->contextField = -1;
        for (int i = (int)st->fields.size() - 1; i >= 0; --i) {
            if (PtIn(st->fields[i].rc, x, y)) { st->contextField = i; break; }
        }
        if (st->contextField >= 0) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"\u041A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u0442\u044C");
            POINT pt; pt.x = x; pt.y = y; ClientToScreen(h, &pt);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) {
                std::wstring txt = st->fields[st->contextField].value;
                if (!txt.empty()) SetClipboardTextW(h, txt);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HBRUSH wbg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(mem, &rc, wbg); DeleteObject(wbg);

        int listW = ListPaneWidth(h);
        RenderList(mem, h, st, rc.left, rc.top, listW, rc.bottom - rc.top, st->listRc);

        RECT sep{ listW, rc.top, listW + 1, rc.bottom }; FillRect(mem, &sep, (HBRUSH)GetStockObject(GRAY_BRUSH));

        st->fields.clear();

        if (st->sel < st->contacts.size()) {
            int dx = listW + S(h, 12);
            int dy = rc.top + S(h, 12);
            int dw = rc.right - dx - S(h, 12);

            const Contact& c = st->contacts[st->sel];
            std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
            if (name.empty()) name = L"(no name)";
            DrawNameField(mem, st->fonts, h, dx, dy, dw, name);

            // Photo: максимально большой, но без появления вертикальной прокрутки
            if (c.photo && !c.photo->bytes.empty()) {
                auto img = ImageFromBytes(c.photo->bytes);
                if (img) {
                    int iw = (int)img->GetWidth();
                    int ih = (int)img->GetHeight();
                    if (iw > 0 && ih > 0) {
                        int bottomPad = S(h, 24);
                        int availableH = (rc.bottom - dy) - bottomPad;
                        if (availableH < S(h, 60)) availableH = S(h, 60);
                        int maxW = dw, maxH = availableH;
                        double k = std::min(1.0, std::min((double)maxW / iw, (double)maxH / ih));
                        int drawW = (int)(iw * k), drawH = (int)(ih * k);
                        Gdiplus::Graphics g(mem);
                        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                        g.DrawImage(img.get(), dx, dy, drawW, drawH);
                        dy += drawH + S(h, 8);
                    }
                }
            }

            auto& F = st->fonts;
            auto pushField = [&](const std::wstring& lab, const std::wstring& val) {
                RECT r = DrawLabelValueWithRect(mem, F, dx, dy, dw, lab, val,
                    RGB(90, 90, 90), RGB(30, 30, 30), h);
                FieldHit fh; fh.rc = r; fh.label = lab; fh.value = val;
                st->fields.push_back(std::move(fh));
                };

            if (!c.org.empty())   pushField(L"Org", c.org);
            if (!c.title.empty()) pushField(L"Role", c.title);
            if (!c.url.empty())   pushField(L"URL", c.url);
            if (!c.bday.empty())  pushField(L"BDay", c.bday);

            for (auto& p : c.phones) if (!p.number.empty()) pushField(L"Phone", p.number);

            bool anyEmail = false;
            for (auto& e : c.emails) {
                if (e.addr.empty()) continue;
                anyEmail = true; pushField(L"Email", e.addr);
            }
            if (!anyEmail) {
                std::wstring fb = FallbackEmail_NotesAware(c);
                if (!fb.empty()) pushField(L"Email", fb);
            }

            for (auto& a : c.addrs) if (!a.text.empty()) pushField(L"Address", a.text);

            // ---- Multi-Notes ----
            bool printedAnyNote = false;
            if constexpr (detail_detect::has_notes<Contact>::value) {
                if (!c.notes.empty()) {
                    for (size_t i = 0; i < c.notes.size(); ++i) {
                        std::wstring lab = (i == 0) ? L"Note" : (L"Note #" + std::to_wstring(i + 1));
                        pushField(lab, c.notes[i]);
                    }
                    printedAnyNote = true;
                }
            }
            if (!printedAnyNote && !c.note.empty()) {
                pushField(L"Note", c.note); // fallback для старых контактов
            }

            // ---- Android custom section ----
            if constexpr (detail_detect::has_android<Contact>::value) {
                if (!c.androidCustoms.empty()) {
                    for (const auto& ac : c.androidCustoms) {
                        if (!ac.rawType.empty()) {
                            pushField(L"Android type", ac.rawType);
                        }
                        for (size_t i = 0; i < ac.slots.size(); ++i) {
                            if (ac.slots[i].empty()) continue;
                            std::wstring lab = ac.slots.size() > 1 ? (L"Android slot #" + std::to_wstring(i + 1)) : L"Android";
                            pushField(lab, ac.slots[i]);
                        }
                    }
                }
            }
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------- Public API ----------
HWND CreateVCFView(HWND parent, const std::vector<Contact>& contacts) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VCF_VIEW_CLASS"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc); reg = true;
    }
    HWND h = CreateWindowExW(0, L"VCF_VIEW_CLASS", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h) VCFView_SetContacts(h, contacts);
    return h;
}

void VCFView_SetContacts(HWND h, const std::vector<Contact>& contacts) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st) return;
    st->contacts = contacts;
    st->sel = 0;
    st->listScroll = 0;
    st->contextField = -1;
    InvalidateRect(h, nullptr, TRUE);
}

size_t VCFView_Count(HWND h) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return st ? st->contacts.size() : 0;
}
size_t VCFView_GetSelection(HWND h) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return st ? st->sel : 0;
}
void VCFView_SetSelection(HWND h, size_t idx) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st) return;
    if (idx < st->contacts.size()) {
        st->sel = idx;
        EnsureSelVisible(h, st);
        st->contextField = -1;
        InvalidateRect(h, nullptr, TRUE);
    }
}

// Поиск — без изменений, но учитывает LowerInvariant в builder
static bool isWordBoundary(const std::wstring& s, size_t pos) { return (pos == 0) || !iswalnum(s[pos - 1]); }
static bool isWordBoundary2(const std::wstring& s, size_t pos) { return (pos >= s.size()) || !iswalnum(s[pos]); }

bool VCFView_SearchEx(HWND h, const std::wstring& needle,
    size_t startIndex, bool backwards,
    bool /*matchCase*/, bool wholeWord, bool wrap)
{
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st || st->contacts.empty() || needle.empty()) return false;

    auto norm = [&](const std::wstring& x) { return LowerInvariant(x); };
    std::wstring n = norm(needle);

    auto buildHay = [&](const Contact& c) {
        std::wstring hstr;
        auto add = [&](const std::wstring& s) { if (!s.empty()) { hstr += L" "; hstr += norm(s); } };
        add(c.fn); add(c.n_given); add(c.n_family); add(c.org); add(c.title); add(c.bday); add(c.url); add(c.note);
        if constexpr (detail_detect::has_notes<Contact>::value) {
            for (auto& t : c.notes) add(t);
        }
        for (auto& t : c.phones) { add(t.number); for (auto& tp : t.types) add(tp); }
        for (auto& e : c.emails) { add(e.addr);   for (auto& tp : e.types) add(tp); }
        for (auto& a : c.addrs) { add(a.text); }
        return hstr;
        };

    const size_t count = st->contacts.size();
    auto nextIndex = [&](size_t i)->size_t { return backwards ? (i == 0 ? count - 1 : i - 1) : (i + 1 == count ? 0 : i + 1); };

    size_t i = startIndex % count, first = i;
    do {
        std::wstring hay = buildHay(st->contacts[i]);
        size_t pos = hay.find(n);
        while (pos != std::wstring::npos) {
            if (!wholeWord || (isWordBoundary(hay, pos) && isWordBoundary2(hay, pos + n.size()))) {
                st->sel = i; EnsureSelVisible(h, st); InvalidateRect(h, nullptr, TRUE);
                return true;
            }
            pos = hay.find(n, pos + 1);
        }
        i = nextIndex(i);
    } while (wrap && i != first);

    return false;
}

bool VCFView_CopyActive(HWND) { return false; } // не используется
