// vcf_view.cpp — левый список + правый блок: фото (сверху) + EDIT (ниже)
// Вывод ВСЕХ полей vCard (v2.1/v3/v4) c локализацией ключей/TYPE (RU/EN по языку TC)
// 2.1: поддержка QUOTED-PRINTABLE + CHARSET, склейка мягких переносов, PHOTO;ENCODING=BASE64 многострочный
// UNICODE/_UNICODE come from the project (avoid C4005 redefinition)
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <richedit.h>
#include <uxtheme.h>
#include <wininet.h>
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "uxtheme.lib")

#include <vector>
#include <string>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <map>
#include <locale>  // Добавлено для std::iswspace и локалей (хотя <cwctype> покрывает базовое)

#include "vcf_view.hpp"
#include "vcf_theme.hpp"
#include "vcf_utils.hpp"

// ===================== Helpers =====================
using namespace Gdiplus;
static int Dpi(HWND h) { HDC dc = GetDC(h); int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96; if (dc)ReleaseDC(h, dc); return dpi ? dpi : 96; }
static int S(HWND h, int px) { return MulDiv(px, Dpi(h), 96); }

struct Fonts { HFONT hNorm = nullptr; HFONT hSmall = nullptr; };
static HFONT MakeFont(HWND h, int px, int weight, const wchar_t* face) {
    LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET; lf.lfHeight = -S(h, px); lf.lfWeight = weight; lf.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE); return CreateFontIndirectW(&lf);
}
static void MakeFonts(HWND h, Fonts& f) { f.hNorm = MakeFont(h, 14, FW_NORMAL, L"Segoe UI"); f.hSmall = MakeFont(h, 12, FW_NORMAL, L"Segoe UI"); }
static void FreeFonts(Fonts& f) { if (f.hNorm)DeleteObject(f.hNorm); if (f.hSmall)DeleteObject(f.hSmall); f = {}; }

static std::wstring PrimaryPhone(const Contact& c) { return c.phones.empty() ? L"" : c.phones[0].number; }
static std::wstring PrimaryEmail(const Contact& c) { for (auto& e : c.emails) if (!e.addr.empty()) return e.addr; return L""; }

static bool IsEmailChar(wchar_t ch) { return iswalnum(ch) || ch == L'.' || ch == L'_' || ch == L'-' || ch == L'+'; }
static std::wstring ExtractEmailFromText(const std::wstring& txt) {
    size_t at = txt.find(L'@'); if (at == std::wstring::npos) return L""; size_t l = at, r = at;
    while (l > 0 && IsEmailChar(txt[l - 1])) --l; while (r + 1 < txt.size() && IsEmailChar(txt[r + 1])) ++r;
    return (l<at && r>at) ? txt.substr(l, r - l + 1) : L"";
}
static std::wstring FallbackEmail_NotesAware(const Contact& c) {
    if (!c.url.empty()) {
        const std::wstring m = L"mailto:"; if (c.url.size() > m.size()) {
            std::wstring low = c.url; std::transform(low.begin(), low.end(), low.begin(), ::towlower);
            if (low.rfind(m, 0) == 0) { std::wstring e = c.url.substr(m.size()); size_t q = e.find(L'?'); if (q != std::wstring::npos) e = e.substr(0, q); return e; }
        }
        std::wstring f = ExtractEmailFromText(c.url); if (!f.empty()) return f;
    }
    if (!c.note.empty()) { std::wstring f = ExtractEmailFromText(c.note); if (!f.empty()) return f; }
    for (const auto& n : c.notes) {
        std::wstring f = ExtractEmailFromText(n);
        if (!f.empty()) return f;
    }
    return L"";
}

// ===================== Локализация ключей и TYPE =====================
static std::wstring LocalizeKey(const std::wstring& keyRaw, bool ru) {
    std::wstring k = keyRaw;
    size_t dot = k.find(L'.'); if (dot != std::wstring::npos) k = k.substr(dot + 1);
    std::wstring up = ToUpperASCII(k);

    struct KV { const wchar_t* en; const wchar_t* ru; };
    static const std::map<std::wstring, KV> mapKeys = {
        {L"N",            {L"Name",              L"Имя"}},
        {L"FN",           {L"Full name",         L"Полное имя"}},
        {L"NICKNAME",     {L"Nickname",          L"Псевдоним"}},
        {L"ORG",          {L"Organization",      L"Компания"}},
        {L"TITLE",        {L"Role",              L"Должность"}},
        {L"BDAY",         {L"Birthday",          L"День рождения"}},
        {L"ANNIVERSARY",  {L"Anniversary",       L"Годовщина"}},
        {L"TEL",          {L"Phone",             L"Телефон"}},
        {L"EMAIL",        {L"Email",             L"Почта"}},
        {L"ADR",          {L"Address",           L"Адрес"}},
        {L"URL",          {L"URL",               L"URL"}},
        {L"IMPP",         {L"IM",                L"IM"}},
        {L"NOTE",         {L"Note",              L"Заметка"}},
        {L"CATEGORIES",   {L"Categories",        L"Категории"}},
        {L"PHOTO",        {L"Photo",             L"Фото"}},
        {L"REV",          {L"Last modified",     L"Изменено"}},
        {L"UID",          {L"UID",               L"UID"}},
        {L"KEY",          {L"Key",               L"Ключ"}},
        {L"PRODID",       {L"Product ID",        L"Идентификатор продукта"}},
        {L"VERSION",      {L"Version",           L"Версия"}},
        {L"GENDER",       {L"Gender",            L"Пол"}},
        {L"LANG",         {L"Language",          L"Язык"}},
        {L"KIND",         {L"Kind",              L"Тип контакта"}},
        {L"MEMBER",       {L"Member",            L"Участник"}},
        {L"X-GENDER",     {L"Gender",            L"Пол"}},

        // Доп. ключи
        {L"X-ABDATE",                 {L"Additional Date",        L"Дополнительная дата"}},
        {L"X-ABLABEL",                {L"Label",                  L"Метка"}},
        {L"X-ABRELATEDNAMES",         {L"Related Name",           L"Связанное имя"}},
        {L"X-ALTBDAY",                {L"Alternative Birthday",   L"Альтернативная дата рождения"}},
        {L"X-ANDROID-CUSTOM",         {L"Android Custom Event",   L"Пользовательское событие Android"}},
        {L"X-MAIDENNAME",             {L"Maiden Name",            L"Девичья фамилия"}},
        {L"X-PHONETIC-MIDDLE-NAME",   {L"Phonetic Middle Name",   L"Фонетическое отчество"}},
        {L"X-PHONETIC-ORG",           {L"Phonetic Organization Name", L"Фонетическое название организации"}},
        {L"X-PHONETIC-FIRST-NAME",    {L"Phonetic First Name",    L"Фонетическое имя"}},
        {L"X-PHONETIC-LAST-NAME",     {L"Phonetic Last Name",     L"Фонетическая фамилия"}},
        {L"X-SOCIALPROFILE",          {L"Social Profile",         L"Социальный профиль"}},
    };
    auto it = mapKeys.find(up);
    if (it != mapKeys.end()) return ru ? it->second.ru : it->second.en;
    return keyRaw; // X-*, неизвестные — как в файле
}

static std::wstring LocalizeTypeToken(const std::wstring& typeRaw, bool ru) {
    std::wstring t = ToUpperASCII(typeRaw);

    if (t == L"IPHONE") t = L"CELL";
    if (t == L"MSG")    t = L"TEXT";

    struct KV { const wchar_t* en; const wchar_t* ru; };
    static const std::map<std::wstring, KV> types = {
        {L"HOME",   {L"Home",        L"Дом"}},
        {L"WORK",   {L"Work",        L"Работа"}},
        {L"CELL",   {L"Mobile",      L"Мобильный"}},
        {L"TEXT",   {L"Text",        L"Текст"}},
        {L"VOICE",  {L"Voice",       L"Голос"}},
        {L"FAX",    {L"Fax",         L"Факс"}},
        {L"VIDEO",  {L"Video",       L"Видео"}},
        {L"PAGER",  {L"Pager",       L"Пейджер"}},
        {L"MSG",    {L"Message",     L"Сообщения"}},
        {L"BBS",    {L"BBS",         L"BBS"}},
        {L"MODEM",  {L"Modem",       L"Модем"}},
        {L"ISDN",   {L"ISDN",        L"ISDN"}},
        {L"PCS",    {L"PCS",         L"PCS"}},
        {L"PREF",   {L"Preferred",   L"Предпочт."}},
        {L"INTERNET",{L"Internet",   L"Интернет"}},
        {L"DOM",    {L"Domestic",    L"Внутр."}},
        {L"INTL",   {L"International", L"Междунар."}},
        {L"POSTAL", {L"Postal",      L"Почтовый"}},
        {L"PARCEL", {L"Parcel",      L"Посылки"}},
        {L"OTHER",  {L"Other",       L"Другое"}},

        // Соцсети из X-SOCIALPROFILE:
        {L"TWITTER", {L"Twitter",  L"Twitter"}},
        {L"FACEBOOK",{L"Facebook", L"Facebook"}},
        {L"FLICKR",  {L"Flickr",   L"Flickr"}},

        // Android MIME из X-ANDROID-CUSTOM:
        {L"VND.ANDROID.CURSOR.ITEM/CONTACT_EVENT",
                    {L"Contact event (Android)", L"Событие контакта (Android)"}},
    };

    auto it = types.find(t);
    if (it != types.end()) return ru ? it->second.ru : it->second.en;
    return typeRaw; // неизвестные TYPE — без перевода
}

// ===================== ВЫВОД ВСЕХ ПОЛЕЙ из сырого блока =====================
static bool IsSection(const std::wstring& s, const wchar_t* key) {
    std::wstring t = s; std::transform(t.begin(), t.end(), t.begin(), ::towupper);
    std::wstring k = key; std::transform(k.begin(), k.end(), k.begin(), ::towupper);
    return t.rfind(k, 0) == 0;
}

static bool HeaderHasParam(const std::wstring& headUp, const std::wstring& pname, std::wstring* pValueOut = nullptr) {
    // Ищем NAME=VALUE в шапке (без учёта регистра); если VALUE не нужен — pValueOut=nullptr
    size_t pos = 0;
    while (pos < headUp.size()) {
        size_t semi = headUp.find(L';', pos);
        std::wstring token = headUp.substr(pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos);
        size_t eq = token.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring name = Trim(token.substr(0, eq));
            if (name == pname) {
                std::wstring v = Trim(token.substr(eq + 1));
                if (pValueOut) *pValueOut = unquote(v);
                return true;
            }
        }
        else {
            // bare parameter (например PREF), пропускаем
        }
        if (semi == std::wstring::npos) break;
        pos = semi + 1;
    }
    return false;
}

// Разбор заголовка "KEY;PARAM=...;TYPE=...;PREF;WORK:VALUE" и сбор локализованной подписи
static std::wstring BuildLocalizedHead(const std::wstring& headRaw, bool ru) {
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < headRaw.size()) {
        size_t semi = headRaw.find(L';', i);
        if (semi == std::wstring::npos) { parts.push_back(headRaw.substr(i)); break; }
        parts.push_back(headRaw.substr(i, semi - i));
        i = semi + 1;
    }
    if (parts.empty()) return headRaw;

    std::wstring key = Trim(parts[0]);
    std::vector<std::wstring> types;

    for (size_t k = 1; k < parts.size(); ++k) {
        std::wstring p = parts[k];
        size_t eq = p.find(L'=');
        if (eq == std::wstring::npos) { if (!p.empty()) types.push_back(unquote(p)); continue; }
        std::wstring pname = ToUpperASCII(Trim(p.substr(0, eq)));
        std::wstring pval = Trim(p.substr(eq + 1));
        if (pname == L"TYPE") {
            std::wstring tv = unquote(pval);
            size_t j = 0;
            while (j < tv.size()) {
                size_t comma = tv.find(L',', j);
                if (comma == std::wstring::npos) { types.push_back(Trim(tv.substr(j))); break; }
                types.push_back(Trim(tv.substr(j, comma - j)));
                j = comma + 1;
            }
        }
        else if (pname == L"PREF" || pname == L"ENCODING" || pname == L"CHARSET" || pname == L"VALUE") {
            continue;
        }
        else if (pname == L"X-SERVICE-TYPE") {
            // для IMPP и подобных
            types.push_back(Trim(pval));
        }
    }

    std::wstring label = LocalizeKey(key, ru);
    if (!types.empty()) {
        std::wstring typed;
        for (size_t t = 0; t < types.size(); ++t) {
            std::wstring loc = LocalizeTypeToken(types[t], ru);
            if (t) typed += L", ";
            typed += loc;
        }
        label += L" ("; label += typed; label += L")";
    }
    return label;
}

