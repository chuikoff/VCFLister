// vcf_view.cpp — левый список + правый блок: фото (сверху) + EDIT (ниже)
// Вывод ВСЕХ полей vCard (v2.1/v3/v4) c локализацией ключей/TYPE (RU/EN по языку TC)
// 2.1: поддержка QUOTED-PRINTABLE + CHARSET, склейка мягких переносов, PHOTO;ENCODING=BASE64 многострочный
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
#include <map>
#include <locale>  // Добавлено для std::iswspace и локалей (хотя <cwctype> покрывает базовое)

#include "vcf_view.hpp"
#include "vcf_theme.hpp"
#include "vcf_utils.hpp"

// Заглушки на расширения
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
    if constexpr (detail_detect::has_notes<Contact>::value) {
        for (const auto& n : c.notes) { std::wstring f = ExtractEmailFromText(n); if (!f.empty()) return f; }
    }
    return L"";
}

// === Кодеки для vCard 2.1: quoted-printable и конверсия к Unicode ===
static std::vector<BYTE> DecodeQuotedPrintableToBytes(const std::wstring& wsrc) {
    // Берём только младший байт wchar_t (файл ASCII/latin), игнорируя >255
    std::string src; src.reserve(wsrc.size());
    for (wchar_t wc : wsrc) { src.push_back((char)((unsigned)wc & 0xFF)); }

    std::vector<BYTE> out; out.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        char c = src[i];
        if (c == '=') {
            // мягкий перенос строки?
            if (i + 1 < src.size()) {
                if (src[i + 1] == '\r' && i + 2 < src.size() && src[i + 2] == '\n') { i += 3; continue; }
                if (src[i + 1] == '\n') { i += 2; continue; }
            }
            // =HH
            if (i + 2 < src.size()) {
                auto hex = [](char h)->int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    return -1;
                    };
                int hi = hex(src[i + 1]), lo = hex(src[i + 2]);
                if (hi >= 0 && lo >= 0) { out.push_back((BYTE)((hi << 4) | lo)); i += 3; continue; }
            }
            // иначе буквально '='
            out.push_back((BYTE)'='); ++i; continue;
        }
        else if (c == '\r' || c == '\n') {
            // реальный перевод строки превращаем в \n
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            ++i; if (c == '\r' && i < src.size() && src[i] == '\n') ++i;
            continue;
        }
        else {
            out.push_back((BYTE)c); ++i; continue;
        }
    }
    return out;
}

