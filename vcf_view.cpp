// vcf_view.cpp - VCF Lister view with left list + right EDIT (multiline, read-only)
// Dark/Light theme (Auto by default, INI override), context menu "Copy" on right click
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

// ===================== THEME (Auto + INI override) =====================
static std::wstring g_iniPath;      // устанавливается через VCFView_SetIniPath(...)
static bool g_dark = false;         // текущий режим

// Палитра
static HBRUSH   g_hbrBk = nullptr;
static COLORREF g_clrBk, g_clrTxt, g_clrSub, g_clrGrid, g_clrSeparator;
static COLORREF g_clrListBg, g_clrListSel;

static void SafeDelBrush(HBRUSH& b) { if (b) { DeleteObject(b); b = nullptr; } }

// чтение DWORD из реестра
static bool ReadRegDWORD(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& out) {
    HKEY h; if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = sizeof(DWORD);
    LONG r = RegGetValueW(h, nullptr, name, RRF_RT_REG_DWORD, &type, &out, &size);
    RegCloseKey(h); return r == ERROR_SUCCESS;
}
// --- Subclass for right EDIT to forward ESC to Lister ---
static WNDPROC g_EditOldProc = nullptr;

static LRESULT CALLBACK EditSubclassProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CHAR:
        if (wParam == VK_ESCAPE) {
            // Перешлём Esc родителю Lister (родитель нашего viewer-а)
            HWND viewer = GetParent(hEdit);
            HWND lister = viewer ? GetParent(viewer) : nullptr;
            if (lister) {
                PostMessageW(lister, WM_KEYDOWN, VK_ESCAPE, 0);
                return 0; // съедаем, чтобы EDIT не обрабатывал
            }
        }
        break;
    }
    return CallWindowProcW(g_EditOldProc, hEdit, msg, wParam, lParam);
}


// системная тема: true = Dark, false = Light
static bool DetectSystemDark() {
    DWORD v = 1; // 1 = Light, 0 = Dark
    if (ReadRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", v))
        return v == 0;
    return false;
}

// INI override: [VCFLister] Dark = 0/1/2 (0=Light,1=Dark,2=Auto)
static int ReadIniDarkMode() {
    if (g_iniPath.empty()) return 2;
    wchar_t buf[16]{};
    GetPrivateProfileStringW(L"VCFLister", L"Dark", L"2", buf, 16, g_iniPath.c_str());
    if (buf[0] == L'0') return 0;
    if (buf[0] == L'1') return 1;
    return 2; // Auto
}

static void RecomputeTheme() {
    const int ini = ReadIniDarkMode();
    const bool sysDark = DetectSystemDark();
    g_dark = (ini == 2) ? sysDark : (ini == 1);

    if (g_dark) {
        g_clrBk = RGB(32, 32, 32);
        g_clrTxt = RGB(220, 220, 220);
        g_clrSub = RGB(160, 160, 160);
        g_clrGrid = RGB(64, 64, 64);
        g_clrSeparator = RGB(70, 70, 70);

        g_clrListBg = RGB(28, 28, 28);
        g_clrListSel = RGB(60, 80, 120);
    }
    else {
        g_clrBk = RGB(255, 255, 255);
        g_clrTxt = RGB(30, 30, 30);
        g_clrSub = RGB(110, 110, 110);
        g_clrGrid = RGB(220, 220, 220);
        g_clrSeparator = RGB(200, 200, 200);

        g_clrListBg = RGB(248, 248, 248);
        g_clrListSel = RGB(219, 234, 254);
    }
    SafeDelBrush(g_hbrBk);
    g_hbrBk = CreateSolidBrush(g_clrBk);
}

// Внешний сеттер пути к INI (зовите из ListSetDefaultParams)
extern "C" void VCFView_SetIniPath(const wchar_t* iniPath) {
    if (iniPath && *iniPath) g_iniPath = iniPath;
    RecomputeTheme();
}
// Внешний «освежитель» темы, если нужно вручную
extern "C" void VCFView_RefreshTheme(HWND hWnd) {
    RecomputeTheme();
    if (IsWindow(hWnd)) InvalidateRect(hWnd, nullptr, TRUE);
}

// ===================== Unicode lower-case helper =====================
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
    f.hNorm = MakeFont(h, 14, FW_NORMAL, L"Segoe UI");
    f.hSmall = MakeFont(h, 12, FW_NORMAL, L"Segoe UI");
}

static void FreeFonts(Fonts& f) {
    if (f.hNorm)  DeleteObject(f.hNorm);
    if (f.hSmall) DeleteObject(f.hSmall);
    f = {};
}

// ---------- hit testing legacy (не нужен для EDIT), оставим пустым ----------
struct FieldHit { RECT rc{}; std::wstring label; std::wstring value; };

// ---------- State ----------
struct ViewState {
    std::vector<Contact> contacts;
    size_t sel = 0;

    int listScroll = 0;
    int listItemH = 0;
    RECT listRc{};
    int  perPage = 1;

    HWND hScroll = nullptr;
    HWND hEdit = nullptr; // правая панель: EDIT (multiline, read-only)