// BlackBerry and some phone exports produce JPEGs where APP0/APP1 length
// overruns into the next marker (e.g. DQT). GDI+ then reports
// "Quantization table not defined" and fails. Clamp segment lengths so
// markers stay visible; append EOI if truncated.
static void RepairJpegBytes(std::vector<uint8_t>& data) {
    if (data.size() < 4) return;
    if (!(data[0] == 0xFF && data[1] == 0xD8)) return; // not JPEG

    std::vector<uint8_t> out;
    out.reserve(data.size() + 4);
    out.push_back(0xFF);
    out.push_back(0xD8);
    size_t i = 2;
    auto push_range = [&](size_t a, size_t b) {
        if (b > data.size()) b = data.size();
        if (a < b) out.insert(out.end(), data.begin() + (ptrdiff_t)a, data.begin() + (ptrdiff_t)b);
    };

    while (i + 1 < data.size()) {
        // seek FF
        if (data[i] != 0xFF) {
            size_t j = i;
            while (j < data.size() && data[j] != 0xFF) ++j;
            // skip stray non-marker bytes between segments
            i = j;
            continue;
        }
        // skip fill 0xFF
        while (i + 1 < data.size() && data[i] == 0xFF && data[i + 1] == 0xFF) ++i;
        if (i + 1 >= data.size()) break;
        uint8_t mt = data[i + 1];
        if (mt == 0x00) { // escaped FF in entropy — shouldn't appear outside SOS
            i += 2;
            continue;
        }
        if (mt == 0xD9) { // EOI
            out.push_back(0xFF);
            out.push_back(0xD9);
            data.swap(out);
            return;
        }
        if (mt == 0xD8) { // extra SOI
            out.push_back(0xFF);
            out.push_back(0xD8);
            i += 2;
            continue;
        }
        // RST / TEM without length
        if ((mt >= 0xD0 && mt <= 0xD7) || mt == 0x01) {
            out.push_back(0xFF);
            out.push_back(mt);
            i += 2;
            continue;
        }
        if (i + 3 >= data.size()) {
            push_range(i, data.size());
            break;
        }
        unsigned n = (unsigned(data[i + 2]) << 8) | unsigned(data[i + 3]);
        if (n < 2) { i += 2; continue; }

        size_t segDataStart = i + 4;
        size_t claimedEnd = i + 2 + n; // exclusive

        // For APPn / COM: if a real marker appears before claimedEnd, clamp length
        bool isAppOrCom = (mt >= 0xE0 && mt <= 0xEF) || mt == 0xFE;
        if (isAppOrCom && claimedEnd > segDataStart) {
            size_t limit = std::min(claimedEnd, data.size());
            for (size_t j = segDataStart; j + 1 < limit; ++j) {
                if (data[j] == 0xFF) {
                    uint8_t m2 = data[j + 1];
                    if (m2 != 0x00 && m2 != 0xFF) {
                        // clamp segment to end at j
                        unsigned newN = (unsigned)(j - (i + 2));
                        if (newN >= 2) {
                            out.push_back(0xFF);
                            out.push_back(mt);
                            out.push_back((uint8_t)((newN >> 8) & 0xFF));
                            out.push_back((uint8_t)(newN & 0xFF));
                            push_range(segDataStart, j);
                            i = j;
                            goto next_seg;
                        }
                    }
                }
            }
        }

        if (claimedEnd > data.size()) {
            // truncated segment — copy rest
            push_range(i, data.size());
            break;
        }
        push_range(i, claimedEnd);
        i = claimedEnd;

        if (mt == 0xDA) {
            // entropy-coded scan: copy remaining bytes
            push_range(i, data.size());
            break;
        }
    next_seg:;
    }

    // Ensure EOI for truncated BlackBerry photos (scan cut off)
    if (out.size() < 2 || !(out[out.size() - 2] == 0xFF && out[out.size() - 1] == 0xD9)) {
        out.push_back(0xFF);
        out.push_back(0xD9);
    }
    data.swap(out);
}

// Create Bitmap that owns its pixels. FromStream often keeps a live IStream
// reference — never return that object after releasing the stream (UAF).
static std::unique_ptr<Gdiplus::Bitmap> BitmapFromMemory(const std::vector<uint8_t>& bytesIn) {
    if (bytesIn.empty()) return nullptr;

    std::vector<uint8_t> bytes = bytesIn;
    bool repaired = false;
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        RepairJpegBytes(bytes);
        repaired = true;
    }

    auto tryLoad = [](const std::vector<uint8_t>& data) -> std::unique_ptr<Gdiplus::Bitmap> {
        if (data.empty()) return nullptr;

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
        if (!hMem) return nullptr;
        void* p = GlobalLock(hMem);
        if (!p) { GlobalFree(hMem); return nullptr; }
        memcpy(p, data.data(), data.size());
        GlobalUnlock(hMem);

        // TRUE: stream frees HGLOBAL when its last ref dies
        IStream* pStream = nullptr;
        if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) != S_OK) {
            GlobalFree(hMem);
            return nullptr;
        }

        // Stream-backed image — only temporary. Do not return this pointer.
        std::unique_ptr<Gdiplus::Bitmap> streamBmp(Gdiplus::Bitmap::FromStream(pStream));
        std::unique_ptr<Gdiplus::Bitmap> owned;

        if (streamBmp) {
            const UINT w = streamBmp->GetWidth();
            const UINT h = streamBmp->GetHeight();
            if (w > 0 && h > 0) {
                // Force full decode while stream is still valid
                BitmapData bd{};
                Rect r(0, 0, (INT)w, (INT)h);
                if (streamBmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok)
                    streamBmp->UnlockBits(&bd);

                // Clone → independent pixel buffer (safe after stream release)
                Bitmap* cloned = streamBmp->Clone(r, PixelFormat32bppARGB);
                if (cloned && cloned->GetLastStatus() == Ok && cloned->GetWidth() > 0) {
                    owned.reset(cloned);
                } else {
                    delete cloned;
                    // Fallback: draw into a new memory Bitmap (also stream-independent)
                    std::unique_ptr<Gdiplus::Bitmap> memBmp(new Gdiplus::Bitmap((INT)w, (INT)h, PixelFormat32bppARGB));
                    if (memBmp && memBmp->GetLastStatus() == Ok) {
                        Graphics g(memBmp.get());
                        if (g.GetLastStatus() == Ok) {
                            g.DrawImage(streamBmp.get(), 0, 0, (INT)w, (INT)h);
                            if (memBmp->GetLastStatus() == Ok && memBmp->GetWidth() > 0)
                                owned = std::move(memBmp);
                        }
                    }
                }
            }
        }

        // Destroy stream-backed bitmap BEFORE releasing our stream ref
        streamBmp.reset();
        pStream->Release();
        // Never return streamBmp — only the independent `owned` bitmap
        return owned;
    };

    if (auto bmp = tryLoad(bytes)) return bmp;
    if (repaired)
        return tryLoad(bytesIn);
    return nullptr;
}

static bool LooksLikeBinaryBlob(const std::wstring& v) {
    if (v.size() < 60) return false;
    // JPEG/PNG base64 signature (photo dump into text)
    std::wstring t = v;
    while (!t.empty() && iswspace(t[0])) t.erase(t.begin());
    if (t.rfind(L"/9j/", 0) == 0 || t.rfind(L"iVBOR", 0) == 0) return true;

    // long base64-ish payload (Apple X-ADDRESSING-GRAMMAR / orphan PHOTO folds)
    size_t b64 = 0, total = 0;
    for (wchar_t c : v) {
        if (c == L'\r' || c == L'\n' || c == L' ') continue;
        ++total;
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
            (c >= L'0' && c <= L'9') || c == L'+' || c == L'/' || c == L'=' || c == L'-' || c == L'_')
            ++b64;
    }
    return total >= 60 && b64 * 10 >= total * 9;
}

// ===================== Card text from parsed Contact (no second raw parse) =====================
static std::wstring NormalizeDisplayNewlines(std::wstring val) {
    // \n → CRLF for Win32 RichEdit; keep other content
    std::wstring u;
    u.reserve(val.size() + 8);
    for (size_t k = 0; k < val.size(); ++k) {
        if (val[k] == L'\n') {
            if (u.empty() || u.back() != L'\r') u += L'\r';
            u += L'\n';
            continue;
        }
        if (val[k] == L'\r') {
            u += L'\r';
            if (k + 1 < val.size() && val[k + 1] == L'\n') { u += L'\n'; ++k; }
            else u += L'\n';
            continue;
        }
        u += val[k];
    }
    return u;
}

static std::wstring BuildFromContact(const Contact& c, bool ru) {
    // X-ABLABEL → group prefix map (item2. → "Домашние контакты")
    std::map<std::wstring, std::wstring> itemLabels;
    auto stripApple = [](std::wstring s) -> std::wstring {
        if (s.find(L"_$!<") == 0 && s.size() > 4 && s.rfind(L">!$_") == s.size() - 4)
            return s.substr(4, s.size() - 8);
        if (s.find(L"$!<") == 0 && s.rfind(L">!$") == s.size() - 3)
            return s.substr(3, s.size() - 6);
        return s;
    };
    for (const auto& f : c.fields) {
        if (f.prop != L"X-ABLABEL") continue;
        std::wstring pretty = stripApple(f.value);
        std::wstring key = ToUpperASCII(pretty);
        if (key == L"HOMEPAGE") pretty = ru ? L"Домашняя страница" : L"Home page";
        else if (key == L"ANNIVERSARY") pretty = ru ? L"Годовщина" : L"Anniversary";
        else if (key == L"MOTHER") pretty = ru ? L"Мать" : L"Mother";
        else if (key == L"FATHER") pretty = ru ? L"Отец" : L"Father";
        else if (key == L"BROTHER") pretty = ru ? L"Брат" : L"Brother";
        else if (key == L"SISTER") pretty = ru ? L"Сестра" : L"Sister";
        else if (key == L"SPOUSE") pretty = ru ? L"Супруг(а)" : L"Spouse";
        else if (key == L"CHILD") pretty = ru ? L"Ребёнок" : L"Child";
        else if (key == L"FRIEND") pretty = ru ? L"Друг" : L"Friend";
        else if (key == L"MANAGER") pretty = ru ? L"Руководитель" : L"Manager";
        size_t d = f.head.find(L'.');
        if (d != std::wstring::npos)
            itemLabels[f.head.substr(0, d + 1)] = pretty;
    }

    std::wstring out;
    for (const auto& f : c.fields) {
        if (!f.show && f.prop != L"X-ABLABEL") continue;
        if (f.prop == L"X-ABLABEL") continue;
        if (!f.show) continue;

        std::wstring val = f.value;
        if (val.empty() && !f.isNote) continue;
        if (LooksLikeBinaryBlob(val)) continue;

        // Skip N when equal to FN
        if (f.prop == L"N" && !c.fn.empty() && val == c.fn) continue;

        // Pretty GENDER / KIND
        if (f.prop == L"GENDER" || f.prop == L"X-GENDER") {
            std::wstring g = val;
            size_t sc = g.find(L';');
            std::wstring sex = ToUpperASCII(Trim(sc != std::wstring::npos ? g.substr(0, sc) : g));
            std::wstring id = (sc != std::wstring::npos) ? Trim(g.substr(sc + 1)) : L"";
            std::wstring pretty = sex;
            if (sex == L"M") pretty = ru ? L"Мужской" : L"Male";
            else if (sex == L"F") pretty = ru ? L"Женский" : L"Female";
            else if (sex == L"O") pretty = ru ? L"Другой" : L"Other";
            else if (sex == L"N") pretty = ru ? L"Не указан" : L"None";
            else if (sex == L"U") pretty = ru ? L"Неизвестно" : L"Unknown";
            if (!id.empty() && id != pretty) pretty += L" (" + id + L")";
            val = pretty;
        } else if (f.prop == L"KIND") {
            std::wstring k = ToUpperASCII(Trim(val));
            if (k == L"INDIVIDUAL") val = ru ? L"Человек" : L"Individual";
            else if (k == L"GROUP") val = ru ? L"Группа" : L"Group";
            else if (k == L"ORG" || k == L"ORGANIZATION") val = ru ? L"Организация" : L"Organization";
            else if (k == L"LOCATION") val = ru ? L"Место" : L"Location";
        }

        if (f.prop == L"X-ANDROID-CUSTOM") {
            // already have raw; light cleanup
            if (val.find(L"vnd.android.cursor.item/") == 0)
                val = val.substr(24);
        }

        val = NormalizeDisplayNewlines(std::move(val));

        std::wstring label = BuildLocalizedHead(f.head, ru);
        size_t dotPos = f.head.find(L'.');
        if (dotPos != std::wstring::npos) {
            auto it = itemLabels.find(f.head.substr(0, dotPos + 1));
            if (it != itemLabels.end()) label = it->second;
        }
        if (f.prop.rfind(L"X-FCENCODED-", 0) == 0)
            label = ru ? L"Связанное / Пользовательское" : L"Related / Custom";

        if (f.isNote || f.prop == L"NOTE") {
            if (!out.empty()) out += L"\r\n";
            out += L"── ";
            out += label;
            out += L" ──\r\n";
            out += val;
            if (val.empty() || val.back() != L'\n') out += L"\r\n";
            out += L"\r\n";
            continue;
        }

        out += label;
        out += L'\t';
        out += val;
        out += L"\r\n";
    }

    // Optional PHOTO URL line (if any, no embedded photo)
    if (!c.photo_url.empty() && !c.photo.has_value()) {
        out += (ru ? L"Фото" : L"Photo");
        out += L'\t';
        out += c.photo_url;
        out += L"\r\n";
    }
    return out;
}