static std::wstring BytesToWide(const std::vector<BYTE>& bytes, UINT codepage) {
    if (bytes.empty()) return L"";
    int need = MultiByteToWideChar(codepage, 0, (LPCCH)bytes.data(), (int)bytes.size(), nullptr, 0);
    if (need <= 0) {
        // как fallback попробуем CP_UTF8, затем 1251
        UINT cps[2] = { CP_UTF8, 1251 };
        for (UINT cp : cps) {
            need = MultiByteToWideChar(cp, 0, (LPCCH)bytes.data(), (int)bytes.size(), nullptr, 0);
            if (need > 0) { codepage = cp; break; }
        }
        if (need <= 0) return L"";
    }
    std::wstring w; w.resize(need);
    MultiByteToWideChar(codepage, 0, (LPCCH)bytes.data(), (int)bytes.size(), &w[0], need);
    return w;
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
        {L"X-MAIDENNAME",             {L"Maiden Name",            L"Девичья фамилия"}},
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

// Склейка значений для:
// - 2.1 QUOTED-PRINTABLE с мягкими переносами (= в конце строки)
// - PHOTO;ENCODING=BASE64 / B — собираем всё до следующей строки со знаком ':'
static std::wstring CollectValuePossiblyMultiline(const std::vector<std::wstring>& lines, size_t& i, const std::wstring& headUp) {
    std::wstring val = (lines[i].find(L':') != std::wstring::npos) ? Trim(lines[i].substr(lines[i].find(L':') + 1)) : L"";
    bool isQP = false;
    std::wstring encVal;
    if (HeaderHasParam(headUp, L"ENCODING", &encVal)) {
        std::wstring e = ToUpperASCII(encVal);
        if (e == L"QUOTED-PRINTABLE") isQP = true;
    }
    bool isB64 = false;
    if (!isQP) {
        std::wstring e;
        if (HeaderHasParam(headUp, L"ENCODING", &e)) {
            std::wstring up = ToUpperASCII(e);
            if (up == L"BASE64" || up == L"B") isB64 = true;
        }
    }

    if (isQP) {
        // Для QP: склеиваем строки, если текущая часть оканчивается '='
        while (true) {
            if (!val.empty() && val.back() == L'=') {
                val.pop_back(); // удалить '='
                if (i + 1 < lines.size()) {
                    ++i;
                    // если следующая строка начинается с пробела/таб — это обычное folding (уже разрулено выше),
                    // но в 2.1 часто просто следующая строка — продолжение.
                    val += Trim(lines[i]);
                    continue;
                }
            }
            break;
        }
    }
    else if (isB64) {
        // Для Base64 (особенно PHOTO): собираем продолжения.
        // Останавливаемся только на строках, которые выглядят как начало нового свойства vCard (WORD:),
        // чтобы не обрываться на случайных ':' внутри загрязнённых/плохих данных.
        while (i + 1 < lines.size()) {
            const std::wstring& nxt = lines[i + 1];
            if (nxt.find(L':') != std::wstring::npos) {
                std::wstring nt = Trim(nxt);
                size_t cp = nt.find(L':');
                if (cp != std::wstring::npos) {
                    std::wstring prop = Trim(nt.substr(0, cp));
                    // Простая эвристика: property name состоит из допустимых символов и выглядит как ключ
                    bool looksLikeProp = !prop.empty() &&
                        prop.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::wstring::npos &&
                        prop.size() >= 2;
                    if (looksLikeProp) break;
                }
            }
            ++i;
            val += Trim(nxt);
        }
    }
    return val;
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

// ===================== Фото: декодер Base64 =====================
static const int* GetB64Table() {
    static int T[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        for (int i = 'A'; i <= 'Z'; ++i) T[i] = i - 'A';
        for (int i = 'a'; i <= 'z'; ++i) T[i] = i - 'a' + 26;
        for (int i = '0'; i <= '9'; ++i) T[i] = i - '0' + 52;
        T[(unsigned)'+'] = 62;
        T[(unsigned)'/'] = 63;
        inited = true;
    }
    return T;
}

static std::vector<BYTE> Base64Decode(const std::wstring& wsrc) {
    const int* T = GetB64Table();

    std::vector<BYTE> out; out.reserve(wsrc.size() * 3 / 4);
    int val = 0, valb = -8;
    for (wchar_t wc : wsrc) {
        if (wc == L'=' || wc == L'\r' || wc == L'\n' || wc == L' ' || wc == L'\t') {
            if (wc == L'=') break; // stop on padding, don't process further
            continue;
        }
        if (wc > 255) continue;
        int d = T[(unsigned char)wc];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((BYTE)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// Загрузка фото из raw (поддержка 2.1: многострочный Base64)
static std::unique_ptr<Gdiplus::Bitmap> BitmapFromMemory(const std::vector<uint8_t>& bytes);

static std::unique_ptr<Gdiplus::Bitmap> LoadPhotoFromRaw(const std::wstring& raw) {
    auto lines0 = SplitLines(raw);
    auto lines = UnfoldVCard_Folded(lines0);

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& L = lines[i];
        if (L.empty()) continue;
        if (IsSection(L, L"BEGIN:VCARD") || IsSection(L, L"END:VCARD") || IsSection(L, L"VERSION")) continue;

        size_t colon = L.find(L':'); if (colon == std::wstring::npos) continue;
        std::wstring head = Trim(L.substr(0, colon));
        std::wstring headUpFull = ToUpperASCII(head);
        size_t dot = headUpFull.find(L'.');
        std::wstring headUpStripped = (dot != std::wstring::npos) ? headUpFull.substr(dot + 1) : headUpFull;
        if (headUpStripped.rfind(L"PHOTO", 0) != 0) continue;

        // Собираем значение (передаём полный заголовок для ENCODING)
        std::wstring val = CollectValuePossiblyMultiline(lines, i, headUpFull);
        // Доп. очистка base64 — помогает с v2.1 folded + странными токенами вроде ;JPEG (см. contacts (6).vcf)
        std::wstring b64clean;
        b64clean.reserve(val.size());
        for (wchar_t c : val) {
            if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                (c >= L'0' && c <= L'9') || c == L'+' || c == L'/' || c == L'=') b64clean += c;
        }
        // Случай data:... тоже поддержим (v3/v4)
        std::vector<BYTE> bytes;
        std::wstring vUp = ToUpperASCII(val);
        size_t dataPos = vUp.find(L"DATA:");
        if (dataPos == 0) {
            size_t comma = val.find(L',');
            if (comma != std::wstring::npos) {
                std::wstring b64 = val.substr(comma + 1);
                // clean for safety
                std::wstring bc; for (wchar_t c : b64) if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'||c=='=') bc+=c;
                bytes = Base64Decode(bc.empty() ? b64 : bc);
            }
        }
        else {
            // BASE64 (2.1/3/4)
            bytes = Base64Decode(b64clean.empty() ? val : b64clean);
        }

        if (!bytes.empty()) {
            // Delegate to common loader (fixes ownership/stream timing for reliable decode+render)
            return BitmapFromMemory(bytes);
        }
        // URL мы не загружаем
        break; // берём только первый PHOTO
    }
    return nullptr;
}

// Создать Bitmap из сырых байтов изображения
static std::unique_ptr<Gdiplus::Bitmap> BitmapFromMemory(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hMem) return nullptr;
    void* p = GlobalLock(hMem);
    if (!p) {
        GlobalFree(hMem);
        return nullptr;
    }
    memcpy(p, bytes.data(), bytes.size());
    GlobalUnlock(hMem);
    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) != S_OK) {  // TRUE: stream will free hMem on Release()
        GlobalFree(hMem);
        return nullptr;
    }
    std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromStream(pStream));
    // Best-effort decode for real-world (sometimes slightly corrupt) vCard photos.
    // Query dimensions even if status != Ok; many JPEGs with minor issues still report size.
    bool good = false;
    UINT w = 0, h = 0;
    if (bmp) {
        // Always try to get size — force materialization
        w = bmp->GetWidth();
        h = bmp->GetHeight();
        if (w > 0 && h > 0) good = true;
    }
    if (good && w > 0 && h > 0) {
        // Extra force: lock bits
        BitmapData bd{};
        Rect r(0, 0, (INT)w, (INT)h);
        if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
            bmp->UnlockBits(&bd);
        }
        // Clone to a fully independent Bitmap (owns its own pixel buffer).
        Bitmap* cloned = bmp->Clone(Rect(0, 0, (INT)w, (INT)h), PixelFormat32bppARGB);
        if (cloned && cloned->GetLastStatus() == Ok) {
            pStream->Release();
            return std::unique_ptr<Gdiplus::Bitmap>(cloned);
        }
        // If clone failed but we have positive size, still try to return the original bmp (best effort)
        pStream->Release();
        return bmp;
    }
    pStream->Release();
    return nullptr;
}

