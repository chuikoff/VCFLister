// vcf_view.cpp - VCF Lister view (non-clickable, proper layout, left-pane scrollbar, email fallback)
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
    HFONT hTitle = nullptr; // big value
    HFONT hBold = nullptr; // field labels
    HFONT hNorm = nullptr; // normal text
    HFONT hSmall = nullptr; // list subtitle
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

// ---------- State ----------
struct ViewState {
    std::vector<Contact> contacts;
    size_t sel = 0;

    int listScroll = 0;
    int listItemH = 0; // 2 строки + паддинги
    RECT listRc{};
    int  perPage = 1;

    HWND hScroll = nullptr; // отдельный контрол прокрутки слева

    Fonts fonts;
};

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

// извлекаем e-mail из mailto: или из текста (простой поиск вокруг '@')
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
static std::wstring FallbackEmail(const Contact& c) {
    // 1) mailto: в URL
    if (!c.url.empty()) {
        const std::wstring m = L"mailto:";
        if (c.url.size() > m.size()) {
            std::wstring low = c.url; std::transform(low.begin(), low.end(), low.begin(), ::towlower);
            if (low.rfind(m, 0) == 0) {
                std::wstring e = c.url.substr(m.size());
                // обрежем параметры после '?'
                size_t q = e.find(L'?'); if (q != std::wstring::npos) e = e.substr(0, q);
                return e;
            }
        }
        // иначе попробуем вытащить что-то похожее на e-mail прямо из url
        std::wstring f = ExtractEmailFromText(c.url);
        if (!f.empty()) return f;
    }
    // 2) из заметки
    if (!c.note.empty()) {
        std::wstring f = ExtractEmailFromText(c.note);
        if (!f.empty()) return f;
    }
    return L"";
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
    // Label
    DrawLabel(dc, f.hBold, x, y, L"Name:", RGB(90, 90, 90));
    // Нормальный отступ под значение
    y += S(h, 22);

    // Value
    RECT rc{ x, y, x + w, y + 10000 };
    int used = DrawMeasuredText(dc, f.hTitle, name, rc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX, RGB(20, 20, 20));
    y += used + S(h, 10);
}

static void DrawLabelValue(HDC dc, Fonts& f, int x, int& y, int w,
    const std::wstring& label, const std::wstring& value,
    COLORREF labCol, COLORREF valCol,
    HWND hWnd)
{
    if (value.empty()) return;

    // label + ": "
    std::wstring lab = label + L": ";
    HFONT old = (HFONT)SelectObject(dc, f.hBold);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, labCol);
    TextOutW(dc, x, y, lab.c_str(), (int)lab.size());

    // точная ширина метки
    SIZE szLab{}; GetTextExtentPoint32W(dc, lab.c_str(), (int)lab.size(), &szLab);

    // value с переносами
    RECT rcVal{ x + szLab.cx, y, x + w, y + 10000 };
    SelectObject(dc, f.hNorm);
    int hUsed = DrawMeasuredText(dc, f.hNorm, value, rcVal, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX, valCol);

    y += std::max(S(hWnd, 22), hUsed) + S(hWnd, 6);
    SelectObject(dc, old);
}

// ---------- left list (2 lines + inside scrollbar control) ----------
static void EnsureListMetrics(HWND h, ViewState* st) {
    if (!st->listItemH) {
        st->listItemH = S(h, 52); // name + subtitle
    }
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
    ShowWindow(st->hScroll, (total > st->perPage) ? SW_SHOW : SW_HIDE); // можно поставить SW_SHOW всегда
}

static int ListPaneWidth(HWND h) { return S(h, 260); }
static int ScrollbarWidth() { return GetSystemMetrics(SM_CXVSCROLL); }