// ===================== Состояние вьюера =====================
static const wchar_t* kClass = L"VCF_VIEW_CLASS";
static const wchar_t* kPhotoClass = L"VCF_PHOTO_VIEW";
static const int kSplitHit = 4; // half-width of splitter hit zone (px at 96dpi)

struct ViewState {
    std::vector<Contact> contacts;           // все контакты файла
    std::vector<std::wstring> rawBlocks;     // сырые vCard-блоки
    std::vector<size_t> visibleIdx;          // индексы после quick filter
    size_t sel = 0;

    int listScroll = 0;
    int listItemH = 0;
    RECT listRc{};
    int  perPage = 1;

    HWND hScroll = nullptr;
    HWND hPhoto = nullptr;                  // окно превью фото
    HWND hEdit = nullptr;
    HWND hFilter = nullptr;                 // quick filter edit above list
    HWND hPhotoOnly = nullptr;              // checkbox: only with photo

    int rightScroll = 0;
    HWND hRightScroll = nullptr;            // single scrollbar for photo+text card

    // resizable splitter (list width at 96dpi; 0 = default)
    int listPaneW96 = 0;
    bool draggingSplit = false;
    int splitX = 0;

    // search highlight (TC F3)
    std::wstring searchNeedle;
    std::vector<unsigned char> matchFlags; // 1 = contact matches current search
    int matchCount = 0;
    int matchPos = 0; // 1-based index among matches for status

    // quick filter (local, above list)
    std::wstring filterText;
    bool filterPhotoOnly = false;

    // tooltip for long names
    HWND hTip = nullptr;
    int tipRow = -1;

    // settings (from TC ini)
    bool loadPhotoUrl = false; // LoadPhotoUrl=1 enables HTTP photo fetch

    std::unique_ptr<Gdiplus::Bitmap> photo;  // изображение
    Fonts fonts;
};

// GWLP_USERDATA only if hwnd is a live window (guards PhotoWndProc parent / stray HWNDs).
static ViewState* ViewStateFromHwnd(HWND h) {
    if (!h || !IsWindow(h)) return nullptr;
    return reinterpret_cast<ViewState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

static bool ContactHasPhoto(const ViewState* st, size_t idx) {
    if (!st || idx >= st->contacts.size()) return false;
    const Contact& c = st->contacts[idx];
    if (c.photo.has_value() && !c.photo->bytes.empty()) return true;
    if (!c.photo_url.empty()) return true;
    return false;
}

static bool ContactMatchesNeedle(const Contact& c, const std::wstring& needle, bool wholeWord, bool matchCase = false);
static void SetSelectionAndReveal(HWND h, ViewState* st, size_t idx);
static void UpdateMatchPosFromSelection(ViewState* st);

static void RebuildVisibleList(ViewState* st) {
    if (!st) return;
    st->visibleIdx.clear();
    st->visibleIdx.reserve(st->contacts.size());
    std::wstring f = LowerInvariant(st->filterText);
    for (size_t i = 0; i < st->contacts.size(); ++i) {
        if (st->filterPhotoOnly && !ContactHasPhoto(st, i)) continue;
        if (!f.empty() && !ContactMatchesNeedle(st->contacts[i], f, false)) continue;
        st->visibleIdx.push_back(i);
    }
    // keep selection if still visible; else select first visible
    bool selVisible = false;
    for (size_t v : st->visibleIdx) if (v == st->sel) { selVisible = true; break; }
    if (!selVisible) {
        st->sel = st->visibleIdx.empty() ? 0 : st->visibleIdx[0];
        st->listScroll = 0;
        st->rightScroll = 0;
        UpdateMatchPosFromSelection(st);
    }
}

static int VisibleCount(const ViewState* st) {
    return st ? (int)st->visibleIdx.size() : 0;
}

static size_t VisibleContact(const ViewState* st, int visRow) {
    if (!st || visRow < 0 || visRow >= (int)st->visibleIdx.size()) return (size_t)-1;
    return st->visibleIdx[(size_t)visRow];
}

static int FindVisibleRow(const ViewState* st, size_t contactIdx) {
    if (!st) return -1;
    for (int i = 0; i < (int)st->visibleIdx.size(); ++i)
        if (st->visibleIdx[(size_t)i] == contactIdx) return i;
    return -1;
}

// Outer window height needed so all multiline text is visible (no inner V-scroll).
// Uses real EDIT layout (EM_POSFROMCHAR) + DrawText / line-count fallbacks.
static int MeasureEditContentHeight(HWND hEdit, int widthPx) {
    if (!hEdit || !IsWindow(hEdit) || widthPx <= 8) return 40;
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return 40;

    // Font line height
    int lineH = 16;
    HDC dc = GetDC(hEdit);
    HFONT hf = (HFONT)SendMessageW(hEdit, WM_GETFONT, 0, 0);
    HFONT oldF = nullptr;
    if (dc) {
        oldF = hf ? (HFONT)SelectObject(dc, hf) : nullptr;
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm))
            lineH = tm.tmHeight + tm.tmExternalLeading;
    }

    // Non-client (edge) overhead: MoveWindow sets outer size, text lives in client
    RECT wr{}, cr{};
    GetWindowRect(hEdit, &wr);
    GetClientRect(hEdit, &cr);
    int ncH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    if (ncH < 0) ncH = 0;
    if (ncH == 0) ncH = 2 * GetSystemMetrics(SM_CYEDGE) + 2; // WS_EX_CLIENTEDGE estimate

    // Force a tall layout at the target width so wrapping/line count match the real card
    const int probeH = 16000;
    SetWindowPos(hEdit, nullptr, 0, 0, widthPx, probeH,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);

    int byPos = 0;
    // Y of last character (client coords) — most accurate when char is laid out
    LRESULT pos = SendMessageW(hEdit, EM_POSFROMCHAR, (WPARAM)(len > 0 ? len - 1 : 0), 0);
    if (pos != (LRESULT)-1) {
        int y = (int)(short)HIWORD((DWORD)pos);
        // include full last line + padding
        byPos = y + lineH + 8;
    }

    int lineCount = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0);
    if (lineCount < 1) lineCount = 1;
    int byLines = lineCount * lineH + 12;

    int byDraw = 0;
    if (dc) {
        std::wstring text((size_t)len, L'\0');
        GetWindowTextW(hEdit, &text[0], len + 1);
        // EDIT client is a bit narrower than window width
        int textW = std::max(8, widthPx - ncH - 8);
        RECT rc{ 0, 0, textW, 0 };
        DrawTextW(dc, text.c_str(), len, &rc,
            DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EDITCONTROL | DT_EXPANDTABS);
        byDraw = (rc.bottom - rc.top) + 12;
        if (oldF) SelectObject(dc, oldF);
        ReleaseDC(hEdit, dc);
        dc = nullptr;
    }
    if (dc) { if (oldF) SelectObject(dc, oldF); ReleaseDC(hEdit, dc); }

    // Client content height, then convert to outer window height
    int clientH = std::max(byPos, std::max(byLines, byDraw));
    if (clientH < lineH + 8) clientH = lineH + 8;
    int outerH = clientH + ncH + 4;
    if (outerH < 40) outerH = 40;
    if (outerH > 20000) outerH = 20000;
    return outerH;
}

static int ReadIniInt(const wchar_t* key, int defVal) {
    if (g_iniPath.empty()) return defVal;
    return (int)GetPrivateProfileIntW(L"VCFLister", key, defVal, g_iniPath.c_str());
}
static void WriteIniInt(const wchar_t* key, int val) {
    if (g_iniPath.empty()) return;
    wchar_t buf[32]; wsprintfW(buf, L"%d", val);
    WritePrivateProfileStringW(L"VCFLister", key, buf, g_iniPath.c_str());
}
static void LoadViewSettings(ViewState* st) {
    if (!st) return;
    st->listPaneW96 = ReadIniInt(L"ListWidth", 0);
    st->loadPhotoUrl = ReadIniInt(L"LoadPhotoUrl", 0) != 0;
}
static void SaveListWidth(ViewState* st, HWND h) {
    if (!st || st->listPaneW96 <= 0) return;
    WriteIniInt(L"ListWidth", st->listPaneW96);
    (void)h;
}

// Download image bytes from http(s) URL (optional photo feature; default off)
static std::vector<uint8_t> HttpGetBytes(const std::wstring& url, DWORD timeoutMs = 5000) {
    std::vector<uint8_t> data;
    if (url.size() < 8) return data;
    std::wstring low = url; std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    if (low.rfind(L"http://", 0) != 0 && low.rfind(L"https://", 0) != 0) return data;

    HINTERNET hNet = InternetOpenW(L"VCFLister/2.1", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) return data;
    InternetSetOptionW(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(hNet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE;
    if (low.rfind(L"https://", 0) == 0) flags |= INTERNET_FLAG_SECURE;
    HINTERNET hUrl = InternetOpenUrlW(hNet, url.c_str(), nullptr, 0, flags, 0);
    if (!hUrl) { InternetCloseHandle(hNet); return data; }

    BYTE buf[8192];
    DWORD rd = 0;
    const size_t kMax = 5 * 1024 * 1024;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &rd) && rd > 0) {
        data.insert(data.end(), buf, buf + rd);
        if (data.size() >= kMax) break;
    }
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    return data;
}

static std::wstring ContactDisplayName(const Contact& c) {
    std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
    while (!name.empty() && (name.back() == L'=' || name.back() == L' ' || name.back() == L'\t'))
        name.pop_back();
    return name;
}

// Match needle inside one field; no cross-field haystring.
// When matchCase==false, needle must already be LowerInvariant.
static bool FieldMatchesNeedle(const std::wstring& field, const std::wstring& needle, bool wholeWord, bool matchCase) {
    if (field.empty() || needle.empty()) return false;
    std::wstring storage;
    const std::wstring* hay = &field;
    if (!matchCase) {
        // Lower after a raw-size reject would be wrong: e.g. "ß" → "ss" grows under LCMapString.
        storage = LowerInvariant(field);
        hay = &storage;
    }
    if (hay->size() < needle.size()) return false;
    for (size_t pos = hay->find(needle); pos != std::wstring::npos; pos = hay->find(needle, pos + 1)) {
        if (!wholeWord || (isWordBoundary(*hay, pos) && isWordBoundary2(*hay, pos + needle.size())))
            return true;
    }
    return false;
}