// Быстрая проверка: есть ли PHOTO в raw-блоке
static bool RawBlockHasPhoto(const std::wstring& raw) {
    auto lines0 = SplitLines(raw);
    auto lines = UnfoldVCard_Folded(lines0);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& L = lines[i];
        if (L.empty()) continue;
        if (IsSection(L, L"BEGIN:VCARD") || IsSection(L, L"END:VCARD") || IsSection(L, L"VERSION")) continue;
        size_t colon = L.find(L':'); if (colon == std::wstring::npos) continue;
        std::wstring head = Trim(L.substr(0, colon));
        std::wstring headUp = ToUpperASCII(head);
        size_t dot = headUp.find(L'.');
        if (dot != std::wstring::npos) headUp = headUp.substr(dot + 1);
        if (headUp.rfind(L"PHOTO", 0) == 0) return true;
    }
    return false;
}

// ===================== Сборка текста с локализацией и X-ABLabel значениями =====================
static std::wstring BuildFromRawBlock(const std::wstring& raw, bool ru) {
    auto lines0 = SplitLines(raw);
    auto lines = UnfoldVCard_Folded(lines0);

    // Pre-collect X-ABLABELs to attach nice labels to itemN. fields instead of separate "Label:" lines
    std::map<std::wstring, std::wstring> itemLabels;
    for (size_t j = 0; j < lines.size(); ++j) {
        const std::wstring& LL = lines[j];
        if (LL.empty()) continue;
        size_t ppos = LL.find(L':');
        if (ppos == std::wstring::npos) continue;
        std::wstring h = Trim(LL.substr(0, ppos));
        std::wstring hUp = ToUpperASCII(h);
        if (hUp.find(L"X-ABLABEL") == std::wstring::npos) continue;
        size_t jj = j;
        std::wstring v = CollectValuePossiblyMultiline(lines, jj, hUp);
        // translate like before
        std::wstring vUp = ToUpperASCII(v);
        auto stripApple = [](std::wstring s) -> std::wstring {
            if (s.find(L"_$!<") == 0 && s.size() > 4 && s.rfind(L">!$_") == s.size()-4) return s.substr(4, s.size()-8);
            if (s.find(L"$!<") == 0 && s.rfind(L">!$") == s.size()-3) return s.substr(3, s.size()-6);
            return s;
        };
        std::wstring norm = stripApple(vUp);
        std::wstring translated = norm; // fallback raw
        if (norm == L"HomePage") translated = ru ? L"Домашняя страница" : L"Home page";
        else if (norm == L"Anniversary") translated = ru ? L"Годовщина" : L"Anniversary";
        else if (norm == L"Mother") translated = ru ? L"Мать" : L"Mother";
        else if (norm == L"Father") translated = ru ? L"Отец" : L"Father";
        // find item prefix for this label
        size_t d = h.find(L'.');
        if (d != std::wstring::npos) {
            std::wstring item = h.substr(0, d+1); // "item1."
            itemLabels[item] = translated;
        }
    }

    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& L = lines[i];
        if (L.empty()) continue;
        if (IsSection(L, L"BEGIN:VCARD"))   continue;
        if (IsSection(L, L"END:VCARD"))     continue;
        if (IsSection(L, L"VERSION"))       continue;

        size_t pos = L.find(L':');
        if (pos == std::wstring::npos) { out += L; out += L"\r\n"; continue; }

        std::wstring head = Trim(L.substr(0, pos));
        std::wstring headUp = ToUpperASCII(head);
        // Strip itemN. / group prefix so checks work for Apple-style "item4.PHOTO", "item1.X-ABLABEL" etc.
        {
            size_t dot = headUp.find(L'.');
            if (dot != std::wstring::npos) headUp = headUp.substr(dot + 1);
        }
        // Skip PHOTO fields — displayed separately in the photo panel.
        // Only skip embedded/base64 photos (to avoid dumping huge base64 into text).
        // URI photos (vCard 4 / some v3) should be shown as "Photo: https://..."
        if (headUp.rfind(L"PHOTO", 0) == 0) {
            std::wstring rawVal = (pos != std::wstring::npos && pos + 1 < L.size()) ? Trim(L.substr(pos + 1)) : L"";
            bool isUriPhoto = rawVal.find(L"http://") == 0 || rawVal.find(L"https://") == 0 || rawVal.find(L"data:") == 0;
            bool looksLikeBase64 = !isUriPhoto && (rawVal.size() > 60 || rawVal.find(L'/') == 0 || rawVal.find(L"9j") == 0);
            if (!isUriPhoto && looksLikeBase64) continue;
            // URI or short photo value -> let it through to be displayed
        }
        // Skip X-ABLABEL lines themselves (we attach their value to the item field above)
        if (headUp.find(L"X-ABLABEL") != std::wstring::npos) continue;

        std::wstring val = CollectValuePossiblyMultiline(lines, i, headUp);

        // Clean structured fields: remove empty ;;; parts for nicer display (N, ADR etc.)
        if (headUp.find(L"N") == 0 || headUp.find(L"ADR") == 0) {
            std::vector<std::wstring> parts;
            size_t start = 0;
            while (start <= val.size()) {
                size_t semi = val.find(L';', start);
                if (semi == std::wstring::npos) { parts.push_back(Trim(val.substr(start))); break; }
                parts.push_back(Trim(val.substr(start, semi - start)));
                start = semi + 1;
            }
            std::wstring cleaned;
            for (auto& p : parts) {
                if (!p.empty()) {
                    if (!cleaned.empty()) cleaned += (headUp.find(L"N") == 0 ? L" " : L", ");
                    cleaned += p;
                }
            }
            if (!cleaned.empty()) val = cleaned;
        }

        // Skip empty N/FN lines (e.g. N:;;;; or FN: ) to avoid "Name: " or "Full name: "
        if (val.empty() && (headUp.find(L"N") == 0 || headUp == L"FN")) continue;

        // Clean ugly Android custom lines a bit (strip vnd prefix and trailing ;;;;;;;;; )
        if (headUp.find(L"X-ANDROID-CUSTOM") == 0) {
            std::vector<std::wstring> parts;
            size_t st = 0;
            while (true) {
                size_t s = val.find(L';', st);
                if (s == std::wstring::npos) {
                    parts.push_back(Trim(val.substr(st)));
                    break;
                }
                parts.push_back(Trim(val.substr(st, s - st)));
                st = s + 1;
            }
            if (!parts.empty()) {
                std::wstring type = parts[0];
                if (type.find(L"vnd.android.cursor.item/") == 0) type = type.substr(24);
                std::wstring date = parts.size() > 1 ? parts[1] : L"";
                std::wstring lbl = L"";
                if (parts.size() > 3 && !parts[3].empty()) lbl = parts[3];
                else if (parts.size() > 2 && !parts[2].empty() && parts[2] != L"0" && parts[2] != L"1") lbl = parts[2];
                size_t scp = lbl.find(L';');
                if (scp != std::wstring::npos) lbl = lbl.substr(0, scp);
                std::wstring res = type;
                if (!lbl.empty()) res += L": " + lbl;
                if (!date.empty() && date != L"0" && date.find(L';') == std::wstring::npos) res += L" (" + date + L")";
                val = res;
            }
        }

        // vCard 2.1: QUOTED-PRINTABLE + CHARSET
        std::wstring encVal;
        bool isQP = HeaderHasParam(headUp, L"ENCODING", &encVal) && (ToUpperASCII(encVal) == L"QUOTED-PRINTABLE");
        if (isQP) {
            // Определим кодировку
            std::wstring ch;
            UINT cp = 0;
            if (HeaderHasParam(headUp, L"CHARSET", &ch)) {
                std::wstring up = ToUpperASCII(ch);
                if (up.find(L"UTF-8") != std::wstring::npos || up.find(L"UTF8") != std::wstring::npos) cp = CP_UTF8;
                else if (up.find(L"1251") != std::wstring::npos || up.find(L"WINDOWS-1251") != std::wstring::npos) cp = 1251;
                else if (up.find(L"CP1251") != std::wstring::npos) cp = 1251;
                else if (up.find(L"KOI8") != std::wstring::npos) cp = 20866; // KOI8-R (best-effort)
            }
            auto bytes = DecodeQuotedPrintableToBytes(val);
            val = BytesToWide(bytes, cp ? cp : CP_UTF8);
            if (val.empty()) val = BytesToWide(bytes, 1251); // ещё раз, если UTF-8 не подошёл
        }

        // Специальная обработка соцсетей для более чистого вида
        if (headUp.find(L"X-SOCIALPROFILE") == 0) {
            // Уже "Социальный профиль (twitter): url" из BuildLocalizedHead
        }

        std::wstring label = BuildLocalizedHead(head, ru);

        // If this field has itemN. prefix and we have a custom label for it, use the nice label instead of standard key
        size_t dotPos = head.find(L'.');
        if (dotPos != std::wstring::npos) {
            std::wstring item = head.substr(0, dotPos + 1);
            auto it = itemLabels.find(item);
            if (it != itemLabels.end()) {
                label = it->second;
            }
        }

        // Special handling for the encoded custom fields in this vCard (from ez-vcard sample)
        if (headUp.find(L"X-FCENCODED-") == 0) {
            label = ru ? L"Связанное / Пользовательское" : L"Related / Custom";
        }

        out += label; out += L": "; out += val; out += L"\r\n";
    }
    return out;
}

