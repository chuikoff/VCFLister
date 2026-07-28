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
#include <commctrl.h>
#include <wininet.h>
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wininet.lib")

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
            // soft-break: "=\r\n", "=\n", or trailing '=' at end of value (vCard 2.1)
            if (i + 1 >= src.size()) break;
            if (i + 1 < src.size()) {
                if (src[i + 1] == '\r' && i + 2 < src.size() && src[i + 2] == '\n') { i += 3; continue; }
                if (src[i + 1] == '\n' || src[i + 1] == '\r') { i += 2; continue; }
                if (src[i + 1] == ' ' || src[i + 1] == '\t') { i += 2; continue; }
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

// Technical / binary Apple & iOS fields that should not appear as card text
static bool IsNoiseField(const std::wstring& headUp) {
    static const wchar_t* noise[] = {
        L"X-ADDRESSING-GRAMMAR",
        L"X-SHARED-PHOTO-DISPLAY-PREF",
        L"X-IMAGETYPE",
        L"X-IMAGEHASH",
        L"X-ABUID",
        L"X-ABSHOWAS",
        L"UID",
        L"PRODID",
        L"CLIENTPIDMAP",
    };
    for (auto* n : noise) {
        if (headUp.rfind(n, 0) == 0) return true;
    }
    return false;
}

static bool LooksLikeBinaryBlob(const std::wstring& v) {
    if (v.size() < 80) return false;
    // long base64-ish payload (Apple X-ADDRESSING-GRAMMAR etc.)
    size_t b64 = 0, total = 0;
    for (wchar_t c : v) {
        if (c == L'\r' || c == L'\n' || c == L' ') continue;
        ++total;
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
            (c >= L'0' && c <= L'9') || c == L'+' || c == L'/' || c == L'=' || c == L'-' || c == L'_')
            ++b64;
    }
    return total >= 80 && b64 * 10 >= total * 9;
}