static void RenderList(HDC dc, HWND h, ViewState* st,
    int x, int y, int w, int hgt, RECT& outListRc)
{
    EnsureListMetrics(h, st);

    int sbw = ScrollbarWidth();
    int wList = w - sbw; if (wList < S(h, 120)) wList = w; // safety

    HBRUSH bg = CreateSolidBrush(RGB(248, 248, 248));
    RECT rbg{ x,y,x + wList,y + hgt }; FillRect(dc, &rbg, bg); DeleteObject(bg);

    int pad = S(h, 8);
    int innerTop = y + pad;
    int innerH = hgt - pad - pad;
    st->perPage = std::max(1, innerH / st->listItemH);

    // clamp scroll
    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max(0, (int)st->contacts.size() - st->perPage);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;

    // draw items
    int ycur = innerTop;
    for (int row = 0; row < st->perPage && st->listScroll + row < (int)st->contacts.size(); ++row) {
        size_t idx = (size_t)(st->listScroll + row);
        const Contact& c = st->contacts[idx];

        std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
        if (name.empty()) name = L"(no name)";

        std::wstring pv = PrimaryPhone(c);
        std::wstring subLabel = !pv.empty() ? L"Tel: " : L"Email: ";
        if (pv.empty()) pv = PrimaryEmail(c);
        std::wstring sub = subLabel + pv;

        RECT item{ x + pad, ycur, x + wList - pad, ycur + st->listItemH - S(h,2) };
        HBRUSH ibg = CreateSolidBrush(idx == st->sel ? RGB(219, 234, 254) : RGB(248, 248, 248));
        FillRect(dc, &item, ibg); DeleteObject(ibg);

        // separator
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(230, 230, 230));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, item.left, item.bottom, nullptr);
        LineTo(dc, item.right, item.bottom);
        SelectObject(dc, oldPen); DeleteObject(pen);

        // name (single line)
        HFONT old = (HFONT)SelectObject(dc, st->fonts.hNorm);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(30, 30, 30));
        RECT nameRc = item; nameRc.left += S(h, 8); nameRc.top += S(h, 6); nameRc.right -= S(h, 6);
        DrawTextW(dc, name.c_str(), (int)name.size(), &nameRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        // subtitle
        SelectObject(dc, st->fonts.hSmall);
        SetTextColor(dc, RGB(110, 110, 110));
        std::wstring subShow = sub;
        if (subShow.empty()) {
            // Левый подзаголовок тоже пытается показать email из fallback
            std::wstring fb = FallbackEmail(c);
            if (!fb.empty()) subShow = L"Email: " + fb;
        }
        RECT subRc = nameRc; subRc.top = nameRc.top + S(h, 20);
        DrawTextW(dc, subShow.c_str(), (int)subShow.size(), &subRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        ycur += st->listItemH;
    }

    outListRc = RECT{ x, y, x + wList, y + hgt };

    // sync scrollbar control
    UpdateListScrollbar(st, (int)st->contacts.size());
}