// ===================== Состояние вьюера =====================
static const wchar_t* kClass = L"VCF_VIEW_CLASS";
static const wchar_t* kPhotoClass = L"VCF_PHOTO_VIEW";

struct FieldHit { RECT rc{}; std::wstring label; std::wstring value; }; // неисп.
struct ViewState {
    std::vector<Contact> contacts;           // для левого списка/поиска
    std::vector<std::wstring> rawBlocks;     // сырые vCard-блоки
    size_t sel = 0;

    int listScroll = 0;
    int listItemH = 0;
    RECT listRc{};
    int  perPage = 1;

    HWND hScroll = nullptr;
    HWND hPhoto = nullptr;                  // окно превью фото
    HWND hEdit = nullptr;

    int rightScroll = 0;
    HWND hRightScroll = nullptr;            // scrollbar for right panel (large photo / long text)

    std::unique_ptr<Gdiplus::Bitmap> photo;  // изображение
    Fonts fonts;
};

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
static int ListPaneWidth(HWND h) { return S(h, 260); }
static void EnsureListMetrics(HWND h, ViewState* st) { if (!st->listItemH) st->listItemH = S(h, 52); }

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

    int sbw = DlgSBW();
    int wList = w - sbw; if (wList < S(h, 120)) wList = w;

    HBRUSH bg = CreateSolidBrush(g_clrListBg);
    RECT rbg{ x,y,x + wList,y + hgt }; FillRect(dc, &rbg, bg); DeleteObject(bg);

    int pad = S(h, 8);
    int innerTop = y + pad;
    int innerH = hgt - pad - pad;
    st->perPage = std::max<int>(1, innerH / st->listItemH);

    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max<int>(0, (int)st->contacts.size() - st->perPage);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;

    int ycur = innerTop;
    for (int row = 0; row < st->perPage && st->listScroll + row < (int)st->contacts.size(); ++row) {
        size_t idx = (size_t)(st->listScroll + row);
        const Contact& c = st->contacts[idx];

        std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
        if (name.empty()) {
            // Show empty cards explicitly
            name = g_tcRu ? L"(пустая карточка)" : L"(empty card)";
        }

        // Индикатор фото
        bool hasPhoto = false;
        if (c.photo.has_value() || !c.photo_url.empty()) hasPhoto = true;
        else if (idx < st->rawBlocks.size()) hasPhoto = RawBlockHasPhoto(st->rawBlocks[idx]);
        if (hasPhoto) name += L" 📷";

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

        HPEN pen = CreatePen(PS_SOLID, 1, g_clrGrid); HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, item.left, item.bottom, nullptr); LineTo(dc, item.right, item.bottom);
        SelectObject(dc, oldPen); DeleteObject(pen);

        HFONT old = (HFONT)SelectObject(dc, st->fonts.hNorm);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_clrTxt);
        RECT nameRc = item; nameRc.left += S(h, 8); nameRc.top += S(h, 6); nameRc.right -= S(h, 6);
        DrawTextW(dc, name.c_str(), (int)name.size(), &nameRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, st->fonts.hSmall);
        SetTextColor(dc, g_clrSub);
        if (sub.empty()) {
            std::wstring fb = FallbackEmail_NotesAware(c);
            if (!fb.empty()) sub = L"Email: " + fb;
        }
        RECT subRc = nameRc; subRc.top = nameRc.top + S(h, 20);
        DrawTextW(dc, sub.c_str(), (int)sub.size(), &subRc, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);

        ycur += st->listItemH;
    }

    outListRc = RECT{ x,y,x + wList,y + hgt };
    UpdateListScrollbar(st, (int)st->contacts.size());
}