static bool ContactMatchesNeedle(const Contact& c, const std::wstring& needle, bool wholeWord, bool matchCase) {
    if (needle.empty()) return false;
    auto check = [&](const std::wstring& s) -> bool {
        return FieldMatchesNeedle(s, needle, wholeWord, matchCase);
    };

    // Card UI is built from fields — cover NICKNAME/IMPP/X-*/etc., not only typed members.
    for (const auto& f : c.fields) {
        if (!f.value.empty() && check(f.value)) return true;
    }
    for (const auto& ac : c.androidCustoms) {
        if (check(ac.rawType)) return true;
        for (const auto& slot : ac.slots) if (check(slot)) return true;
    }

    // Structured members (also used when fields is empty / partial contacts)
    if (check(c.fn) || check(c.n_given) || check(c.n_family) || check(c.org) ||
        check(c.title) || check(c.bday) || check(c.url) || check(c.note) ||
        check(c.gender) || check(c.lang) || check(c.kind) || check(c.photo_url))
        return true;

    for (const auto& t : c.notes) if (check(t)) return true;
    for (const auto& t : c.phones) {
        if (check(t.number)) return true;
        for (const auto& tp : t.types) if (check(tp)) return true;
    }
    for (const auto& e : c.emails) {
        if (check(e.addr)) return true;
        for (const auto& tp : e.types) if (check(tp)) return true;
    }
    for (const auto& a : c.addrs) if (check(a.text)) return true;
    for (const auto& u : c.urls) if (check(u)) return true;
    for (const auto& lg : c.langs) if (check(lg)) return true;
    for (const auto& m : c.members) if (check(m)) return true;
    return false;
}

// 1-based position of st->sel among matches; 0 if selection is not a match (or no search).
static void UpdateMatchPosFromSelection(ViewState* st) {
    if (!st) return;
    st->matchPos = 0;
    if (st->matchCount <= 0 || st->matchFlags.empty()) return;
    if (st->sel >= st->matchFlags.size() || !st->matchFlags[st->sel]) return;
    int p = 0;
    for (size_t i = 0; i <= st->sel; ++i) if (st->matchFlags[i]) ++p;
    st->matchPos = p;
}

static void RebuildSearchFlags(ViewState* st, bool wholeWord = false, bool matchCase = false) {
    if (!st) return;
    st->matchFlags.assign(st->contacts.size(), 0);
    st->matchCount = 0;
    st->matchPos = 0;
    if (st->searchNeedle.empty()) return;
    const std::wstring n = matchCase ? st->searchNeedle : LowerInvariant(st->searchNeedle);
    for (size_t i = 0; i < st->contacts.size(); ++i) {
        if (ContactMatchesNeedle(st->contacts[i], n, wholeWord, matchCase)) {
            st->matchFlags[i] = 1;
            st->matchCount++;
        }
    }
    UpdateMatchPosFromSelection(st);
}

// Extract value part from a card line (table: label\\tvalue, or legacy "label: value")
static std::wstring ValueAfterColon(const std::wstring& line) {
    size_t p = line.find(L'\t');
    if (p != std::wstring::npos) return Trim(line.substr(p + 1));
    p = line.find(L':');
    if (p == std::wstring::npos) return Trim(line);
    return Trim(line.substr(p + 1));
}

static std::wstring GetEditCurrentLine(HWND hEdit) {
    DWORD a = 0, b = 0;
    SendMessageW(hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    // Use selection start; for multi-line sel still the line under the caret start
    int line = (int)SendMessageW(hEdit, EM_LINEFROMCHAR, a, 0);
    int idx = (int)SendMessageW(hEdit, EM_LINEINDEX, line, 0);
    if (idx < 0) return L"";
    // Prefer EM_GETTEXTRANGE — reliable on RichEdit (EM_GETLINE length quirks)
    int lineLen = (int)SendMessageW(hEdit, EM_LINELENGTH, idx, 0);
    if (lineLen < 0) lineLen = 0;
    if (lineLen > 16000) lineLen = 16000;
    if (lineLen == 0) {
        // EM_LINELENGTH can be 0 for empty line; try GETLINE capacity probe
        wchar_t probe[8]; *(WORD*)probe = 7;
        lineLen = (int)SendMessageW(hEdit, EM_GETLINE, line, (LPARAM)probe);
        if (lineLen < 0) lineLen = 0;
    }
    if (lineLen <= 0) return L"";
    std::wstring out((size_t)lineLen, L'\0');
    TEXTRANGEW tr{};
    tr.chrg.cpMin = idx;
    tr.chrg.cpMax = idx + lineLen;
    tr.lpstrText = &out[0];
    int got = (int)SendMessageW(hEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    if (got < 0) got = 0;
    if (got > lineLen) got = lineLen;
    out.resize((size_t)got);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n'))
        out.pop_back();
    return out;
}

// Full field value on the current line (ignores selection) — for "Copy line value"
static std::wstring GetEditLineValue(HWND hEdit) {
    return ValueAfterColon(GetEditCurrentLine(hEdit));
}

// Selection text via RichEdit char range (not GetWindowText substr — indices differ / off-by-one)
static std::wstring GetEditSelectionText(HWND hEdit) {
    CHARRANGE cr{};
    SendMessageW(hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (cr.cpMin == cr.cpMax) return L"";
    LONG n = cr.cpMax - cr.cpMin;
    if (n < 0) n = -n;
    if (n > 1'000'000) n = 1'000'000;
    std::wstring out((size_t)n, L'\0');
    TEXTRANGEW tr{};
    tr.chrg.cpMin = (std::min)(cr.cpMin, cr.cpMax);
    tr.chrg.cpMax = (std::max)(cr.cpMin, cr.cpMax);
    tr.lpstrText = &out[0];
    int got = (int)SendMessageW(hEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    if (got < 0) got = 0;
    out.resize((size_t)got);
    return out;
}

// ===================== Копирование =====================
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

// ===================== Левая панель (список) =====================
static int DlgSBW() { return GetSystemMetrics(SM_CXVSCROLL); }
static int ListPaneWidth(HWND h) {
    auto* st = ViewStateFromHwnd(h);
    int def96 = 300;
    int w96 = (st && st->listPaneW96 > 0) ? st->listPaneW96 : def96;
    int w = S(h, w96);
    RECT rc{}; GetClientRect(h, &rc);
    int minW = S(h, 160);
    int maxW = std::max<int>(minW, (int)rc.right - S(h, 220));
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    return w;
}
static void EnsureListMetrics(HWND h, ViewState* st) { if (!st->listItemH) st->listItemH = S(h, 60); }
static int SplitterX(HWND h) { return ListPaneWidth(h); }
static bool HitSplitter(HWND h, int x) {
    int sx = SplitterX(h);
    int hit = S(h, kSplitHit);
    return x >= sx - hit && x <= sx + hit;
}

static void UpdateListScrollbar(ViewState* st, int total) {
    if (!st->hScroll) return;
    SCROLLINFO si{}; si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS; si.nMin = 0; si.nMax = std::max<int>(0, total - 1);
    si.nPage = std::max<int>(1, st->perPage);
    si.nPos = std::min<int>(st->listScroll, std::max<int>(0, total - st->perPage));
    SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
    ShowWindow(st->hScroll, (total > st->perPage) ? SW_SHOW : SW_HIDE);
}

static void RenderList(HDC dc, HWND h, ViewState* st, int x, int y, int w, int hgt, RECT& outListRc) {
    EnsureListMetrics(h, st);
    if (st->visibleIdx.empty() && !st->contacts.empty())
        RebuildVisibleList(st);

    int sbw = DlgSBW();
    int wList = w - sbw; if (wList < S(h, 120)) wList = w;
    int visN = VisibleCount(st);

    HBRUSH bg = CreateSolidBrush(g_clrListBg);
    RECT rbg{ x,y,x + wList,y + hgt }; FillRect(dc, &rbg, bg); DeleteObject(bg);

    int pad = S(h, 8);
    int innerTop = y + pad;
    int innerH = hgt - pad - pad;
    st->perPage = std::max<int>(1, innerH / st->listItemH);

    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max<int>(0, visN - st->perPage);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;

    int ycur = innerTop;
    for (int row = 0; row < st->perPage && st->listScroll + row < visN; ++row) {
        size_t idx = VisibleContact(st, st->listScroll + row);
        if (idx == (size_t)-1 || idx >= st->contacts.size()) continue;
        const Contact& c = st->contacts[idx];

        std::wstring name = ContactDisplayName(c);
        if (name.empty()) {
            name = g_tcRu ? L"(пустая карточка)" : L"(empty card)";
        }

        // Индикатор фото
        bool hasPhoto = ContactHasPhoto(st, idx);
        if (hasPhoto) name += L" 📷";

        std::wstring sub;
        std::wstring pv = PrimaryPhone(c);
        if (!pv.empty()) sub = L"Tel: " + pv;
        else {
            std::wstring em = PrimaryEmail(c);
            if (!em.empty()) sub = L"Email: " + em;
        }
        if (sub.empty() && !c.kind.empty()) {
            sub = (g_tcRu ? L"Тип: " : L"Kind: ") + c.kind;
        }

        RECT item{ x + pad, ycur, x + wList - pad, ycur + st->listItemH - S(h,2) };
        COLORREF bgItem = g_clrListBg;
        if (idx == st->sel) bgItem = g_clrListSel;
        else if (idx < st->matchFlags.size() && st->matchFlags[idx]) {
            // search match highlight
            bgItem = g_dark ? RGB(48, 60, 40) : RGB(255, 249, 196);
        }
        HBRUSH ibg = CreateSolidBrush(bgItem);
        FillRect(dc, &item, ibg); DeleteObject(ibg);

        HPEN pen = CreatePen(PS_SOLID, 1, g_clrGrid); HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, item.left, item.bottom, nullptr); LineTo(dc, item.right, item.bottom);
        SelectObject(dc, oldPen); DeleteObject(pen);

        HFONT old = (HFONT)SelectObject(dc, st->fonts.hNorm);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_clrTxt);

        // Имя: даём больше места по ширине, разрешаем перенос на 2 строки если имя очень длинное
        RECT nameRc = item;
        nameRc.left += S(h, 8);
        nameRc.top += S(h, 4);
        nameRc.right -= S(h, 4);
        nameRc.bottom -= S(h, 18);  // макс. место под имя (sub займёт снизу)

        // Измеряем реальную высоту имени (для плотного размещения sub)
        RECT nameCalc = nameRc;
        DrawTextW(dc, name.c_str(), (int)name.size(), &nameCalc, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK | DT_CALCRECT);

        int nameBottom = std::min<int>(nameCalc.bottom, nameRc.bottom);
        DrawTextW(dc, name.c_str(), (int)name.size(), &nameRc, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK | DT_END_ELLIPSIS);

        SelectObject(dc, st->fonts.hSmall);
        SetTextColor(dc, g_clrSub);
        if (sub.empty()) {
            std::wstring fb = FallbackEmail_NotesAware(c);
            if (!fb.empty()) sub = L"Email: " + fb;
        }
        RECT subRc = item;
        subRc.left += S(h, 8);
        subRc.top = nameBottom + S(h, 2);
        subRc.right -= S(h, 4);
        subRc.bottom -= S(h, 2);
        DrawTextW(dc, sub.c_str(), (int)sub.size(), &subRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old); // restore font

        ycur += st->listItemH;
    }

    outListRc = RECT{ x,y,x + wList,y + hgt };
    UpdateListScrollbar(st, visN);
}

// ===================== Card text (RichEdit) — two-column table (#18) =====================
static void EnsureRichEditLoaded() {
    static bool once = false;
    if (!once) {
        if (!LoadLibraryW(L"Msftedit.dll"))
            LoadLibraryW(L"Riched20.dll");
        once = true;
    }
}

// Label column width in twips (~38% of client, clamped)
static void ApplyCardEditTabStop(HWND hEdit) {
    if (!hEdit || !IsWindow(hEdit)) return;
    RECT rc{}; GetClientRect(hEdit, &rc);
    int w = rc.right - rc.left;
    if (w < 40) return;
    HDC dc = GetDC(hEdit);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hEdit, dc);
    if (dpi <= 0) dpi = 96;

    int labelPx = w * 38 / 100;
    if (labelPx < 100) labelPx = std::min(100, w / 2);
    if (labelPx > 240) labelPx = 240;
    if (labelPx >= w - 40) labelPx = w / 2;

    LONG tabTwips = MulDiv(labelPx, 1440, dpi);
    if (tabTwips < 400) tabTwips = 400;

    PARAFORMAT2 pf{};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_TABSTOPS;
    pf.cTabCount = 1;
    pf.rgxTabs[0] = tabTwips;
    // Apply to all text
    SendMessageW(hEdit, EM_SETSEL, 0, -1);
    SendMessageW(hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessageW(hEdit, EM_SETSEL, 0, 0);
}

// Two-column card: "Label\\tValue". Colors only (no bold).
// Positions from EM_LINEINDEX — RichEdit's real indices (not source \\r\\n offsets).
static void SetCardEditText(HWND hEdit, const std::wstring& text, HFONT hFont) {
    if (!hEdit || !IsWindow(hEdit)) return;

    LOGFONTW lf{};
    if (hFont) GetObjectW(hFont, sizeof(lf), &lf);
    int yHeight = 180;
    if (lf.lfHeight != 0) {
        HDC dc = GetDC(hEdit);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(hEdit, dc);
        int px = (lf.lfHeight < 0) ? -lf.lfHeight : lf.lfHeight;
        if (px > 0 && dpi > 0)
            yHeight = MulDiv(px, 72 * 20, dpi);
        if (yHeight < 140) yHeight = 140;
        if (yHeight > 360) yHeight = 360;
    }
    const wchar_t* face = (lf.lfFaceName[0]) ? lf.lfFaceName : L"Segoe UI";

    SendMessageW(hEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_clrBk);
    SendMessageW(hEdit, EM_SETUNDOLIMIT, 0, 0);
    SendMessageW(hEdit, WM_SETREDRAW, FALSE, 0);

    SETTEXTEX stx{};
    stx.flags = ST_DEFAULT;
    stx.codepage = 1200;
    SendMessageW(hEdit, EM_SETTEXTEX, (WPARAM)&stx, (LPARAM)text.c_str());

    ApplyCardEditTabStop(hEdit);

    CHARFORMAT2W cfBase{};
    cfBase.cbSize = sizeof(cfBase);
    cfBase.dwMask = CFM_BOLD | CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cfBase.dwEffects = 0; // never bold
    cfBase.crTextColor = g_clrTxt;
    cfBase.yHeight = yHeight;
    cfBase.bCharSet = DEFAULT_CHARSET;
    wcsncpy_s(cfBase.szFaceName, face, _TRUNCATE);
    SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cfBase);

    CHARFORMAT2W cfLabel = cfBase;
    cfLabel.crTextColor = g_clrSub; // muted labels

    CHARFORMAT2W cfValue = cfBase;
    cfValue.crTextColor = g_clrTxt; // primary values

    CHARFORMAT2W cfHead = cfBase;
    cfHead.crTextColor = g_clrTxt;

    const int lineCount = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0);
    for (int li = 0; li < lineCount; ++li) {
        const int lineIdx = (int)SendMessageW(hEdit, EM_LINEINDEX, li, 0);
        if (lineIdx < 0) continue;
        int lineLen = (int)SendMessageW(hEdit, EM_LINELENGTH, lineIdx, 0);
        if (lineLen <= 0) continue;
        if (lineLen > 8000) lineLen = 8000;

        std::vector<wchar_t> buf((size_t)lineLen + 2, 0);
        *(WORD*)buf.data() = (WORD)(lineLen + 1);
        int got = (int)SendMessageW(hEdit, EM_GETLINE, li, (LPARAM)buf.data());
        if (got < 0) got = 0;
        if (got > lineLen) got = lineLen;
        std::wstring line(buf.data(), buf.data() + got);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
            line.pop_back();
        if (line.empty()) continue;

        const int absEnd = lineIdx + (int)line.size();

        // Note header ── … ──
        if (line[0] == L'\x2500' || (line.size() >= 2 && line[0] == L'-' && line[1] == L'-')) {
            SendMessageW(hEdit, EM_SETSEL, (WPARAM)lineIdx, (LPARAM)absEnd);
            SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfHead);
            continue;
        }

        // Table row: label \\t value
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos) {
            SendMessageW(hEdit, EM_SETSEL, (WPARAM)lineIdx, (LPARAM)(lineIdx + (int)tab));
            SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfLabel);
            if (tab + 1 < line.size()) {
                SendMessageW(hEdit, EM_SETSEL, (WPARAM)(lineIdx + (int)tab + 1), (LPARAM)absEnd);
                SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfValue);
            }
            continue;
        }

        // Legacy "label: value" or NOTE body
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos) {
            size_t valOff = colon + 1;
            if (valOff < line.size() && line[valOff] == L' ') ++valOff;
            SendMessageW(hEdit, EM_SETSEL, (WPARAM)lineIdx, (LPARAM)(lineIdx + (int)valOff));
            SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfLabel);
            if (valOff < line.size()) {
                SendMessageW(hEdit, EM_SETSEL, (WPARAM)(lineIdx + (int)valOff), (LPARAM)absEnd);
                SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfValue);
            }
        } else {
            SendMessageW(hEdit, EM_SETSEL, (WPARAM)lineIdx, (LPARAM)absEnd);
            SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfValue);
        }
    }

    SendMessageW(hEdit, EM_SETSEL, 0, 0);
    SendMessageW(hEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hEdit, nullptr, TRUE);
}