// ---------- Window procedure ----------
static const wchar_t* kClass = L"VCF_VIEW_CLASS";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ViewState* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_CREATE: {
        st = new ViewState();
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        MakeFonts(h, st->fonts);
        static ULONG_PTR gdipToken = 0;
        if (!gdipToken) { GdiplusStartupInput gi; GdiplusStartup(&gdipToken, &gi, nullptr); }

        // создаем ЛЕВЫЙ вертикальный скроллбар (внутри окна)
        st->hScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100,
            h, nullptr, GetModuleHandleW(nullptr), nullptr);
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
        // позиционируем полосу прокрутки у края левой панели
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
            if (pos != st->listScroll) {
                st->listScroll = pos;
                InvalidateRect(h, nullptr, TRUE);
            }
            // sync pos
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
            // обновим контрол
            SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = st->listScroll;
            SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // только выбор элемента списка, без ссылок
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        // вычислим индекс по координате
        int pad = S(h, 8);
        int top = pad;
        int listW = ListPaneWidth(h) - ScrollbarWidth();
        if (x >= pad && x < listW - pad) {
            int row = (y - top) / (st->listItemH ? st->listItemH : S(h, 52));
            if (row >= 0) {
                size_t idx = (size_t)(st->listScroll + row);
                if (idx < st->contacts.size()) {
                    st->sel = idx;
                    InvalidateRect(h, nullptr, TRUE);
                    return 0;
                }
            }
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!st) break;
        if (w == VK_UP) {
            if (st->sel > 0) { st->sel--; if ((int)st->sel < st->listScroll) st->listScroll = (int)st->sel; InvalidateRect(h, nullptr, TRUE); }
            return 0;
        }
        if (w == VK_DOWN) {
            if (st->sel + 1 < st->contacts.size()) {
                st->sel++;
                RECT rc; GetClientRect(h, &rc);
                int innerH = rc.bottom - rc.top - S(h, 16);
                if (!st->listItemH) st->listItemH = S(h, 52);
                int perPage = std::max(1, innerH / st->listItemH);
                if ((int)st->sel >= st->listScroll + perPage) st->listScroll = (int)st->sel - (perPage - 1);
                InvalidateRect(h, nullptr, TRUE);
            }
            return 0;
        }
        if (w == VK_PRIOR) {
            st->listScroll = std::max(0, st->listScroll - std::max(1, st->perPage - 1));
            InvalidateRect(h, nullptr, TRUE); return 0;
        }
        if (w == VK_NEXT) {
            st->listScroll = std::min(std::max(0, (int)st->contacts.size() - st->perPage), st->listScroll + std::max(1, st->perPage - 1));
            InvalidateRect(h, nullptr, TRUE); return 0;
        }
        if (w == VK_HOME) { st->listScroll = 0; st->sel = 0; InvalidateRect(h, nullptr, TRUE); return 0; }
        if (w == VK_END) {
            st->sel = st->contacts.empty() ? 0 : st->contacts.size() - 1;
            st->listScroll = std::max(0, (int)st->contacts.size() - st->perPage);
            InvalidateRect(h, nullptr, TRUE); return 0;
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
        // левая колонка
        RenderList(mem, h, st, rc.left, rc.top, listW, rc.bottom - rc.top, st->listRc);

        // разделитель
        RECT sep{ listW, rc.top, listW + 1, rc.bottom }; FillRect(mem, &sep, (HBRUSH)GetStockObject(GRAY_BRUSH));

        // правая панель (все поля контакта)
        if (st->sel < st->contacts.size()) {
            int dx = listW + S(h, 12);
            int dy = rc.top + S(h, 12);
            int dw = rc.right - dx - S(h, 12);

            const Contact& c = st->contacts[st->sel];

            // Name
            std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
            if (name.empty()) name = L"(no name)";

            DrawNameField(mem, st->fonts, h, dx, dy, dw, name);

            // Photo
            if (c.photo && !c.photo->bytes.empty()) {
                auto img = ImageFromBytes(c.photo->bytes);
                if (img) {
                    int maxW = S(h, 160);
                    int iw = (int)img->GetWidth();
                    int ih = (int)img->GetHeight();
                    if (iw > 0 && ih > 0) {
                        double k = (double)maxW / iw; if (k > 1.0) k = 1.0;
                        int dwp = (int)(iw * k), dhp = (int)(ih * k);
                        Graphics g(mem);
                        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                        g.DrawImage(img.get(), dx, dy, dwp, dhp);
                        dy += dhp + S(h, 8);
                    }
                }
            }

            auto& F = st->fonts;
            if (!c.org.empty())   DrawLabelValue(mem, F, dx, dy, dw, L"Org", c.org, RGB(90, 90, 90), RGB(30, 30, 30), h);
            if (!c.title.empty()) DrawLabelValue(mem, F, dx, dy, dw, L"Role", c.title, RGB(90, 90, 90), RGB(30, 30, 30), h);
            if (!c.url.empty())   DrawLabelValue(mem, F, dx, dy, dw, L"URL", c.url, RGB(90, 90, 90), RGB(30, 30, 30), h);
            if (!c.bday.empty())  DrawLabelValue(mem, F, dx, dy, dw, L"BDay", c.bday, RGB(90, 90, 90), RGB(30, 30, 30), h);

            // Phones
            for (auto& p : c.phones) {
                if (p.number.empty()) continue;
                std::wstring val = p.number;
                if (!p.types.empty()) {
                    std::wstring types;
                    for (size_t i = 0; i < p.types.size(); ++i) { if (i) types += L","; types += p.types[i]; }
                    val += L" (" + types + L")";
                }
                DrawLabelValue(mem, F, dx, dy, dw, L"Phone", val, RGB(90, 90, 90), RGB(30, 30, 30), h);
            }

            // Emails (primary) — если пусто, используем fallback
            bool anyEmail = false;
            for (auto& e : c.emails) {
                if (e.addr.empty()) continue;
                anyEmail = true;
                std::wstring val = e.addr;
                if (!e.types.empty()) {
                    std::wstring types;
                    for (size_t i = 0; i < e.types.size(); ++i) { if (i) types += L","; types += e.types[i]; }
                    val += L" (" + types + L")";
                }
                DrawLabelValue(mem, F, dx, dy, dw, L"Email", val, RGB(90, 90, 90), RGB(30, 30, 30), h);
            }
            if (!anyEmail) {
                std::wstring fb = FallbackEmail(c);
                if (!fb.empty()) {
                    DrawLabelValue(mem, F, dx, dy, dw, L"Email", fb, RGB(90, 90, 90), RGB(30, 30, 30), h);
                }
            }

            // Addresses
            for (auto& a : c.addrs) {
                if (a.text.empty()) continue;
                DrawLabelValue(mem, F, dx, dy, dw, L"Address", a.text, RGB(90, 90, 90), RGB(30, 30, 30), h);
            }
            if (!c.note.empty()) {
                DrawLabelValue(mem, F, dx, dy, dw, L"Note", c.note, RGB(90, 90, 90), RGB(30, 30, 30), h);
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
        InvalidateRect(h, nullptr, TRUE);
    }
}

// Search helpers
static bool isWordBoundary(const std::wstring& s, size_t pos) { return (pos == 0) || !iswalnum(s[pos - 1]); }
static bool isWordBoundary2(const std::wstring& s, size_t pos) { return (pos >= s.size()) || !iswalnum(s[pos]); }

bool VCFView_SearchEx(HWND h, const std::wstring& needle,
    size_t startIndex, bool backwards,
    bool matchCase, bool wholeWord, bool wrap)
{
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st || st->contacts.empty() || needle.empty()) return false;

    auto norm = [&](std::wstring x) {
        if (!matchCase) { std::transform(x.begin(), x.end(), x.begin(), ::towlower); }
        return x;
        };
    std::wstring n = norm(needle);

    auto buildHay = [&](const Contact& c) {
        std::wstring h;
        auto add = [&](const std::wstring& s) { if (!s.empty()) { h += L" "; h += norm(s); } };
        add(c.fn); add(c.n_given); add(c.n_family); add(c.org); add(c.title); add(c.bday); add(c.url); add(c.note);
        for (auto& t : c.phones) { add(t.number); for (auto& tp : t.types) add(tp); }
        for (auto& e : c.emails) { add(e.addr); for (auto& tp : e.types) add(tp); }
        for (auto& a : c.addrs) { add(a.text); }
        return h;
        };

    const size_t count = st->contacts.size();
    auto nextIndex = [&](size_t i)->size_t {
        return backwards ? (i == 0 ? count - 1 : i - 1) : (i + 1 == count ? 0 : i + 1);
        };

    size_t i = startIndex % count;
    size_t first = i;
    do {
        std::wstring hay = buildHay(st->contacts[i]);
        size_t pos = hay.find(n);
        while (pos != std::wstring::npos) {
            if (!wholeWord || (isWordBoundary(hay, pos) && isWordBoundary2(hay, pos + n.size()))) {
                st->sel = i;
                InvalidateRect(h, nullptr, TRUE);
                return true;
            }
            pos = hay.find(n, pos + 1);
        }
        i = nextIndex(i);
    } while (wrap && i != first);

    return false;
}

bool VCFView_Search(HWND h, const std::wstring& needle) {
    return VCFView_SearchEx(h, needle, 0, /*backwards*/false, /*matchCase*/false, /*whole*/false, /*wrap*/true);
}

bool VCFView_CopyActive(HWND) { return false; } // отключено