    std::vector<FieldHit> fields; // не используется, оставлено для совместимости

    Fonts fonts;
};

// ---------- Basic utils ----------
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

// ---------- Build details text for EDIT ----------
static void AppendLine(std::wstring& out, const std::wstring& k, const std::wstring& v) {
    if (v.empty()) return;
    out += k; out += L": "; out += v; out += L"\r\n";
}
static std::wstring BuildContactText(const Contact& c) {
    std::wstring t;

    // Name
    std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
    if (name.empty()) name = L"(no name)";
    AppendLine(t, L"Name", name);

    AppendLine(t, L"Org", c.org);
    AppendLine(t, L"Role", c.title);
    AppendLine(t, L"URL", c.url);
    AppendLine(t, L"BDay", c.bday);

    // Phones
    for (auto& p : c.phones) {
        if (p.number.empty()) continue;
        if (!p.types.empty()) {
            std::wstring types;
            for (size_t i = 0; i < p.types.size(); ++i) { if (i) types += L","; types += p.types[i]; }
            AppendLine(t, L"Phone (" + types + L")", p.number);
        }
        else {
            AppendLine(t, L"Phone", p.number);
        }
    }

    // Emails (with fallback)
    bool anyEmail = false;
    for (auto& e : c.emails) {
        if (e.addr.empty()) continue;
        anyEmail = true;
        if (!e.types.empty()) {
            std::wstring types;
            for (size_t i = 0; i < e.types.size(); ++i) { if (i) types += L","; types += e.types[i]; }
            AppendLine(t, L"Email (" + types + L")", e.addr);
        }
        else {
            AppendLine(t, L"Email", e.addr);
        }
    }
    if (!anyEmail) {
        std::wstring fb = FallbackEmail_NotesAware(c);
        if (!fb.empty()) AppendLine(t, L"Email", fb);
    }

    // Addresses
    for (auto& a : c.addrs) {
        if (a.text.empty()) continue;
        AppendLine(t, L"Address", a.text);
    }

    // Notes
    bool printedAnyNote = false;
    if constexpr (detail_detect::has_notes<Contact>::value) {
        if (!c.notes.empty()) {
            for (size_t i = 0; i < c.notes.size(); ++i) {
                std::wstring lab = (i == 0) ? L"Note" : (L"Note #" + std::to_wstring(i + 1));
                AppendLine(t, lab, c.notes[i]);
            }
            printedAnyNote = true;
        }
    }
    if (!printedAnyNote && !c.note.empty()) {
        AppendLine(t, L"Note", c.note);
    }

    // Android customs
    if constexpr (detail_detect::has_android<Contact>::value) {
        if (!c.androidCustoms.empty()) {
            for (const auto& ac : c.androidCustoms) {
                if (!ac.rawType.empty()) AppendLine(t, L"Android type", ac.rawType);
                for (size_t i = 0; i < ac.slots.size(); ++i) {
                    if (ac.slots[i].empty()) continue;
                    std::wstring lab = ac.slots.size() > 1 ? (L"Android slot #" + std::to_wstring(i + 1)) : L"Android";
                    AppendLine(t, lab, ac.slots[i]);
                }
            }
        }
    }

    return t;
}

// ---------- clipboard (для «копировать всё», если нет выделения) ----------
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

    HBRUSH bg = CreateSolidBrush(g_clrListBg);
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
        HBRUSH ibg = CreateSolidBrush(idx == st->sel ? g_clrListSel : g_clrListBg);
        FillRect(dc, &item, ibg); DeleteObject(ibg);

        HPEN pen = CreatePen(PS_SOLID, 1, g_clrGrid);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, item.left, item.bottom, nullptr);
        LineTo(dc, item.right, item.bottom);
        SelectObject(dc, oldPen); DeleteObject(pen);

        HFONT old = (HFONT)SelectObject(dc, st->fonts.hNorm);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_clrTxt);
        RECT nameRc = item; nameRc.left += S(h, 8); nameRc.top += S(h, 6); nameRc.right -= S(h, 6);
        DrawTextW(dc, name.c_str(), (int)name.size(), &nameRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, st->fonts.hSmall);
        SetTextColor(dc, g_clrSub);
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

// ---------- EDIT content update ----------
static void UpdateRightEdit(ViewState* st) {
    if (!st || !IsWindow(st->hEdit)) return;
    std::wstring text;
    if (st->sel < st->contacts.size()) {
        text = BuildContactText(st->contacts[st->sel]);
    }
    else {
        text = L"";
    }
    // Установим текст; оставляем курсор в начале
    SendMessageW(st->hEdit, WM_SETTEXT, 0, (LPARAM)text.c_str());
    SendMessageW(st->hEdit, EM_SETSEL, 0, 0);
    SendMessageW(st->hEdit, EM_SCROLLCARET, 0, 0);
}