// ===================== EDIT и ФОТО: наполнение и поведение =====================
static void UpdateRightPanel(ViewState* st) {
    if (!st) return;
    // фото: приоритет — встроенное в Contact, затем rawBlocks
    st->photo.reset();
    if (st->sel < st->contacts.size()) {
        const Contact& c = st->contacts[st->sel];
        if (c.photo.has_value() && !c.photo->bytes.empty()) {
            st->photo = BitmapFromMemory(c.photo->bytes);
        }
    }
    if (!st->photo && st->sel < st->rawBlocks.size()) {
        st->photo = LoadPhotoFromRaw(st->rawBlocks[st->sel]);
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

    // текст
    if (IsWindow(st->hEdit)) {
        std::wstring text;
        if (st->sel < st->rawBlocks.size() && !st->rawBlocks[st->sel].empty()) {
            text = BuildFromRawBlock(st->rawBlocks[st->sel], g_tcRu);
        }
        else if (st->sel < st->contacts.size()) {
            const Contact& c = st->contacts[st->sel];
            auto add = [&](const std::wstring& k, const std::wstring& v) { if (!v.empty()) { text += k; text += v; text += L"\r\n"; } };
            std::wstring name = !c.fn.empty() ? c.fn : (c.n_given + (c.n_family.empty() ? L"" : L" ") + c.n_family);
            if (name.empty()) name = L"(no name)";
            add(g_tcRu ? L"Имя" : L"Name", name);
            add(g_tcRu ? L"Компания" : L"Organization", c.org);
            add(g_tcRu ? L"Должность" : L"Role", c.title);
            add(L"URL", c.url);
            add(g_tcRu ? L"День рождения" : L"Birthday", c.bday);
            for (auto& p : c.phones) if (!p.number.empty()) add(g_tcRu ? L"Телефон" : L"Phone", p.number);
            bool any = false; for (auto& e : c.emails) { if (!e.addr.empty()) { add(L"Email", e.addr); any = true; } }
            if (!any) { std::wstring fb = FallbackEmail_NotesAware(c); if (!fb.empty()) add(L"Email", fb); }
            for (auto& a : c.addrs) if (!a.text.empty()) add(g_tcRu ? L"Адрес" : L"Address", a.text);
            if constexpr (detail_detect::has_notes<Contact>::value) { for (auto& n : c.notes) add(g_tcRu ? L"Заметка" : L"Note", n); }
            else if (!c.note.empty()) { add(g_tcRu ? L"Заметка" : L"Note", c.note); }
        }
        else {
            text = L"";
        }
        SendMessageW(st->hEdit, WM_SETTEXT, 0, (LPARAM)text.c_str());
        SendMessageW(st->hEdit, EM_SETSEL, 0, 0);
        SendMessageW(st->hEdit, EM_SCROLLCARET, 0, 0);
    }
}

// Сабкласс EDIT — пробрасываем Esc в окно Lister, чтобы закрывалось
static WNDPROC g_EditOldProc = nullptr;
static LRESULT CALLBACK EditSubclassProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CHAR:
        if (wParam == VK_ESCAPE) {
            HWND viewer = GetParent(hEdit);
            HWND lister = viewer ? GetParent(viewer) : nullptr;
            if (lister) { PostMessageW(lister, WM_KEYDOWN, VK_ESCAPE, 0); return 0; }
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE:  // for selection drag
        {
            LRESULT res = CallWindowProcW(g_EditOldProc, hEdit, msg, wParam, lParam);
            // Redirect focus to main view window so TC lister can switch plugins/views (HEX, other plugins etc.)
            // Selection remains visible thanks to ES_NOHIDESEL
            HWND viewer = GetParent(hEdit);
            if (viewer && IsWindow(viewer)) {
                SetFocus(viewer);
            }
            return res;
        }
    }
    return CallWindowProcW(g_EditOldProc, hEdit, msg, wParam, lParam);
}