// ===================== EDIT и ФОТО: наполнение и поведение =====================
static void UpdateRightPanel(ViewState* st) {
    if (!st) return;
    // фото: embedded → raw base64 → optional URL download
    st->photo.reset();
    std::wstring photoUrl;
    if (st->sel < st->contacts.size()) {
        const Contact& c = st->contacts[st->sel];
        if (c.photo.has_value() && !c.photo->bytes.empty()) {
            st->photo = BitmapFromMemory(c.photo->bytes);
        }
        photoUrl = c.photo_url;
    }
    // Photo already decoded in Contact by parser (no second raw PHOTO parse)
    // Optional HTTP(S) photo (ini LoadPhotoUrl=1)
    if (!st->photo && st->loadPhotoUrl && !photoUrl.empty()) {
        auto bytes = HttpGetBytes(photoUrl);
        if (!bytes.empty()) st->photo = BitmapFromMemory(bytes);
    }
    if (IsWindow(st->hPhoto)) InvalidateRect(st->hPhoto, nullptr, TRUE);

    // Force relayout of photo/edit area whenever photo data (re)loaded, to ensure non-zero height
    if (st->hPhoto && IsWindow(st->hPhoto)) {
        HWND mainH = GetParent(st->hPhoto);
        if (mainH && IsWindow(mainH)) {
            RECT rc; GetClientRect(mainH, &rc);
            SendMessage(mainH, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
        }
    }

    // текст — only from already-parsed Contact.fields (no raw re-parse)
    if (IsWindow(st->hEdit)) {
        std::wstring text;
        if (st->sel < st->contacts.size())
            text = BuildFromContact(st->contacts[st->sel], g_tcRu);
        HWND keepFocus = GetFocus();
        SetCardEditText(st->hEdit, text, st->fonts.hNorm);
        if (keepFocus && IsWindow(keepFocus) && GetFocus() != keepFocus)
            SetFocus(keepFocus);
    }
}

// Forward ESC to Total Commander Lister parent so F3 viewer closes reliably
static void ForwardEscToLister(HWND hwnd) {
    HWND h = hwnd;
    while (h) {
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 64);
        if (_wcsicmp(cls, L"VCF_VIEW_CLASS") == 0) {
            HWND lister = GetParent(h);
            if (lister) {
                // TC Lister closes on ESC; also try WM_CLOSE as fallback
                PostMessageW(lister, WM_KEYDOWN, VK_ESCAPE, 0);
                PostMessageW(lister, WM_CLOSE, 0, 0);
            }
            return;
        }
        h = GetParent(h);
    }
}

// Navigate visible contact list (shared by main window + filter box)
static bool NavigateVisibleList(HWND h, ViewState* st, WPARAM key) {
    if (!st || st->contacts.empty()) return false;
    if (st->visibleIdx.empty()) RebuildVisibleList(st);
    int visN = VisibleCount(st);
    if (visN <= 0) return false;
    int visRow = FindVisibleRow(st, st->sel);
    if (visRow < 0) visRow = 0;
    int newRow = visRow;
    int page = std::max(1, st->perPage);
    bool handled = false;
    switch (key) {
    case VK_UP:    if (newRow > 0) { newRow--; handled = true; } break;
    case VK_DOWN:  if (newRow + 1 < visN) { newRow++; handled = true; } break;
    case VK_PRIOR: newRow = std::max(0, newRow - page); handled = true; break;
    case VK_NEXT:  newRow = std::min(visN - 1, newRow + page); handled = true; break;
    case VK_HOME:
        if (key == VK_HOME && (GetKeyState(VK_CONTROL) & 0x8000)) { /* allow Ctrl+Home in edit? */ }
        newRow = 0; handled = true; break;
    case VK_END:   newRow = visN - 1; handled = true; break;
    default: return false;
    }
    if (!handled) return false;
    size_t idx = VisibleContact(st, newRow);
    if (idx == (size_t)-1) return false;
    SetSelectionAndReveal(h, st, idx);
    return true;
}

// Shared subclass for filter / checkbox
static LRESULT CALLBACK EscChildSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR idSubClass, DWORD_PTR data) {
    HWND viewer = (HWND)data;
    auto* st = ViewStateFromHwnd(viewer);

    // TC Lister uses dialog-like keyboard routing; without WANTARROWS Up/Down never reach us
    if (msg == WM_GETDLGCODE) {
        LRESULT base = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (idSubClass == 1 || idSubClass == 2)
            return base | DLGC_WANTARROWS | DLGC_WANTALLKEYS | DLGC_WANTTAB;
        return base;
    }

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        ForwardEscToLister(hwnd);
        return 0;
    }

    // Filter edit (id=1): arrows/page navigate list and hand focus to viewer so further keys work
    if (idSubClass == 1 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && st) {
        if (wParam == VK_DOWN || wParam == VK_UP || wParam == VK_PRIOR || wParam == VK_NEXT
            || wParam == VK_RETURN) {
            if (wParam == VK_RETURN) {
                if (viewer) SetFocus(viewer);
                return 0;
            }
            NavigateVisibleList(viewer, st, wParam);
            // Always move focus to list: otherwise keys stay trapped in the filter
            if (viewer) SetFocus(viewer);
            return 0;
        }
        if (wParam == VK_TAB) {
            if (viewer) SetFocus(viewer);
            return 0;
        }
    }

    // Checkbox (id=2): arrows navigate and focus list
    if (idSubClass == 2 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && st) {
        if (wParam == VK_DOWN || wParam == VK_UP || wParam == VK_PRIOR || wParam == VK_NEXT
            || wParam == VK_HOME || wParam == VK_END) {
            NavigateVisibleList(viewer, st, wParam);
            if (viewer) SetFocus(viewer);
            return 0;
        }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Custom messages for copy menus (avoid WM_CONTEXTMENU / TPM reentrancy inside TC Lister)
static const UINT WM_VCF_MENU_LIST = WM_APP + 40;
static const UINT WM_VCF_MENU_CARD = WM_APP + 41;
static bool g_inPopupMenu = false;

// Safe popup for WLX under TC Lister (no SetForegroundWindow — can hang/crash TC)
static int TrackCopyMenu(HWND hwndPlugin, HMENU menu, POINT ptScreen) {
    if (!menu || !hwndPlugin) return 0;
    HWND owner = hwndPlugin;
    int cmd = (int)TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_NONOTIFY,
        ptScreen.x, ptScreen.y, 0, owner, nullptr);
    return cmd;
}