// ---------- selection helpers ----------
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
    UpdateRightEdit(st);
    InvalidateRect(h, nullptr, TRUE);
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

        // Тема при старте
        RecomputeTheme();

        // Скролл для списка
        st->hScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100,
            h, nullptr, GetModuleHandleW(nullptr), nullptr);

        // EDIT справа (multiline, readonly)
        st->hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0,
            h, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
        // EDIT справа (multiline, readonly)
        st->hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0,
            h, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);

        // Шрифт и цвета
        SendMessageW(st->hEdit, WM_SETFONT, (WPARAM)st->fonts.hNorm, TRUE);

        // <<< ВАЖНО: сабклассим EDIT для обработки Esc >>>
        g_EditOldProc = (WNDPROC)SetWindowLongPtrW(st->hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        // Шрифт и цвета
        SendMessageW(st->hEdit, WM_SETFONT, (WPARAM)st->fonts.hNorm, TRUE);

        SetFocus(h);
        return 0;
    }
    case WM_DESTROY: {
        if (st) {
            if (st->hEdit && IsWindow(st->hEdit)) DestroyWindow(st->hEdit);
            if (st->hScroll && IsWindow(st->hScroll)) DestroyWindow(st->hScroll);
            FreeFonts(st->fonts);
            delete st;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        SafeDelBrush(g_hbrBk);
        return 0;
    }
    case WM_SIZE: {
        if (!st) break;
        st->listItemH = 0;

        RECT rc; GetClientRect(h, &rc);
        int listW = ListPaneWidth(h);
        int sbw = ScrollbarWidth();
        int cy = HIWORD(l);

        // скролл у левой панели
        MoveWindow(st->hScroll, listW - sbw, 0, sbw, cy, TRUE);

        // EDIT: занимает всё справа
        int pad = S(h, 12);
        int ex = listW + 1 + pad;
        int ew = rc.right - ex - pad;
        int ey = pad;
        int eh = rc.bottom - ey - pad;
        if (ew < S(h, 100)) ew = std::max<int>(0, rc.right - (listW + pad));

        MoveWindow(st->hEdit, ex, ey, ew, eh, TRUE);

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
        if (PtInRect(&st->listRc, pt)) {
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
                if (idx < st->contacts.size()) { st->sel = idx; UpdateRightEdit(st); InvalidateRect(h, nullptr, TRUE); return 0; }
            }
        }
        return 0;
    }

                       // Смена темы системы/цветов
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE: {
        RecomputeTheme();
        // перекрасим EDIT (через WM_CTLCOLOREDIT) и фон
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }

                          // Контекстное меню: правый клик по EDIT → "Копировать"
    case WM_CONTEXTMENU: {
        if (!st) break;
        HWND hSrc = (HWND)w;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (hSrc == st->hEdit || (hSrc == h)) {
            // если пришло на родителя — проверим, попали ли в EDIT
            if (hSrc == h) {
                POINT cpt = pt; ScreenToClient(h, &cpt);
                RECT rcE{}; GetWindowRect(st->hEdit, &rcE);
                if (!(pt.x >= rcE.left && pt.x < rcE.right && pt.y >= rcE.top && pt.y < rcE.bottom)) break;
            }

            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"\u041A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u0442\u044C"); // "Копировать"
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            if (cmd == 1) {
                // Если есть выделение — WM_COPY, иначе копируем весь текст
                DWORD selStart = 0, selEnd = 0;
                SendMessageW(st->hEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
                if (selStart != selEnd) {
                    SendMessageW(st->hEdit, WM_COPY, 0, 0);
                }
                else {
                    int len = GetWindowTextLengthW(st->hEdit);
                    std::wstring all(len, L'\0');
                    GetWindowTextW(st->hEdit, &all[0], len + 1);
                    SetClipboardTextW(h, all);
                }
            }
            return 0;
        }
        break;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, g_clrTxt);
        SetBkColor(dc, g_clrBk);
        return (INT_PTR)(g_hbrBk ? g_hbrBk : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_hbrBk ? g_hbrBk : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        // фон
        HBRUSH wbg = g_hbrBk ? g_hbrBk : (HBRUSH)(COLOR_WINDOW + 1);
        FillRect(mem, &rc, wbg);

        // левая панель
        int listW = ListPaneWidth(h);
        RenderList(mem, h, st, rc.left, rc.top, listW, rc.bottom - rc.top, st->listRc);

        // разделитель
        HBRUSH sepBr = CreateSolidBrush(g_clrSeparator);
        RECT sep{ listW, rc.top, listW + 1, rc.bottom }; FillRect(mem, &sep, sepBr); DeleteObject(sepBr);

        // вывод
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
    UpdateRightEdit(st);
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
        UpdateRightEdit(st);
        InvalidateRect(h, nullptr, TRUE);
    }
}

// Поиск — как раньше (регистр учитывается флагом, но нормализация к lower для корректности кириллицы)
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
                st->sel = i; EnsureSelVisible(h, st); UpdateRightEdit(st); InvalidateRect(h, nullptr, TRUE);
                return true;
            }
            pos = hay.find(n, pos + 1);
        }
        i = nextIndex(i);
    } while (wrap && i != first);

    return false;
}

bool VCFView_CopyActive(HWND) { return false; } // не используется