// ===================== Сборка текста с локализацией и X-ABLabel значениями =====================
static std::wstring BuildFromRawBlock(const std::wstring& raw, bool ru) {
    auto lines0 = SplitLines(raw);
    auto lines = UnfoldVCard_Folded(lines0);

    // Pre-collect X-ABLABELs to attach nice labels to itemN. fields instead of separate "Label:" lines
    std::map<std::wstring, std::wstring> itemLabels;
    auto stripApple = [](std::wstring s) -> std::wstring {
        if (s.find(L"_$!<") == 0 && s.size() > 4 && s.rfind(L">!$_") == s.size() - 4)
            return s.substr(4, s.size() - 8);
        if (s.find(L"$!<") == 0 && s.rfind(L">!$") == s.size() - 3)
            return s.substr(3, s.size() - 6);
        return s;
    };
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
        // Keep original casing for custom labels (e.g. «Домашние контакты», «День ангела»)
        std::wstring pretty = stripApple(v);
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
        size_t d = h.find(L'.');
        if (d != std::wstring::npos) {
            itemLabels[h.substr(0, d + 1)] = pretty;
        }
    }

    // Prefer FN; skip structured N when it equals FN (Apple often has N:;Name;;; + FN:Name)
    std::wstring fnVal;
    for (size_t j = 0; j < lines.size(); ++j) {
        const std::wstring& LL = lines[j];
        size_t ppos = LL.find(L':');
        if (ppos == std::wstring::npos) continue;
        std::wstring hUp = ToUpperASCII(Trim(LL.substr(0, ppos)));
        size_t dot = hUp.find(L'.');
        if (dot != std::wstring::npos) hUp = hUp.substr(dot + 1);
        if (hUp == L"FN" || hUp.rfind(L"FN;", 0) == 0) {
            size_t jj = j;
            fnVal = Trim(CollectValuePossiblyMultiline(lines, jj, hUp));
            break;
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
        // Skip technical / binary Apple fields
        if (IsNoiseField(headUp)) continue;
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
        if (LooksLikeBinaryBlob(val)) continue;
        if (val.empty()) continue;

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
        if (val.empty() && (headUp == L"N" || headUp.rfind(L"N;", 0) == 0 || headUp == L"FN")) continue;
        if ((headUp == L"N" || headUp.rfind(L"N;", 0) == 0) && !fnVal.empty() && val == fnVal)
            continue;

        // vCard 4.0: nicer GENDER / KIND display values
        if (headUp == L"GENDER" || headUp == L"X-GENDER") {
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
        }
        else if (headUp == L"KIND") {
            std::wstring k = ToUpperASCII(Trim(val));
            if (k == L"INDIVIDUAL") val = ru ? L"Человек" : L"Individual";
            else if (k == L"GROUP") val = ru ? L"Группа" : L"Group";
            else if (k == L"ORG" || k == L"ORGANIZATION") val = ru ? L"Организация" : L"Organization";
            else if (k == L"LOCATION") val = ru ? L"Место" : L"Location";
            else if (k == L"DEVICE") val = ru ? L"Устройство" : L"Device";
            else if (k == L"APPLICATION") val = ru ? L"Приложение" : L"Application";
        }

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

static bool ContactHasPhoto(const ViewState* st, size_t idx) {
    if (!st || idx >= st->contacts.size()) return false;
    const Contact& c = st->contacts[idx];
    if (c.photo.has_value() && !c.photo->bytes.empty()) return true;
    if (!c.photo_url.empty()) return true;
    if (idx < st->rawBlocks.size() && RawBlockHasPhoto(st->rawBlocks[idx])) return true;
    return false;
}

static bool ContactMatchesNeedle(const Contact& c, const std::wstring& needleNorm, bool wholeWord);
static void SetSelectionAndReveal(HWND h, ViewState* st, size_t idx);

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

// Full text height of multiline EDIT (no internal V-scroll — outer card scroll only)
static int MeasureEditContentHeight(HWND hEdit, int widthPx) {
    if (!hEdit || !IsWindow(hEdit) || widthPx <= 8) return 40;
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return 40;

    // Prefer actual EDIT metrics after width is applied (matches wrap of multiline control)
    RECT client{};
    GetClientRect(hEdit, &client);
    // Temporarily ensure width for EM_GETLINECOUNT-based estimate
    int lineCount = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0);
    HDC dc = GetDC(hEdit);
    int lineH = 16;
    if (dc) {
        HFONT hf = (HFONT)SendMessageW(hEdit, WM_GETFONT, 0, 0);
        HFONT old = hf ? (HFONT)SelectObject(dc, hf) : nullptr;
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) lineH = tm.tmHeight + tm.tmExternalLeading;
        // DrawText fallback for wrapped long lines (EM_GETLINECOUNT undercounts before layout)
        std::wstring text((size_t)len, L'\0');
        GetWindowTextW(hEdit, &text[0], len + 1);
        RECT rc{ 0, 0, std::max(8, widthPx - 12), 0 };
        DrawTextW(dc, text.c_str(), len, &rc,
            DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EDITCONTROL | DT_EXPANDTABS);
        int byDraw = (rc.bottom - rc.top) + 16;
        if (old) SelectObject(dc, old);
        ReleaseDC(hEdit, dc);
        int byLines = (lineCount > 0 ? lineCount : 1) * lineH + 16;
        int h = std::max(byDraw, byLines);
        if (h < 40) h = 40;
        if (h > 20000) h = 20000;
        return h;
    }
    int h = (lineCount > 0 ? lineCount : 1) * lineH + 16;
    if (h < 40) h = 40;
    if (h > 20000) h = 20000;
    return h;
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

static bool ContactMatchesNeedle(const Contact& c, const std::wstring& needleNorm, bool wholeWord) {
    if (needleNorm.empty()) return false;
    auto norm = [&](const std::wstring& x) { return LowerInvariant(x); };
    std::wstring hay;
    auto add = [&](const std::wstring& s) { if (!s.empty()) { hay += L' '; hay += norm(s); } };
    add(c.fn); add(c.n_given); add(c.n_family); add(c.org); add(c.title); add(c.bday); add(c.url); add(c.note);
    add(c.gender); add(c.lang); add(c.kind);
    for (auto& t : c.notes) add(t);
    for (auto& t : c.phones) { add(t.number); for (auto& tp : t.types) add(tp); }
    for (auto& e : c.emails) { add(e.addr); for (auto& tp : e.types) add(tp); }
    for (auto& a : c.addrs) add(a.text);
    for (auto& u : c.urls) add(u);
    for (auto& lg : c.langs) add(lg);
    for (auto& m : c.members) add(m);

    size_t pos = hay.find(needleNorm);
    while (pos != std::wstring::npos) {
        if (!wholeWord || (isWordBoundary(hay, pos) && isWordBoundary2(hay, pos + needleNorm.size())))
            return true;
        pos = hay.find(needleNorm, pos + 1);
    }
    return false;
}

static void RebuildSearchFlags(ViewState* st, bool wholeWord = false) {
    if (!st) return;
    st->matchFlags.assign(st->contacts.size(), 0);
    st->matchCount = 0;
    st->matchPos = 0;
    if (st->searchNeedle.empty()) return;
    std::wstring n = LowerInvariant(st->searchNeedle);
    for (size_t i = 0; i < st->contacts.size(); ++i) {
        if (ContactMatchesNeedle(st->contacts[i], n, wholeWord)) {
            st->matchFlags[i] = 1;
            st->matchCount++;
        }
    }
    if (st->sel < st->matchFlags.size() && st->matchFlags[st->sel]) {
        int p = 0;
        for (size_t i = 0; i <= st->sel; ++i) if (st->matchFlags[i]) ++p;
        st->matchPos = p;
    }
}

// Extract "value" part after first ':' from a detail line (for copy)
static std::wstring ValueAfterColon(const std::wstring& line) {
    size_t p = line.find(L':');
    if (p == std::wstring::npos) return Trim(line);
    return Trim(line.substr(p + 1));
}

static std::wstring GetEditCurrentLine(HWND hEdit) {
    DWORD a = 0, b = 0;
    SendMessageW(hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    int line = (int)SendMessageW(hEdit, EM_LINEFROMCHAR, a, 0);
    int idx = (int)SendMessageW(hEdit, EM_LINEINDEX, line, 0);
    if (idx < 0) return L"";
    wchar_t buf[2048];
    *(WORD*)buf = 2047;
    int n = (int)SendMessageW(hEdit, EM_GETLINE, line, (LPARAM)buf);
    if (n < 0) n = 0;
    if (n > 2047) n = 2047;
    buf[n] = 0;
    return std::wstring(buf, n);
}

static std::wstring GetEditSelectionOrLine(HWND hEdit) {
    DWORD a = 0, b = 0;
    SendMessageW(hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return L"";
    std::wstring all((size_t)len, L'\0');
    GetWindowTextW(hEdit, &all[0], len + 1);
    if (a != b && (int)b <= len) return all.substr(a, b - a);
    return GetEditCurrentLine(hEdit);
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
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
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
    if (!st->photo && st->sel < st->rawBlocks.size()) {
        st->photo = LoadPhotoFromRaw(st->rawBlocks[st->sel]);
    }
    // Optional HTTP(S) photo (ini LoadPhotoUrl=1). Default off — no network by default.
    if (!st->photo && st->loadPhotoUrl) {
        if (photoUrl.empty() && st->sel < st->rawBlocks.size()) {
            // try extract first PHOTO:http from raw (BuildFromRawBlock skips embedded only)
            auto lines = UnfoldVCard_Folded(SplitLines(st->rawBlocks[st->sel]));
            for (auto& L : lines) {
                size_t cpos = L.find(L':');
                if (cpos == std::wstring::npos) continue;
                std::wstring head = ToUpperASCII(Trim(L.substr(0, cpos)));
                size_t d = head.find(L'.');
                if (d != std::wstring::npos) head = head.substr(d + 1);
                if (head.rfind(L"PHOTO", 0) != 0) continue;
                std::wstring v = Trim(L.substr(cpos + 1));
                if (v.rfind(L"http://", 0) == 0 || v.rfind(L"https://", 0) == 0) {
                    photoUrl = v;
                    break;
                }
            }
        }
        if (!photoUrl.empty()) {
            auto bytes = HttpGetBytes(photoUrl);
            if (!bytes.empty()) st->photo = BitmapFromMemory(bytes);
        }
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
            add(g_tcRu ? L"Имя: " : L"Name: ", name);
            add(g_tcRu ? L"Тип контакта: " : L"Kind: ", c.kind);
            add(g_tcRu ? L"Пол: " : L"Gender: ", c.gender);
            add(g_tcRu ? L"Язык: " : L"Language: ", c.lang);
            for (auto& lg : c.langs) if (lg != c.lang) add(g_tcRu ? L"Язык: " : L"Language: ", lg);
            add(g_tcRu ? L"Компания: " : L"Organization: ", c.org);
            add(g_tcRu ? L"Должность: " : L"Role: ", c.title);
            if (!c.urls.empty()) { for (auto& u : c.urls) add(L"URL: ", u); }
            else add(L"URL: ", c.url);
            add(g_tcRu ? L"День рождения: " : L"Birthday: ", c.bday);
            for (auto& m : c.members) add(g_tcRu ? L"Участник: " : L"Member: ", m);
            for (auto& p : c.phones) if (!p.number.empty()) add(g_tcRu ? L"Телефон: " : L"Phone: ", p.number);
            bool any = false; for (auto& e : c.emails) { if (!e.addr.empty()) { add(L"Email: ", e.addr); any = true; } }
            if (!any) { std::wstring fb = FallbackEmail_NotesAware(c); if (!fb.empty()) add(L"Email: ", fb); }
            for (auto& a : c.addrs) if (!a.text.empty()) add(g_tcRu ? L"Адрес: " : L"Address: ", a.text);
            if constexpr (detail_detect::has_notes<Contact>::value) { for (auto& n : c.notes) add(g_tcRu ? L"Заметка: " : L"Note: ", n); }
            else if (!c.note.empty()) { add(g_tcRu ? L"Заметка: " : L"Note: ", c.note); }
        }
        else {
            text = L"";
        }
        // Preserve focus: EM_SETSEL/EM_SCROLLCARET can steal it from filter or list
        HWND keepFocus = GetFocus();
        SendMessageW(st->hEdit, WM_SETTEXT, 0, (LPARAM)text.c_str());
        SendMessageW(st->hEdit, EM_SETSEL, 0, 0);
        SendMessageW(st->hEdit, EM_SCROLLCARET, 0, 0);
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
    auto* st = viewer ? (ViewState*)GetWindowLongPtrW(viewer, GWLP_USERDATA) : nullptr;

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

// Сабкласс EDIT — Esc → Lister, Ctrl+C, focus redirect
static WNDPROC g_EditOldProc = nullptr;
static LRESULT CALLBACK EditSubclassProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ForwardEscToLister(hEdit);
            return 0;
        }
        // Ctrl+C without selection → copy current line value (after ':')
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
        // Forward wheel to parent so single card scrollbar moves (no inner V-scroll)
        HWND viewer = GetParent(hEdit);
        if (viewer) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            // lParam is screen coords for WM_MOUSEWHEEL
            return SendMessageW(viewer, WM_MOUSEWHEEL, wParam, lParam);
        }
        break;
    }
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

        // EDIT above photo: wrap text, no own scrollbars (outer hRightScroll scrolls card)
        st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE |
            ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
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

        // --- Right column: TEXT first, PHOTO below (#11); one outer vertical scroll (#7/#13) ---
        int pad = S(h, 12);
        int ex = listW + 1 + pad;
        int rightScrollBarW = sbw;
        int rightAreaTop = 0;
        int rightAreaH = std::max<int>(50, (int)rc.bottom);

        int ew = (int)rc.right - ex - pad - rightScrollBarW;
        if (ew < S(h, 80))
            ew = std::max<int>(0, (int)rc.right - ex - rightScrollBarW);

        // Cap photo size so it doesn't dominate the card (unified after text)
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
        // Assign width first so multiline wrap / EM_GETLINECOUNT are correct
        if (st->hEdit) {
            // temporary height; real height after measure
            MoveWindow(st->hEdit, ex, pad, ew, S(h, 200), FALSE);
        }
        int eh = MeasureEditContentHeight(st->hEdit, ew);
        if (eh < S(h, 80)) eh = S(h, 80);

        int editY = pad;
        int photoY = editY + eh + (photoH > 0 ? sep : 0);

        int contentH = pad + eh + (photoH > 0 ? sep + photoH : 0) + pad;
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
        // Text on top, photo under it — both moved by outer rightScroll
        MoveWindow(st->hEdit, ex, editY - rightScroll, ew, eh, TRUE);
        if (st->hPhoto) {
            if (photoH > 0) {
                ShowWindow(st->hPhoto, SW_SHOW);
                MoveWindow(st->hPhoto, ex, photoY - rightScroll, photoW, photoH, TRUE);
            } else {
                MoveWindow(st->hPhoto, ex, photoY - rightScroll, 0, 0, TRUE);
                ShowWindow(st->hPhoto, SW_HIDE);
            }
        }

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
                    st->sel = idx; st->rightScroll = 0;
                    UpdateRightPanel(st);
                    SetFocus(h); // again: UpdateRightPanel must not leave focus on hEdit
                    InvalidateRect(h, nullptr, FALSE);
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
        // Tooltip for long list names
        if (st->hTip) {
            RECT rcClient{}; GetClientRect(h, &rcClient);
            int statusH = S(h, 18);
            int listW = ListPaneWidth(h);
            int pad = S(h, 8);
            int tipRow = -1;
            std::wstring tipText;
            if (x < listW && y < rcClient.bottom - statusH) {
                int rowH = st->listItemH ? st->listItemH : S(h, 60);
                int row = (y - pad) / rowH;
                if (row >= 0 && row < st->perPage) {
                    size_t idx = (size_t)(st->listScroll + row);
                    if (idx < st->contacts.size()) {
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

    // ПКМ → копирование (список / EDIT)
    case WM_CONTEXTMENU: {
        if (!st) break;
        HWND hSrc = (HWND)w;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (pt.x == -1 && pt.y == -1) { // keyboard
            GetCursorPos(&pt);
        }
        POINT ptClient = pt; ScreenToClient(h, &ptClient);

        // Right-click on list: copy name / phone / email
        int listW = ListPaneWidth(h);
        if (hSrc == h && ptClient.x < listW && st->sel < st->contacts.size()) {
            const Contact& c = st->contacts[st->sel];
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 10, g_tcRu ? L"Копировать имя" : L"Copy name");
            AppendMenuW(m, MF_STRING, 11, g_tcRu ? L"Копировать телефон" : L"Copy phone");
            AppendMenuW(m, MF_STRING, 12, g_tcRu ? L"Копировать email" : L"Copy email");
            AppendMenuW(m, MF_STRING, 13, g_tcRu ? L"Копировать карточку" : L"Copy card text");
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, h, nullptr);
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
            return 0;
        }

        if (hSrc == st->hEdit || (hSrc == h)) {
            if (hSrc == h) {
                RECT rcE{}; GetWindowRect(st->hEdit, &rcE);
                if (!(pt.x >= rcE.left && pt.x < rcE.right && pt.y >= rcE.top && pt.y < rcE.bottom)) break;
            }
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, g_tcRu ? L"Копировать" : L"Copy");
            AppendMenuW(m, MF_STRING, 2, g_tcRu ? L"Копировать строку (значение)" : L"Copy line value");
            AppendMenuW(m, MF_STRING, 3, g_tcRu ? L"Копировать всё" : L"Copy all");
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            if (cmd == 1) {
                DWORD a = 0, b = 0; SendMessageW(st->hEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
                if (a != b) SendMessageW(st->hEdit, WM_COPY, 0, 0);
                else {
                    std::wstring line = GetEditCurrentLine(st->hEdit);
                    SetClipboardTextW(h, ValueAfterColon(line));
                }
            } else if (cmd == 2) {
                std::wstring line = GetEditSelectionOrLine(st->hEdit);
                SetClipboardTextW(h, ValueAfterColon(line));
            } else if (cmd == 3) {
                int len = GetWindowTextLengthW(st->hEdit);
                std::wstring all((size_t)std::max(0, len), L'\0');
                if (len > 0) GetWindowTextW(st->hEdit, &all[0], len + 1);
                SetClipboardTextW(h, all);
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
    if (idx < st->contacts.size()) { st->sel = idx; st->rightScroll = 0; EnsureSelVisible(h, st); UpdateRightPanel(st); InvalidateRect(h, nullptr, FALSE); 
        RECT rc; GetClientRect(h, &rc); SendMessage(h, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom)); }
}

// Поиск — по разобранным полям; подсветка совпадений + счётчик в статусбаре
bool VCFView_SearchEx(HWND h, const std::wstring& needle, size_t startIndex, bool backwards, bool /*matchCase*/, bool wholeWord, bool wrap) {
    auto* st = (ViewState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!st || st->contacts.empty() || needle.empty()) return false;

    st->searchNeedle = needle;
    RebuildSearchFlags(st, wholeWord);

    auto norm = [&](const std::wstring& x) { return LowerInvariant(x); };
    std::wstring n = norm(needle);

    const size_t count = st->contacts.size();
    auto nextIndex = [&](size_t i)->size_t { return backwards ? (i == 0 ? count - 1 : i - 1) : (i + 1 == count ? 0 : i + 1); };

    size_t i = startIndex % count, first = i;
    do {
        if (i < st->matchFlags.size() && st->matchFlags[i]) {
            // verify whole-word against same helper
            if (ContactMatchesNeedle(st->contacts[i], n, wholeWord)) {
                st->sel = i;
                st->rightScroll = 0;
                // update matchPos among matches
                int p = 0;
                for (size_t k = 0; k <= i; ++k) if (k < st->matchFlags.size() && st->matchFlags[k]) ++p;
                st->matchPos = p;
                EnsureSelVisible(h, st);
                UpdateRightPanel(st);
                // highlight needle in EDIT text if present
                if (IsWindow(st->hEdit)) {
                    int len = GetWindowTextLengthW(st->hEdit);
                    if (len > 0) {
                        std::wstring all((size_t)len, L'\0');
                        GetWindowTextW(st->hEdit, &all[0], len + 1);
                        std::wstring low = LowerInvariant(all);
                        size_t pos = low.find(n);
                        if (pos != std::wstring::npos)
                            SendMessageW(st->hEdit, EM_SETSEL, (WPARAM)pos, (LPARAM)(pos + n.size()));
                    }
                }
                InvalidateRect(h, nullptr, FALSE);
                return true;
            }
        }
        i = nextIndex(i);
    } while (wrap && i != first);

    InvalidateRect(h, nullptr, FALSE); // still show match highlights even if wrap failed mid-way
    return false;
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