static LRESULT CALLBACK EditSubclassProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*id*/, DWORD_PTR /*data*/) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ForwardEscToLister(hEdit);
            return 0;
        }
        if ((wParam == 'C' || wParam == 'c') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DWORD a = 0, b = 0;
            SendMessageW(hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
            if (a == b) {
                HWND viewer = GetParent(hEdit);
                std::wstring v = ValueAfterColon(GetEditCurrentLine(hEdit));
                if (!v.empty() && viewer) { SetClipboardTextW(viewer, v); return 0; }
            }
        }
        break;
    case WM_SYSKEYDOWN:
    case WM_CHAR:
        if (wParam == VK_ESCAPE) {
            ForwardEscToLister(hEdit);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        HWND viewer = GetParent(hEdit);
        if (viewer)
            return SendMessageW(viewer, WM_MOUSEWHEEL, wParam, lParam);
        break;
    }
    case WM_RBUTTONDOWN: {
        // Keep existing mouse selection (user may RMB → Copy).
        // Only place caret when nothing is selected.
        DWORD a = 0, b = 0;
        SendMessageW(hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if (a == b) {
            // RichEdit: lParam is POINTL*, NOT MAKELPARAM (that crashes / AVs!)
            POINTL ptl{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            LRESULT idx = SendMessageW(hEdit, EM_CHARFROMPOS, 0, (LPARAM)&ptl);
            if (idx >= 0)
                SendMessageW(hEdit, EM_SETSEL, (WPARAM)idx, (LPARAM)idx);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        if (!g_inPopupMenu) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hEdit, &pt);
            HWND viewer = GetParent(hEdit);
            if (viewer && IsWindow(viewer))
                PostMessageW(viewer, WM_VCF_MENU_CARD, 0, POINTTOPOINTS(pt));
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        // Keyboard Shift+F10 / Apps key only — mouse path uses WM_VCF_MENU_CARD
        if (!g_inPopupMenu && (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1)) {
            HWND viewer = GetParent(hEdit);
            POINT pt{}; GetCursorPos(&pt);
            if (viewer)
                PostMessageW(viewer, WM_VCF_MENU_CARD, 0, POINTTOPOINTS(pt));
        }
        return 0; // never show built-in RichEdit menu
    case WM_LBUTTONDOWN:
        // Keep focus on the card text while the user selects — do NOT SetFocus(viewer) here
        // (that broke selection and could freeze RMB menu after drag-select).
        return DefSubclassProc(hEdit, msg, wParam, lParam);
    case WM_LBUTTONUP: {
        LRESULT res = DefSubclassProc(hEdit, msg, wParam, lParam);
        // If nothing selected (simple click), return focus to viewer for TC Lister keys.
        // If there is a selection, keep focus on the edit so copy / RMB work.
        DWORD a = 0, b = 0;
        SendMessageW(hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if (a == b) {
            HWND viewer = GetParent(hEdit);
            if (viewer && IsWindow(viewer))
                SetFocus(viewer);
        }
        return res;
    }
    case WM_MOUSEMOVE:
        return DefSubclassProc(hEdit, msg, wParam, lParam);
    }
    return DefSubclassProc(hEdit, msg, wParam, lParam);
}

// Окно превью фото
static LRESULT CALLBACK PhotoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Parent must be our viewer; never trust GWLP_USERDATA on a dead/foreign HWND.
    auto* st = ViewStateFromHwnd(GetParent(hwnd));
    switch (msg) {
    case WM_ERASEBKGND: {
        // Paint will cover everything via double buffer; prevent default erase to reduce flicker
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        // Double buffer the photo to reduce flicker (especially with larger size)
        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(memDC, memBmp);

        HBRUSH bg = g_hbrBk ? g_hbrBk : (HBRUSH)(COLOR_WINDOW + 1);
        FillRect(memDC, &rc, bg);

        if (st && st->photo) {
            int maxW = std::min<int>(1000, rc.right - rc.left);
            int maxH = std::min<int>(1000, rc.bottom - rc.top);
            UINT w = st->photo->GetWidth();
            UINT h = st->photo->GetHeight();
            if (w > 0 && h > 0) {
                double sx = static_cast<double>(maxW) / static_cast<double>(w);
                double sy = static_cast<double>(maxH) / static_cast<double>(h);
                double s = std::min<double>(1.0, std::min<double>(sx, sy));
                int dw = static_cast<int>(static_cast<double>(w) * s);
                int dh = static_cast<int>(static_cast<double>(h) * s);
                // Align photo to top-left corner (as requested), keep aspect fit
                int pad = 2;
                int x = rc.left + pad;
                int y = rc.top + pad;

                Graphics g(memDC);
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.DrawImage(st->photo.get(), Rect(x, y, dw, dh));
            }
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, old);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ===================== Выбор/скролл =====================
static void EnsureSelVisible(HWND h, ViewState* st) {
    if (st->visibleIdx.empty() && !st->contacts.empty()) RebuildVisibleList(st);
    RECT rc; GetClientRect(h, &rc);
    int statusH = S(h, 18);
    int filterH = S(h, 26);
    int innerH = (rc.bottom - statusH - filterH) - S(h, 16);
    int rowH = st->listItemH ? st->listItemH : S(h, 60);
    int per = std::max<int>(1, innerH / rowH);
    st->perPage = per;

    int visRow = FindVisibleRow(st, st->sel);
    if (visRow < 0) {
        st->listScroll = 0;
    } else {
        if (visRow < st->listScroll) st->listScroll = visRow;
        else if (visRow >= st->listScroll + per) st->listScroll = visRow - (per - 1);
    }

    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max<int>(0, VisibleCount(st) - per);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;
}
static void SetSelectionAndReveal(HWND h, ViewState* st, size_t idx) {
    if (!st || st->contacts.empty()) return;
    if (idx >= st->contacts.size()) idx = st->contacts.size() - 1;
    st->sel = idx; st->rightScroll = 0; EnsureSelVisible(h, st);
    // Keep status-bar "Found: p/N" in sync when selection changes via click/arrows
    // (SearchEx already updates matchPos; manual selection did not).
    UpdateMatchPosFromSelection(st);

    UpdateRightPanel(st);

    if (st->hScroll) {
        SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = st->listScroll;
        SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
    }
    InvalidateRect(h, nullptr, FALSE);
}

// ===================== Window proc =====================
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTALLKEYS;

    // Click on owner-drawn list must activate viewer (filter otherwise keeps keyboard)
    case WM_MOUSEACTIVATE:
        if (st) {
            SetFocus(h);
            return MA_ACTIVATE;
        }
        break;

    case WM_CREATE: {
        st = new ViewState(); SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        MakeFonts(h, st->fonts);
        static ULONG_PTR gdipToken = 0; if (!gdipToken) { GdiplusStartupInput gi; GdiplusStartup(&gdipToken, &gi, nullptr); }
        RecomputeTheme();

        // Регистрируем окно фото (один раз на процесс ок)
        static bool photoReg = false;
        if (!photoReg) {
            WNDCLASSW wc{}; wc.lpfnWndProc = PhotoWndProc; wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kPhotoClass; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            RegisterClassW(&wc); photoReg = true;
        }

        LoadViewSettings(st);

        // Tooltip for long contact names
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        st->hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            h, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (st->hTip) {
            SetWindowPos(st->hTip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            TOOLINFOW ti{}; ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
            ti.hwnd = h; ti.uId = 1;
            ti.lpszText = (LPWSTR)L"";
            GetClientRect(h, &ti.rect);
            SendMessageW(st->hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            SendMessageW(st->hTip, TTM_SETMAXTIPWIDTH, 0, 400);
            SendMessageW(st->hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
        }

        // Quick filter above list + "with photo" checkbox
        st->hFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 0, 0, h, (HMENU)1010, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(st->hFilter, WM_SETFONT, (WPARAM)st->fonts.hSmall, TRUE);
        // Cue banner (Vista+)
        SendMessageW(st->hFilter, 0x1501 /*EM_SETCUEBANNER*/, TRUE,
            (LPARAM)(g_tcRu ? L"Фильтр..." : L"Filter..."));
        SetWindowSubclass(st->hFilter, EscChildSubclass, 1, (DWORD_PTR)h);

        st->hPhotoOnly = CreateWindowExW(0, L"BUTTON",
            g_tcRu ? L"📷 фото" : L"📷 photo",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_TEXT,
            0, 0, 0, 0, h, (HMENU)1011, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(st->hPhotoOnly, WM_SETFONT, (WPARAM)st->fonts.hSmall, TRUE);
        // Disable visual styles so WM_CTLCOLORBTN can set light text in dark theme (#20)
        SetWindowTheme(st->hPhotoOnly, L"", L"");
        SetWindowSubclass(st->hPhotoOnly, EscChildSubclass, 2, (DWORD_PTR)h);

        // Скролл слева
        st->hScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100, h, nullptr, GetModuleHandleW(nullptr), nullptr);

        // Единственный скроллбар карточки (фото + полный текст)
        st->hRightScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100, h, nullptr, GetModuleHandleW(nullptr), nullptr);

        // Фото сверху справа
        st->hPhoto = CreateWindowExW(0, kPhotoClass, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, h, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);

        // RichEdit below photo: wrap, readonly, bold values (#18); outer hRightScroll scrolls card
        EnsureRichEditLoaded();
        st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL | ES_SAVESEL,
            0, 0, 0, 0, h, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
        if (!st->hEdit) {
            // Fallback if Msftedit unavailable
            st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, RICHEDIT_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
                0, 0, 0, 0, h, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
        }
        if (!st->hEdit) {
            st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
                0, 0, 0, 0, h, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
        }
        SendMessageW(st->hEdit, WM_SETFONT, (WPARAM)st->fonts.hNorm, TRUE);
        SendMessageW(st->hEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_clrBk);
        SendMessageW(st->hEdit, EM_AUTOURLDETECT, FALSE, 0);
        SetWindowSubclass(st->hEdit, EditSubclassProc, 3, 0);

        SetFocus(h);
        return 0;
    }
    case WM_DESTROY: {
        if (st) {
            if (st->hEdit && IsWindow(st->hEdit)) {
                RemoveWindowSubclass(st->hEdit, EditSubclassProc, 3);
                DestroyWindow(st->hEdit);
            }
            if (st->hPhoto && IsWindow(st->hPhoto)) DestroyWindow(st->hPhoto);
            if (st->hScroll && IsWindow(st->hScroll)) DestroyWindow(st->hScroll);
            if (st->hRightScroll && IsWindow(st->hRightScroll)) DestroyWindow(st->hRightScroll);
            if (st->hFilter && IsWindow(st->hFilter)) {
                RemoveWindowSubclass(st->hFilter, EscChildSubclass, 1);
                DestroyWindow(st->hFilter);
            }
            if (st->hPhotoOnly && IsWindow(st->hPhotoOnly)) {
                RemoveWindowSubclass(st->hPhotoOnly, EscChildSubclass, 2);
                DestroyWindow(st->hPhotoOnly);
            }
            if (st->hTip && IsWindow(st->hTip)) DestroyWindow(st->hTip);
            st->photo.reset();
            FreeFonts(st->fonts); delete st;
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
        int sbw = GetSystemMetrics(SM_CXVSCROLL);
        int statusH = S(h, 18);
        int filterH = S(h, 26);
        int filterPad = S(h, 4);
        int photoChkW = S(h, 72);

        // --- Left: filter row + list + status ---
        int filterTop = filterPad;
        int listTop = filterTop + filterH + filterPad;
        int listContentH = std::max<int>(0, (int)rc.bottom - statusH - listTop);

        if (st->hFilter) {
            MoveWindow(st->hFilter, filterPad, filterTop,
                std::max(40, listW - photoChkW - filterPad * 3), filterH, TRUE);
        }
        if (st->hPhotoOnly) {
            MoveWindow(st->hPhotoOnly, listW - photoChkW - filterPad, filterTop,
                photoChkW, filterH, TRUE);
        }
        MoveWindow(st->hScroll, listW - sbw, listTop, sbw, listContentH, TRUE);

        // --- Right column: PHOTO on top, TEXT below; one outer vertical scroll ---
        int pad = S(h, 12);
        int ex = listW + 1 + pad;
        int rightScrollBarW = sbw;
        int rightAreaTop = 0;
        int rightAreaH = std::max<int>(50, (int)rc.bottom);

        int ew = (int)rc.right - ex - pad - rightScrollBarW;
        if (ew < S(h, 80))
            ew = std::max<int>(0, (int)rc.right - ex - rightScrollBarW);

        // Cap photo size so huge images don't dominate the card
        int photoMaxW = std::min<int>(ew, S(h, 360));
        int photoMaxH = S(h, 360);
        int photoW = photoMaxW;
        int photoH = 0;
        if (st->photo) {
            UINT iw = st->photo->GetWidth();
            UINT ih = st->photo->GetHeight();
            double s = 1.0;
            if (iw > 0 && ih > 0) {
                double sx = static_cast<double>(photoMaxW) / static_cast<double>(iw);
                double sy = static_cast<double>(photoMaxH) / static_cast<double>(ih);
                s = std::min<double>(1.0, std::min<double>(sx, sy));
                photoW = std::max(1, (int)(iw * s));
                photoH = std::max(1, (int)(ih * s));
            }
            if (photoH <= 0) photoH = S(h, 120);
        }

        int sep = S(h, 8);
        int photoBlock = (photoH > 0) ? (photoH + sep) : 0;

        // Measure full text height at target width
        int measured = MeasureEditContentHeight(st->hEdit, ew);
        if (measured < S(h, 80)) measured = S(h, 80);

        // If photo + text fit, grow EDIT into free space (no clipped text / empty gap)
        int pads = pad * 2;
        int fitH = rightAreaH - pads - photoBlock;
        if (fitH < S(h, 80)) fitH = S(h, 80);

        int eh = measured;
        int contentH = pad + photoBlock + measured + pad;
        if (contentH <= rightAreaH) {
            eh = std::max(measured, fitH);
            contentH = rightAreaH;
        }

        // Photo first (top), then text
        int photoY = pad;
        int editY = pad + photoBlock;

        int maxScroll = std::max<int>(0, contentH - rightAreaH);
        if (st->rightScroll > maxScroll) st->rightScroll = maxScroll;
        if (st->rightScroll < 0) st->rightScroll = 0;
        int rightScroll = st->rightScroll;

        if (st->hRightScroll) {
            LONG_PTR style = GetWindowLongPtrW(st->hRightScroll, GWL_STYLE);
            if (!(style & SBS_VERT)) {
                SetWindowLongPtrW(st->hRightScroll, GWL_STYLE, (style & ~SBS_HORZ) | SBS_VERT | WS_CHILD);
            }
            MoveWindow(st->hRightScroll,
                (int)rc.right - rightScrollBarW, rightAreaTop,
                rightScrollBarW, rightAreaH, TRUE);
        }
        if (st->hPhoto) {
            if (photoH > 0) {
                ShowWindow(st->hPhoto, SW_SHOW);
                MoveWindow(st->hPhoto, ex, photoY - rightScroll, photoW, photoH, TRUE);
            } else {
                MoveWindow(st->hPhoto, ex, photoY - rightScroll, 0, 0, TRUE);
                ShowWindow(st->hPhoto, SW_HIDE);
            }
        }
        MoveWindow(st->hEdit, ex, editY - rightScroll, ew, eh, TRUE);
        // Keep label/value columns aligned when width changes
        if (st->hEdit) ApplyCardEditTabStop(st->hEdit);

        SCROLLINFO rsi{};
        rsi.cbSize = sizeof(rsi);
        rsi.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        rsi.nMin = 0;
        rsi.nMax = std::max<int>(0, contentH - 1);
        rsi.nPage = (UINT)std::max<int>(1, rightAreaH);
        rsi.nPos = rightScroll;
        if (st->hRightScroll) {
            SetScrollInfo(st->hRightScroll, SB_CTL, &rsi, TRUE);
            ShowWindow(st->hRightScroll, (maxScroll > 0) ? SW_SHOW : SW_HIDE);
        }

        InvalidateRect(h, nullptr, FALSE);
        if (st->hPhoto) InvalidateRect(st->hPhoto, nullptr, FALSE);
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        if (LOWORD(w) == 1010 && HIWORD(w) == EN_CHANGE && (HWND)l == st->hFilter) {
            int len = GetWindowTextLengthW(st->hFilter);
            st->filterText.assign((size_t)std::max(0, len), L'\0');
            if (len > 0) GetWindowTextW(st->hFilter, &st->filterText[0], len + 1);
            RebuildVisibleList(st);
            UpdateRightPanel(st);
            InvalidateRect(h, nullptr, FALSE);
            RECT rc; GetClientRect(h, &rc);
            SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
            return 0;
        }
        if (LOWORD(w) == 1011 && HIWORD(w) == BN_CLICKED && (HWND)l == st->hPhotoOnly) {
            st->filterPhotoOnly = (SendMessageW(st->hPhotoOnly, BM_GETCHECK, 0, 0) == BST_CHECKED);
            RebuildVisibleList(st);
            UpdateRightPanel(st);
            InvalidateRect(h, nullptr, FALSE);
            RECT rc; GetClientRect(h, &rc);
            SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
            // Вернуть фокус на viewer, чтобы ESC и стрелки снова работали
            SetFocus(h);
            return 0;
        }
        break;
    }
    case WM_VSCROLL: {
        if (!st) break;
        if ((HWND)l == st->hScroll) {
            int total = VisibleCount(st); if (total <= 0) return 0;
            int maxScroll = std::max<int>(0, total - st->perPage);
            int pos = st->listScroll;
            switch (LOWORD(w)) {
            case SB_LINEUP:   pos -= 1; break;
            case SB_LINEDOWN: pos += 1; break;
            case SB_PAGEUP:   pos -= std::max<int>(1, st->perPage - 1); break;
            case SB_PAGEDOWN: pos += std::max<int>(1, st->perPage - 1); break;
            case SB_TOP:      pos = 0; break;
            case SB_BOTTOM:   pos = maxScroll; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(st->hScroll, SB_CTL, &si); pos = si.nTrackPos; break;
            }
            }
            pos = std::max<int>(0, std::min<int>(maxScroll, pos));
            if (pos != st->listScroll) { st->listScroll = pos; InvalidateRect(h, nullptr, FALSE); }
            SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = st->listScroll;
            SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
            return 0;
        }
        else if ((HWND)l == st->hRightScroll) {
            // Скролл правой панели (карточка) — большое фото или длинный текст
            RECT rc; GetClientRect(h, &rc);
            int pad = S(h, 12);
            int rightAreaH = std::max<int>(50, (int)rc.bottom - pad - pad);

            SCROLLINFO cur{}; cur.cbSize = sizeof(cur); cur.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            GetScrollInfo(st->hRightScroll, SB_CTL, &cur);
            int maxPos = std::max<int>(0, (int)cur.nMax - (int)cur.nPage + 1);

            int pos = st->rightScroll;
            switch (LOWORD(w)) {
            case SB_LINEUP:   pos -= 30; break;
            case SB_LINEDOWN: pos += 30; break;
            case SB_PAGEUP:   pos -= std::max<int>(1, rightAreaH / 2); break;
            case SB_PAGEDOWN: pos += std::max<int>(1, rightAreaH / 2); break;
            case SB_TOP:      pos = 0; break;
            case SB_BOTTOM:   pos = maxPos; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(st->hRightScroll, SB_CTL, &si); pos = si.nTrackPos; break;
            }
            }
            st->rightScroll = std::max<int>(0, std::min<int>(maxPos, pos));
            SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
            InvalidateRect(h, nullptr, FALSE);
            if (st->hPhoto) InvalidateRect(st->hPhoto, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (!st) break;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        if (PtInRect(&st->listRc, pt)) {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            int step = (delta > 0) ? -1 : +1;
            int maxScroll = std::max<int>(0, VisibleCount(st) - st->perPage);
            st->listScroll = std::max<int>(0, std::min<int>(maxScroll, st->listScroll + step));
            SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_POS; si.nPos = st->listScroll;
            SetScrollInfo(st->hScroll, SB_CTL, &si, TRUE);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        } else {
            // Скролл правой карточки колесом мыши, если курсор над правой областью
            int listW = ListPaneWidth(h);
            if (pt.x > listW) {
                int delta = GET_WHEEL_DELTA_WPARAM(w);
                int step = (delta > 0) ? -40 : +40;
                SCROLLINFO cur{}; cur.cbSize = sizeof(cur); cur.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
                if (st->hRightScroll) GetScrollInfo(st->hRightScroll, SB_CTL, &cur);
                int maxPos = std::max<int>(0, (int)cur.nMax - (int)cur.nPage + 1);
                st->rightScroll = std::max<int>(0, std::min<int>(maxPos, st->rightScroll + step));
                RECT rc; GetClientRect(h, &rc);
                SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
                InvalidateRect(h, nullptr, FALSE);
                if (st->hPhoto) InvalidateRect(st->hPhoto, nullptr, FALSE);
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN: {
        // ESC — закрыть Lister (когда фокус на главном окне плагина)
        if (w == VK_ESCAPE) {
            ForwardEscToLister(h);
            return 0;
        }
        // Ctrl+C: copy selection / line value / contact name
        if (st && (GetKeyState(VK_CONTROL) & 0x8000) && (w == 'C' || w == 'c')) {
            HWND focus = GetFocus();
            if (focus == st->hEdit) {
                DWORD a = 0, b = 0; SendMessageW(st->hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
                if (a != b) SendMessageW(st->hEdit, WM_COPY, 0, 0);
                else SetClipboardTextW(h, ValueAfterColon(GetEditCurrentLine(st->hEdit)));
                return 0;
            }
            if (st->sel < st->contacts.size()) {
                std::wstring n = ContactDisplayName(st->contacts[st->sel]);
                if (!n.empty()) SetClipboardTextW(h, n);
                return 0;
            }
        }
        if (!st || st->contacts.empty()) return 0;
        // Навигация только по видимому (отфильтрованному) списку
        if (NavigateVisibleList(h, st, w)) return 0;
        break;
    }
    case WM_SETCURSOR: {
        if (st && LOWORD(l) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (st->draggingSplit || HitSplitter(h, pt.x)) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        RECT rcClient{}; GetClientRect(h, &rcClient);
        int statusH = S(h, 18);
        // Splitter drag
        if (HitSplitter(h, x)) {
            st->draggingSplit = true;
            SetCapture(h);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            SetFocus(h);
            return 0;
        }
        // Клики по статусбару / фильтру не выбирают контакты
        int filterH = S(h, 26);
        int filterPad = S(h, 4);
        int listTop = filterPad + filterH + filterPad;
        if (y >= rcClient.bottom - statusH || y < listTop) return 0;

        // Always take keyboard from filter/checkbox when user clicks the list area
        SetFocus(h);

        int pad = S(h, 8);
        int listPane = ListPaneWidth(h);
        int listW = listPane - GetSystemMetrics(SM_CXVSCROLL);
        if (x >= 0 && x < listPane) {
            // Hit-test rows against painted list (pad matches RenderList)
            int rowH = st->listItemH ? st->listItemH : S(h, 60);
            if (rowH <= 0) rowH = S(h, 60);
            int row = (y - listTop - pad) / rowH;
            if (row < 0) row = 0;
            if (row >= 0 && (x < listW || x < listPane)) {
                size_t idx = VisibleContact(st, st->listScroll + row);
                if (idx != (size_t)-1 && idx < st->contacts.size()) {
                    SetSelectionAndReveal(h, st, idx);
                    SetFocus(h); // again: UpdateRightPanel must not leave focus on hEdit
                    return 0;
                }
            }
            SetFocus(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (st->draggingSplit) {
            RECT rc; GetClientRect(h, &rc);
            int minW = S(h, 160), maxW = std::max<int>(minW, (int)rc.right - S(h, 220));
            int w = x;
            if (w < minW) w = minW;
            if (w > maxW) w = maxW;
            st->listPaneW96 = MulDiv(w, 96, Dpi(h));
            if (st->listPaneW96 < 160) st->listPaneW96 = 160;
            SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        // Tooltip for long list names (same hit-test as list click: filter offset + visibleIdx)
        if (st->hTip) {
            RECT rcClient{}; GetClientRect(h, &rcClient);
            int statusH = S(h, 18);
            int filterH = S(h, 26);
            int filterPad = S(h, 4);
            int listTop = filterPad + filterH + filterPad;
            int listW = ListPaneWidth(h);
            int pad = S(h, 8);
            int tipRow = -1;
            std::wstring tipText;
            if (x < listW && y >= listTop && y < rcClient.bottom - statusH) {
                int rowH = st->listItemH ? st->listItemH : S(h, 60);
                if (rowH <= 0) rowH = S(h, 60);
                int row = (y - listTop - pad) / rowH;
                if (row >= 0 && row < st->perPage) {
                    size_t idx = VisibleContact(st, st->listScroll + row);
                    if (idx != (size_t)-1 && idx < st->contacts.size()) {
                        tipRow = (int)idx;
                        tipText = ContactDisplayName(st->contacts[idx]);
                        if (tipText.empty()) tipText = g_tcRu ? L"(пустая карточка)" : L"(empty card)";
                        // only show if likely truncated (long name)
                        if (tipText.size() < 28) tipText.clear();
                    }
                }
            }
            if (tipRow != st->tipRow) {
                st->tipRow = tipRow;
                TOOLINFOW ti{}; ti.cbSize = sizeof(ti);
                ti.hwnd = h; ti.uId = 1;
                ti.lpszText = tipText.empty() ? (LPWSTR)L"" : (LPWSTR)tipText.c_str();
                SendMessageW(st->hTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
                if (!tipText.empty()) {
                    ti.rect = st->listRc;
                    SendMessageW(st->hTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
                }
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (st && st->draggingSplit) {
            st->draggingSplit = false;
            ReleaseCapture();
            SaveListWidth(st, h);
            return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED: {
        if (st) st->draggingSplit = false;
        break;
    }
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE: {
        RecomputeTheme();
        if (st) LoadViewSettings(st);
        UpdateRightPanel(st);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    // List RMB — post custom menu (do not use WM_CONTEXTMENU under TC Lister)
    case WM_RBUTTONUP: {
        if (!st || g_inPopupMenu) return 0;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int listW = ListPaneWidth(h);
        int filterH = S(h, 26), filterPad = S(h, 4), statusH = S(h, 18);
        int listTop = filterPad + filterH + filterPad;
        RECT rc{}; GetClientRect(h, &rc);
        if (x >= 0 && x < listW && y >= listTop && y < rc.bottom - statusH) {
            POINT pt{ x, y };
            ClientToScreen(h, &pt);
            PostMessageW(h, WM_VCF_MENU_LIST, 0, POINTTOPOINTS(pt));
            return 0;
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        // Only keyboard invocation on the plugin root; mouse uses WM_VCF_MENU_*
        if (!st || g_inPopupMenu) return 0;
        if (GET_X_LPARAM(l) != -1 || GET_Y_LPARAM(l) != -1) return 0;
        POINT pt{}; GetCursorPos(&pt);
        POINT pc = pt; ScreenToClient(h, &pc);
        int listW = ListPaneWidth(h);
        if (pc.x < listW)
            PostMessageW(h, WM_VCF_MENU_LIST, 0, POINTTOPOINTS(pt));
        else if (st->hEdit)
            PostMessageW(h, WM_VCF_MENU_CARD, 0, POINTTOPOINTS(pt));
        return 0;
    }
    case WM_VCF_MENU_LIST: {
        if (!st || g_inPopupMenu) return 0;
        if (st->contacts.empty() || st->sel >= st->contacts.size()) return 0;
        g_inPopupMenu = true;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        const Contact& c = st->contacts[st->sel];
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 10, g_tcRu ? L"Копировать имя" : L"Copy name");
        AppendMenuW(m, MF_STRING, 11, g_tcRu ? L"Копировать телефон" : L"Copy phone");
        AppendMenuW(m, MF_STRING, 12, g_tcRu ? L"Копировать email" : L"Copy email");
        AppendMenuW(m, MF_STRING, 13, g_tcRu ? L"Копировать карточку" : L"Copy card text");
        int cmd = TrackCopyMenu(h, m, pt);
        DestroyMenu(m);
        if (cmd == 10) {
            std::wstring n = ContactDisplayName(c);
            if (!n.empty()) SetClipboardTextW(h, n);
        } else if (cmd == 11) {
            std::wstring p = PrimaryPhone(c);
            if (!p.empty()) SetClipboardTextW(h, p);
        } else if (cmd == 12) {
            std::wstring e = PrimaryEmail(c);
            if (e.empty()) e = FallbackEmail_NotesAware(c);
            if (!e.empty()) SetClipboardTextW(h, e);
        } else if (cmd == 13 && IsWindow(st->hEdit)) {
            int len = GetWindowTextLengthW(st->hEdit);
            std::wstring all((size_t)std::max(0, len), L'\0');
            if (len > 0) GetWindowTextW(st->hEdit, &all[0], len + 1);
            SetClipboardTextW(h, all);
        }
        g_inPopupMenu = false;
        SetFocus(h);
        return 0;
    }
    case WM_VCF_MENU_CARD: {
        if (!st || g_inPopupMenu || !st->hEdit) return 0;
        g_inPopupMenu = true;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, g_tcRu ? L"Копировать" : L"Copy");
        AppendMenuW(m, MF_STRING, 2, g_tcRu ? L"Копировать строку (значение)" : L"Copy line value");
        AppendMenuW(m, MF_STRING, 3, g_tcRu ? L"Копировать всё" : L"Copy all");
        int cmd = TrackCopyMenu(h, m, pt);
        DestroyMenu(m);
        if (cmd == 1) {
            // Copy selection if any; else full field value of current line
            std::wstring sel = GetEditSelectionText(st->hEdit);
            if (!sel.empty()) SetClipboardTextW(h, sel);
            else SetClipboardTextW(h, GetEditLineValue(st->hEdit));
        } else if (cmd == 2) {
            // Always full line value, ignore selection (#19)
            SetClipboardTextW(h, GetEditLineValue(st->hEdit));
        } else if (cmd == 3) {
            int len = GetWindowTextLengthW(st->hEdit);
            std::wstring all((size_t)std::max(0, len), L'\0');
            if (len > 0) GetWindowTextW(st->hEdit, &all[0], len + 1);
            SetClipboardTextW(h, all);
        }
        g_inPopupMenu = false;
        SetFocus(h);
        return 0;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:   // checkbox "📷 photo" label (#20)
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        HWND hCtl = (HWND)l;
        // Photo filter checkbox sits on list background — light text in dark theme
        if (st && st->hPhotoOnly && hCtl == st->hPhotoOnly) {
            SetTextColor(dc, g_clrTxt);
            SetBkMode(dc, TRANSPARENT);
            // Transparent so list-area paint shows through
            return (INT_PTR)GetStockObject(HOLLOW_BRUSH);
        }
        SetTextColor(dc, g_clrTxt);
        SetBkColor(dc, g_clrBk);
        SetBkMode(dc, OPAQUE);
        return (INT_PTR)(g_hbrBk ? g_hbrBk : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)w; RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_hbrBk ? g_hbrBk : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        HBRUSH wbg = g_hbrBk ? g_hbrBk : (HBRUSH)(COLOR_WINDOW + 1);
        FillRect(mem, &rc, wbg);

        int listW = ListPaneWidth(h);
        int statusH = S(h, 18);
        int filterH = S(h, 26);
        int filterPad = S(h, 4);
        int listTop = filterPad + filterH + filterPad;
        int listH = std::max(0, (int)rc.bottom - statusH - listTop);
        // list background under filter area (filter is a child control)
        {
            HBRUSH fbg = CreateSolidBrush(g_clrListBg);
            RECT fr{ rc.left, rc.top, listW, listTop };
            FillRect(mem, &fr, fbg); DeleteObject(fbg);
        }
        RenderList(mem, h, st, rc.left, listTop, listW, listH, st->listRc);

        // Splitter (slightly wider visual so it's obvious and draggable)
        int splitW = std::max(2, S(h, 3));
        HBRUSH sepBr = CreateSolidBrush(g_clrSeparator);
        RECT sep{ listW - splitW / 2, rc.top, listW + splitW / 2 + 1, rc.bottom };
        FillRect(mem, &sep, sepBr); DeleteObject(sepBr);

        // Status bar under the list
        if (statusH > 0) {
            RECT srect{ rc.left, rc.bottom - statusH, listW, rc.bottom };
            HBRUSH sbr = CreateSolidBrush(g_clrListBg);
            FillRect(mem, &srect, sbr); DeleteObject(sbr);

            HFONT oldf = (HFONT)SelectObject(mem, st->fonts.hSmall);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, g_clrSub);
            int visN = VisibleCount(st);
            int totalN = (int)st->contacts.size();
            std::wstring stxt = g_tcRu ? L"Контактов: " : L"Contacts: ";
            if (visN != totalN) {
                stxt += std::to_wstring(visN);
                stxt += L"/";
                stxt += std::to_wstring(totalN);
            } else {
                stxt += std::to_wstring(totalN);
            }
            if (!st->contacts.empty()) {
                stxt += L"  (";
                stxt += std::to_wstring(st->sel + 1);
                stxt += L"/";
                stxt += std::to_wstring(totalN);
                stxt += L")";
            }
            if (st->filterPhotoOnly) {
                stxt += g_tcRu ? L"  📷" : L"  📷";
            }
            if (!st->searchNeedle.empty()) {
                stxt += g_tcRu ? L"  |  Найдено: " : L"  |  Found: ";
                stxt += std::to_wstring(st->matchCount);
                if (st->matchCount > 0 && st->matchPos > 0) {
                    stxt += L" (";
                    stxt += std::to_wstring(st->matchPos);
                    stxt += L"/";
                    stxt += std::to_wstring(st->matchCount);
                    stxt += L")";
                }
            }
            RECT tr = srect;
            tr.left += S(h, 6);
            tr.right -= S(h, 6);
            DrawTextW(mem, stxt.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(mem, oldf);
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
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc); reg = true;
    }
    HWND h = CreateWindowExW(0, L"VCF_VIEW_CLASS", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h) VCFView_SetContacts(h, contacts);
    return h;
}
void VCFView_SetContacts(HWND h, const std::vector<Contact>& contacts) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); if (!st) return;
    st->contacts = contacts; st->sel = 0; st->listScroll = 0;
    st->rightScroll = 0;
    st->searchNeedle.clear();
    st->matchFlags.clear();
    st->matchCount = 0;
    st->matchPos = 0;
    LoadViewSettings(st);
    RebuildVisibleList(st);
    UpdateRightPanel(st);
    InvalidateRect(h, nullptr, FALSE);
    RECT rc; GetClientRect(h, &rc);
    SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
}

extern "C" void VCFView_SetRawBlocks(HWND h, const std::vector<std::wstring>& rawBlocks) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); if (!st) return;
    st->rawBlocks = rawBlocks;
    st->rightScroll = 0;
    // raw blocks affect photo detection for filter
    RebuildVisibleList(st);
    UpdateRightPanel(st);
    InvalidateRect(h, nullptr, FALSE);
    RECT rc; GetClientRect(h, &rc);
    SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
}

size_t VCFView_Count(HWND h) { auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); return st ? st->contacts.size() : 0; }
size_t VCFView_GetSelection(HWND h) { auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); return st ? st->sel : 0; }
void VCFView_SetSelection(HWND h, size_t idx) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); if (!st) return;
    if (idx < st->contacts.size()) {
        SetSelectionAndReveal(h, st, idx);
        RECT rc; GetClientRect(h, &rc); SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    }
}

// Поиск — по разобранным полям; подсветка совпадений + счётчик в статусбаре
bool VCFView_SearchEx(HWND h, const std::wstring& needle, size_t startIndex, bool backwards, bool matchCase, bool wholeWord, bool wrap) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st || st->contacts.empty() || needle.empty()) return false;

    st->searchNeedle = needle;
    RebuildSearchFlags(st, wholeWord, matchCase);

    const std::wstring n = matchCase ? needle : LowerInvariant(needle);
    const size_t count = st->contacts.size();
    auto nextIndex = [&](size_t i) -> size_t {
        return backwards ? (i == 0 ? count - 1 : i - 1) : (i + 1 == count ? 0 : i + 1);
    };

    auto selectHit = [&](size_t i) -> bool {
        if (i >= st->matchFlags.size() || !st->matchFlags[i]) return false;
        if (!ContactMatchesNeedle(st->contacts[i], n, wholeWord, matchCase)) return false;
        st->sel = i;
        st->rightScroll = 0;
        UpdateMatchPosFromSelection(st);
        EnsureSelVisible(h, st);
        UpdateRightPanel(st);
        if (IsWindow(st->hEdit)) {
            int len = GetWindowTextLengthW(st->hEdit);
            if (len > 0) {
                std::wstring all((size_t)len, L'\0');
                GetWindowTextW(st->hEdit, &all[0], len + 1);
                size_t pos = std::wstring::npos;
                if (matchCase) {
                    pos = all.find(needle);
                } else {
                    std::wstring low = LowerInvariant(all);
                    pos = low.find(n);
                }
                if (pos != std::wstring::npos)
                    SendMessageW(st->hEdit, EM_SETSEL, (WPARAM)pos, (LPARAM)(pos + needle.size()));
            }
        }
        InvalidateRect(h, nullptr, FALSE);
        return true;
    };

    const size_t start = startIndex % count;
    if (wrap) {
        size_t i = start;
        do {
            if (selectHit(i)) return true;
            i = nextIndex(i);
        } while (i != start);
    } else if (backwards) {
        for (size_t i = start + 1; i-- > 0; )
            if (selectHit(i)) return true;
    } else {
        for (size_t i = start; i < count; ++i)
            if (selectHit(i)) return true;
    }

    InvalidateRect(h, nullptr, FALSE); // still show match highlights even if no hit selected
    return false;
}

bool VCFView_Search(HWND h, const std::wstring& needle) {
    const size_t count = VCFView_Count(h);
    if (count == 0 || needle.empty()) return false;
    size_t start = VCFView_GetSelection(h);
    start = (start + 1) % count;
    return VCFView_SearchEx(h, needle, start, /*backwards*/false, /*matchCase*/false, /*wholeWord*/false, /*wrap*/true);
}

bool VCFView_CopyActive(HWND h) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st) return false;
    if (IsWindow(st->hEdit)) {
        DWORD a = 0, b = 0;
        SendMessageW(st->hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if (a != b) { SendMessageW(st->hEdit, WM_COPY, 0, 0); return true; }
        std::wstring v = ValueAfterColon(GetEditCurrentLine(st->hEdit));
        if (!v.empty()) { SetClipboardTextW(h, v); return true; }
    }
    if (st->sel < st->contacts.size()) {
        std::wstring n = ContactDisplayName(st->contacts[st->sel]);
        if (!n.empty()) { SetClipboardTextW(h, n); return true; }
    }
    return false;
}