// Окно превью фото
static LRESULT CALLBACK PhotoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND parent = GetParent(hwnd);
    auto* st = (ViewState*)GetWindowLongPtrW(parent, GWLP_USERDATA);
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
    RECT rc; GetClientRect(h, &rc);
    int statusH = S(h, 18);
    int innerH = (rc.bottom - statusH) - S(h, 16);
    int rowH = st->listItemH ? st->listItemH : S(h, 52);
    int per = std::max<int>(1, innerH / rowH);
    st->perPage = per;

    int sel = (int)st->sel;
    if (sel < st->listScroll) st->listScroll = sel;
    else if (sel >= st->listScroll + per) st->listScroll = sel - (per - 1);

    if (st->listScroll < 0) st->listScroll = 0;
    int maxScroll = std::max<int>(0, (int)st->contacts.size() - per);
    if (st->listScroll > maxScroll) st->listScroll = maxScroll;
}
static void SetSelectionAndReveal(HWND h, ViewState* st, size_t idx) {
    if (!st || st->contacts.empty()) return;
    if (idx >= st->contacts.size()) idx = st->contacts.size() - 1;
    st->sel = idx; st->rightScroll = 0; EnsureSelVisible(h, st);

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
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;

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

        // Скролл слева
        st->hScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100, h, nullptr, GetModuleHandleW(nullptr), nullptr);

        // Правый скроллбар для карточки (большое фото или много текста)
        st->hRightScroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, GetSystemMetrics(SM_CXVSCROLL), 100, h, nullptr, GetModuleHandleW(nullptr), nullptr);

        // Фото сверху справа
        st->hPhoto = CreateWindowExW(0, kPhotoClass, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, h, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);

        // EDIT ниже фото
        // Добавлен WS_VSCROLL для явного скроллбара в карточке при большом объёме данных
        st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL | WS_VSCROLL | WS_HSCROLL,
            0, 0, 0, 0, h, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(st->hEdit, WM_SETFONT, (WPARAM)st->fonts.hNorm, TRUE);
        g_EditOldProc = (WNDPROC)SetWindowLongPtrW(st->hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        SetFocus(h);
        return 0;
    }
    case WM_DESTROY: {
        if (st) {
            if (st->hEdit && IsWindow(st->hEdit)) DestroyWindow(st->hEdit);
            if (st->hPhoto && IsWindow(st->hPhoto)) DestroyWindow(st->hPhoto);
            if (st->hScroll && IsWindow(st->hScroll)) DestroyWindow(st->hScroll);
            if (st->hRightScroll && IsWindow(st->hRightScroll)) DestroyWindow(st->hRightScroll);
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
        int listContentH = rc.bottom - statusH;

        MoveWindow(st->hScroll, listW - sbw, 0, sbw, listContentH, TRUE);

        // Правая колонка
        int pad = S(h, 12);
        int ex = listW + 1 + pad;
        int ew = rc.right - ex - pad;
        if (ew < S(h, 100)) ew = std::max<int>(0, rc.right - (listW + pad));

        // Фото: ширина = min(ew, 1000), высота по содержимому (≤1000). Размер увеличен в ~2 раза по запросу
        int photoW = std::min<int>(ew, 1000);
        int photoH = 0;
        if (st->photo) {
            UINT iw = st->photo->GetWidth();
            UINT ih = st->photo->GetHeight();
            double s = 1.0;
            if (iw > 0 && ih > 0) {
                double sx = static_cast<double>(photoW) / static_cast<double>(iw);
                double sy = 1000.0 / static_cast<double>(ih);
                s = std::min<double>(1.0, std::min<double>(sx, sy));
            }
            photoH = static_cast<int>(std::min<double>(1000.0, static_cast<double>(st->photo ? st->photo->GetHeight() : 0) * s));
        }
        if (st->photo && photoH <= 0) photoH = 100; // min height to show the photo window

        int ey = pad;
        int sep = S(h, 8);

        // Правый скролл для карточки: учитываем rightScroll для позиционирования содержимого
        int rightScroll = st->rightScroll;
        int rightScrollBarW = GetSystemMetrics(SM_CXVSCROLL);

        // Позиция правого скроллбара (справа от правой колонки)
        int rightAreaTop = pad;
        int rightAreaH = rc.bottom - statusH - pad;  // примерно высота правой области с учётом статусбара слева
        if (rightAreaH < 50) rightAreaH = 50;
        MoveWindow(st->hRightScroll, ex + ew, rightAreaTop, rightScrollBarW, rightAreaH, TRUE);

        // Позиционируем фото и EDIT с учётом скролла (для большого фото / длинного текста)
        MoveWindow(st->hPhoto, ex, ey - rightScroll, photoW, (photoH > 0 ? photoH : 0), TRUE);

        int editY = ey + (photoH > 0 ? photoH + sep : 0);
        int eh = (rc.bottom - statusH) - editY - pad;
        MoveWindow(st->hEdit, ex, editY - rightScroll, ew, std::max<int>(0, eh), TRUE);

        // Обновляем диапазон правого скроллбара
        int contentBottom = editY + std::max<int>(0, eh) + pad;
        int maxScroll = std::max(0, contentBottom - rightAreaH);
        SCROLLINFO rsi{};
        rsi.cbSize = sizeof(rsi);
        rsi.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        rsi.nMin = 0;
        rsi.nMax = maxScroll;
        rsi.nPage = rightAreaH / 2;  // примерно
        rsi.nPos = std::min(rightScroll, maxScroll);
        SetScrollInfo(st->hRightScroll, SB_CTL, &rsi, TRUE);
        ShowWindow(st->hRightScroll, (maxScroll > 0) ? SW_SHOW : SW_HIDE);

        // Если текущий скролл больше допустимого — подправим
        if (rightScroll > maxScroll) {
            st->rightScroll = maxScroll;
            // переместим заново (повторный вызов layout не нужен, т.к. мы уже установили)
        }

        InvalidateRect(h, nullptr, FALSE);
        if (st->hPhoto) InvalidateRect(st->hPhoto, nullptr, FALSE);
        return 0;
    }
    case WM_VSCROLL: {
        if (!st) break;
        if ((HWND)l == st->hScroll) {
            int total = (int)st->contacts.size(); if (total <= 0) return 0;
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
            int listW = ListPaneWidth(h);
            int ex = listW + 1 + pad;
            int ew = rc.right - ex - pad;
            if (ew < S(h, 100)) ew = std::max<int>(0, rc.right - (listW + pad));
            int statusH = S(h, 18);
            int rightAreaH = rc.bottom - statusH - pad;
            if (rightAreaH < 50) rightAreaH = 50;

            int pos = st->rightScroll;
            switch (LOWORD(w)) {
            case SB_LINEUP:   pos -= 30; break;
            case SB_LINEDOWN: pos += 30; break;
            case SB_PAGEUP:   pos -= rightAreaH / 2; break;
            case SB_PAGEDOWN: pos += rightAreaH / 2; break;
            case SB_TOP:      pos = 0; break;
            case SB_BOTTOM:   pos = 999999; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(st->hRightScroll, SB_CTL, &si); pos = si.nTrackPos; break;
            }
            }
            st->rightScroll = std::max(0, pos);
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
            int maxScroll = std::max<int>(0, (int)st->contacts.size() - st->perPage);
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
                st->rightScroll = std::max(0, st->rightScroll + step);
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
        if (!st || st->contacts.empty()) return 0;
        size_t sel = st->sel, count = st->contacts.size(); bool handled = false;
        switch (w) {
        case VK_UP:    if (sel > 0) sel--, handled = true; break;
        case VK_DOWN:  if (sel + 1 < count) sel++, handled = true; break;
        case VK_PRIOR: if (st->perPage > 0) sel = (sel > (size_t)st->perPage) ? (sel - (size_t)st->perPage) : 0, handled = true; break;
        case VK_NEXT:  if (st->perPage > 0) sel = std::min<size_t>(count - 1, sel + (size_t)st->perPage), handled = true; break;
        case VK_HOME:  sel = 0; handled = true; break;
        case VK_END:   sel = count ? count - 1 : 0; handled = true; break;
        }
        if (handled) { SetSelectionAndReveal(h, st, sel); return 0; }
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int pad = S(h, 8), listW = ListPaneWidth(h) - GetSystemMetrics(SM_CXVSCROLL);
        if (x >= pad && x < listW - pad) {
            int rowH = st->listItemH ? st->listItemH : S(h, 52);
            int row = (y - pad) / rowH;
            if (row >= 0) {
                size_t idx = (size_t)(st->listScroll + row);
                if (idx < st->contacts.size()) { st->sel = idx; st->rightScroll = 0; UpdateRightPanel(st); InvalidateRect(h, nullptr, FALSE); return 0; }
            }
        }
        return 0;
    }
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE: { RecomputeTheme(); UpdateRightPanel(st); InvalidateRect(h, nullptr, FALSE); return 0; }

                          // ПКМ по EDIT → «Копировать»
    case WM_CONTEXTMENU: {
        if (!st) break;
        HWND hSrc = (HWND)w; POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (hSrc == st->hEdit || (hSrc == h)) {
            if (hSrc == h) { // попали ли в EDIT?
                RECT rcE{}; GetWindowRect(st->hEdit, &rcE);
                if (!(pt.x >= rcE.left && pt.x < rcE.right && pt.y >= rcE.top && pt.y < rcE.bottom)) break;
            }
            HMENU m = CreatePopupMenu(); AppendMenuW(m, MF_STRING, 1, L"\u041A\u043E\u043F\u0438\u0440\u043E\u0432\u0430\u0442\u044C");
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, h, nullptr); DestroyMenu(m);
            if (cmd == 1) {
                DWORD a = 0, b = 0; SendMessageW(st->hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
                if (a != b) SendMessageW(st->hEdit, WM_COPY, 0, 0);
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
        HDC dc = (HDC)w; SetTextColor(dc, g_clrTxt); SetBkColor(dc, g_clrBk);
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
        int listH = rc.bottom - statusH;
        RenderList(mem, h, st, rc.left, rc.top, listW, listH, st->listRc);

        HBRUSH sepBr = CreateSolidBrush(g_clrSeparator);
        RECT sep{ listW, rc.top, listW + 1, rc.bottom }; FillRect(mem, &sep, sepBr); DeleteObject(sepBr);

        // Status bar under the list (total contacts count)
        if (statusH > 0) {
            RECT srect{ rc.left, rc.bottom - statusH, listW, rc.bottom };
            HBRUSH sbr = CreateSolidBrush(g_clrListBg);
            FillRect(mem, &srect, sbr); DeleteObject(sbr);

            HFONT oldf = (HFONT)SelectObject(mem, st->fonts.hSmall);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, g_clrSub);
            std::wstring stxt = g_tcRu ? L"Контактов: " : L"Contacts: ";
            stxt += std::to_wstring((int)st->contacts.size());
            if (!st->contacts.empty()) {
                stxt += g_tcRu ? L"  (" : L"  (";
                stxt += std::to_wstring(st->sel + 1);
                stxt += L"/";
                stxt += std::to_wstring((int)st->contacts.size());
                stxt += L")";
            }
            RECT tr = srect;
            tr.left += S(h, 6);
            tr.right -= S(h, 6);
            DrawTextW(mem, stxt.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
    UpdateRightPanel(st);
    InvalidateRect(h, nullptr, FALSE);
    // Force layout of photo/edit children now that photo may be available
    RECT rc; GetClientRect(h, &rc);
    SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
}

extern "C" void VCFView_SetRawBlocks(HWND h, const std::vector<std::wstring>& rawBlocks) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); if (!st) return;
    st->rawBlocks = rawBlocks;
    st->rightScroll = 0;
    UpdateRightPanel(st);
    InvalidateRect(h, nullptr, FALSE);
    // Force layout of photo/edit children now that photo may be available from raw
    RECT rc; GetClientRect(h, &rc);
    SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
}

size_t VCFView_Count(HWND h) { auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); return st ? st->contacts.size() : 0; }
size_t VCFView_GetSelection(HWND h) { auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); return st ? st->sel : 0; }
void VCFView_SetSelection(HWND h, size_t idx) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA); if (!st) return;
    if (idx < st->contacts.size()) { st->sel = idx; st->rightScroll = 0; EnsureSelVisible(h, st); UpdateRightPanel(st); InvalidateRect(h, nullptr, FALSE); 
        RECT rc; GetClientRect(h, &rc); SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom)); }
}

// Поиск — по разобранным полям (как было)
bool VCFView_SearchEx(HWND h, const std::wstring& needle, size_t startIndex, bool backwards, bool /*matchCase*/, bool wholeWord, bool wrap) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st || st->contacts.empty() || needle.empty()) return false;

    auto norm = [&](const std::wstring& x) { return LowerInvariant(x); };
    std::wstring n = norm(needle);

    auto buildHay = [&](const Contact& c) {
        std::wstring hstr;
        auto add = [&](const std::wstring& s) { if (!s.empty()) { hstr += L" "; hstr += norm(s); } };
        add(c.fn); add(c.n_given); add(c.n_family); add(c.org); add(c.title); add(c.bday); add(c.url); add(c.note);
        if constexpr (detail_detect::has_notes<Contact>::value) { for (auto& t : c.notes) add(t); }
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
                st->sel = i; EnsureSelVisible(h, st); UpdateRightPanel(st); InvalidateRect(h, nullptr, FALSE);
                return true;
            }
            pos = hay.find(n, pos + 1);
        }
        i = nextIndex(i);
    } while (wrap && i != first);

    return false;
}

bool VCFView_CopyActive(HWND) { return false; }