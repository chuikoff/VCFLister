// vcf_parser.cpp — vCard 2.1/3.0 parser with QP + BASE64 photo
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <type_traits> // <-- добавлено для SFINAE-хелперов

#include "vcf_parser.hpp"

// ---------- helpers ----------
static inline std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
static inline std::wstring upper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towupper);
    return s;
}
static std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t p = s.find(sep, i);
        if (p == std::wstring::npos) { out.push_back(s.substr(i)); break; }
        out.push_back(s.substr(i, p - i));
        i = p + 1;
    }
    return out;
}
// расширено: обрабатываем также экранированный двоеточие '\:'
static std::wstring unescape(const std::wstring& s) {
    std::wstring r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t c = s[i + 1];
            if (c == L'n' || c == L'N') { r.push_back(L'\n'); ++i; continue; }
            if (c == L',' || c == L';' || c == L'\\' || c == L':') { r.push_back(c); ++i; continue; }
        }
        r.push_back(s[i]);
    }
    return r;
}

// split по ';' с учётом экранирования "\;"
static std::vector<std::wstring> splitSemicolonEscaped(const std::wstring& s) {
    std::vector<std::wstring> out; out.reserve(8);
    std::wstring cur; cur.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) { cur.push_back(s[++i]); continue; }
        if (c == L';') { out.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// --- Base64 decode (ASCII, игнорирует пробелы/переводы строк) ---
static std::vector<uint8_t> Base64Decode(const std::string& s) {
    auto val = [](unsigned char c)->int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
        };
    std::vector<uint8_t> out;
    int v = 0, vb = -8;
    for (unsigned char c : s) {
        if (c <= ' ') continue; // пропускаем пробелы/CRLF/TAB
        if (c == '=') break;
        int d = val(c);
        if (d < 0) continue;
        v = (v << 6) | d;
        vb += 6;
        if (vb >= 0) {
            out.push_back((uint8_t)((v >> vb) & 0xFF));
            vb -= 8;
        }
    }
    return out;
}

// Разворачивание «сложенных» строк: строки, начинающиеся с пробела/таба, — продолжение предыдущей
static std::vector<std::wstring> unfoldLines_fold_prefix(const std::wstring& text) {
    std::vector<std::wstring> raw;
    std::wstring cur;
    size_t i = 0, n = text.size();
    while (i < n) {
        size_t j = i;
        while (j < n && text[j] != L'\r' && text[j] != L'\n') ++j;
        std::wstring line = text.substr(i, j - i);

        if (j < n && text[j] == L'\r') ++j;
        if (j < n && text[j] == L'\n') ++j;

        if (!line.empty() && (line[0] == L' ' || line[0] == L'\t')) {
            if (!cur.empty()) cur += line.substr(1);
            else cur = line.substr(1);
        }
        else {
            if (!cur.empty()) raw.push_back(cur);
            cur = line;
        }
        i = j;
    }
    if (!cur.empty()) raw.push_back(cur);
    return raw;
}

// map CHARSET -> Windows CP
static UINT codepageFromCharset(std::wstring cs) {
    cs = upper(cs);
    if (cs == L"UTF-8" || cs == L"UTF8") return CP_UTF8;
    if (cs == L"UTF-16" || cs == L"UTF16" || cs == L"UTF-16LE") return 1200;
    if (cs == L"WINDOWS-1251" || cs == L"CP1251" || cs == L"WIN-1251") return 1251;
    if (cs == L"KOI8-R" || cs == L"KOI8R") return 20866;
    if (cs == L"ISO-8859-5" || cs == L"ISO8859-5") return 28595;
    if (cs == L"ISO-8859-1" || cs == L"ISO8859-1" || cs == L"LATIN1") return 28591;
    return CP_UTF8;
}

static std::string w2ascii(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t ch : w) s.push_back((char)((ch < 128) ? ch : '?'));
    return s;
}

// Quoted-Printable decode to bytes; handles soft breaks "=\r\n" / "=\n"
static std::vector<unsigned char> decodeQP(const std::string& in) {
    std::vector<unsigned char> out; out.reserve(in.size());
    const size_t n = in.size();
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') {
            if (i + 1 < n && in[i + 1] == '\r' && i + 2 < n && in[i + 2] == '\n') { i += 3; continue; }
            if (i + 1 < n && in[i + 1] == '\n') { i += 2; continue; }
            auto hex = [&](char x)->int {
                if (x >= '0' && x <= '9') return x - '0';
                if (x >= 'A' && x <= 'F') return 10 + (x - 'A');
                if (x >= 'a' && x <= 'f') return 10 + (x - 'a');
                return -1;
                };
            if (i + 2 < n) {
                int h1 = hex(in[i + 1]), h2 = hex(in[i + 2]);
                if (h1 >= 0 && h2 >= 0) { out.push_back((unsigned char)((h1 << 4) | h2)); i += 3; continue; }
            }
            out.push_back('=');
            ++i;
        }
        else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

static std::wstring mbToWide(const unsigned char* data, int len, UINT cp) {
    DWORD flags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
    int wlen = MultiByteToWideChar(cp, flags, (LPCCH)data, len, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(cp, flags, (LPCCH)data, len, &w[0], wlen);
    return w;
}

static std::wstring decodeTextValue(const std::wstring& wval, bool isQP, const std::wstring& charset) {
    if (!isQP) return wval;
    std::string ascii = w2ascii(wval);
    auto bytes = decodeQP(ascii);
    return mbToWide(bytes.data(), (int)bytes.size(), codepageFromCharset(charset));
}

// item1.EMAIL -> EMAIL
static std::wstring basePropName(const std::wstring& name) {
    size_t dot = name.find(L'.');
    if (dot == std::wstring::npos) return name;
    std::wstring left = upper(name.substr(0, dot));
    if (left.rfind(L"ITEM", 0) == 0) return name.substr(dot + 1);
    size_t last = name.rfind(L'.');
    return (last == std::wstring::npos) ? name : name.substr(last + 1);
}

static std::vector<std::wstring> parseTypes(const std::vector<std::wstring>& params) {
    std::vector<std::wstring> out;
    for (auto& p : params) {
        auto P = upper(p);
        if (P.rfind(L"TYPE=", 0) == 0) {
            auto list = split(P.substr(5), L',');
            for (auto& t : list) {
                auto tt = trim(t);
                if (!tt.empty()) out.push_back(tt);
            }
        }
        else if (P == L"HOME" || P == L"WORK" || P == L"CELL" || P == L"VOICE" || P == L"FAX" || P == L"PREF") {
            out.push_back(P);
        }
    }
    return out;
}

// helper: положить embedded-фото в Contact::photo
static void setEmbeddedPhoto(Contact& c, std::vector<uint8_t> bytes)
{
    if (bytes.empty()) return;
    Photo ph;
    ph.bytes = std::move(bytes);
    c.photo = std::move(ph);
}

/* ===========================
   SFINAE-хелперы для новых полей
   =========================== */

   // addNote: если у Contact есть vector<wstring> notes — пушим туда; иначе аккуратно накапливаем в contact.note
template<typename T>
static auto addNoteImpl(T& c, const std::wstring& txt, int)
-> decltype(c.notes.push_back(txt), void())
{
    c.notes.push_back(txt);
    if (c.note.empty()) c.note = txt; // совместимость
}
static void addNoteImpl(...) { /* no-op fallback */ }

static void addNote(Contact& c, const std::wstring& txt) {
    if (txt.empty()) return;
    // попытка положить в notes (если есть)
    addNoteImpl(c, txt, 0);
    // если поля notes нет — склеиваем в note (с пустой строкой между)
    if (c.notes.size() == 0) {
        if (c.note.empty()) c.note = txt;
        else                c.note += L"\n\n" + txt;
    }
}

// addAndroidCustom: если у Contact есть vector<AndroidCustom> androidCustoms — записываем; иначе no-op
template<typename T>
static auto addAndroidImpl(T& c, const std::wstring& rawType, const std::vector<std::wstring>& slots, int)
-> decltype(c.androidCustoms.push_back(typename T::AndroidCustom{}), void())
{
    typename T::AndroidCustom ac;
    ac.rawType = rawType;
    ac.slots = slots;
    c.androidCustoms.push_back(std::move(ac));
}
static void addAndroidImpl(...) { /* no-op */ }

static void addAndroidCustom(Contact& c, const std::wstring& rawType, const std::vector<std::wstring>& slots) {
    addAndroidImpl(c, rawType, slots, 0);
}

// ---------- main parser ----------
std::vector<Contact> ParseVCard(const std::wstring& text)
{
    std::vector<Contact> contacts;

    // 1) Разворачиваем «сложенные» строки: продолжения начинаются с пробела/таба
    auto lines = unfoldLines_fold_prefix(text);

    Contact cur;
    bool inCard = false;

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        auto raw = trim(lines[idx]);
        if (raw.empty()) continue;

        auto up = upper(raw);

        if (up == L"BEGIN:VCARD") { inCard = true; cur = Contact(); continue; }
        if (up == L"END:VCARD") { if (inCard) { contacts.push_back(cur); cur = Contact(); inCard = false; } continue; }
        if (!inCard) continue;

        size_t colon = raw.find(L':');
        if (colon == std::wstring::npos) continue;

        std::wstring left = raw.substr(0, colon);
        std::wstring value = raw.substr(colon + 1);

        auto parts = split(left, L';');
        if (parts.empty()) continue;

        std::wstring name = basePropName(upper(parts[0]));
        std::vector<std::wstring> params;
        for (size_t i = 1; i < parts.size(); ++i) params.push_back(parts[i]);

        bool encQP = false;
        std::wstring charset;

        // флаги для PHOTO
        bool photoIsBase64 = false;
        bool photoIsURL = false;

        for (auto& p : params) {
            auto P = upper(p);
            if (P.rfind(L"ENCODING=", 0) == 0) {
                auto v = P.substr(9);
                if (v == L"QUOTED-PRINTABLE" || v == L"QP") encQP = true;
                if (v == L"BASE64" || v == L"B") photoIsBase64 = true;
            }
            else if (P.rfind(L"CHARSET=", 0) == 0) {
                charset = p.substr(8);
            }
            else if (P.rfind(L"VALUE=", 0) == 0) {
                auto v = P.substr(6);
                if (v == L"URL") photoIsURL = true;
            }
        }

        // Для vCard 2.1 + QP: склеиваем последующие строки БЕЗ двоеточия — это продолжение QP
        if (encQP) {
            while (idx + 1 < lines.size()) {
                const std::wstring& nextRaw = lines[idx + 1];
                if (nextRaw.find(L':') != std::wstring::npos) break;
                value += L"\n";
                value += nextRaw;
                ++idx;
            }
        }

        // ----- раскладываем по полям -----
        if (name == L"N") {
            auto vs = split(value, L';');
            if (vs.size() >= 1) cur.n_family = unescape(decodeTextValue(vs[0], encQP, charset));
            if (vs.size() >= 2) cur.n_given = unescape(decodeTextValue(vs[1], encQP, charset));
        }
        else if (name == L"FN") {
            cur.fn = unescape(decodeTextValue(value, encQP, charset));
        }
        else if (name == L"ORG") {
            cur.org = unescape(decodeTextValue(value, encQP, charset));
        }
        else if (name == L"TITLE") {
            cur.title = unescape(decodeTextValue(value, encQP, charset));
        }
        else if (name == L"URL") {
            cur.url = unescape(decodeTextValue(value, encQP, charset));
        }
        else if (name == L"BDAY") {
            cur.bday = unescape(decodeTextValue(value, encQP, charset));
        }
        else if (name == L"NOTE") {
            // РАНЬШЕ: перезатирали одно поле note (терялись много NOTE)  [исходник: см. блок NOTE] 
            // ТЕПЕРЬ: накапливаем все заметки (или конкатенируем, если в модели нет vector<notes>)
            addNote(cur, unescape(decodeTextValue(value, encQP, charset)));
        }
        else if (name == L"TEL") {
            Phone p;
            p.number = unescape(decodeTextValue(value, encQP, charset));
            p.types = parseTypes(params);
            if (!p.number.empty()) cur.phones.push_back(std::move(p));
        }
        else if (name == L"EMAIL") {
            Email e;
            e.addr = unescape(decodeTextValue(value, encQP, charset));
            e.types = parseTypes(params);
            if (!e.addr.empty()) cur.emails.push_back(std::move(e));
        }
        else if (name == L"ADR") {
            auto vs = split(unescape(decodeTextValue(value, encQP, charset)), L';');
            std::wstring joined;
            for (auto& part : vs) {
                auto t = trim(part); if (t.empty()) continue;
                if (!joined.empty()) joined += L", ";
                joined += t;
            }
            if (!joined.empty()) {
                Address a; a.text = joined;
                cur.addrs.push_back(std::move(a));
            }
        }
        else if (name == L"PHOTO") {
            // 1) PHOTO;VALUE=URL:...
            if (photoIsURL) {
                cur.photo_url = unescape(decodeTextValue(value, encQP, charset));
            }
            // 2) PHOTO;ENCODING=BASE64: (в v2.1 строки часто «сложены» с пробелом в начале)
            else if (photoIsBase64) {
                // склеим возможные продолжения (начинаются с пробела/таба)
                size_t j = idx;
                while (j + 1 < lines.size()) {
                    const std::wstring& nxt = lines[j + 1];
                    if (!nxt.empty() && (nxt[0] == L' ' || nxt[0] == L'\t')) {
                        value.append(nxt.c_str() + 1);
                        ++j;
                    }
                    else break;
                }
                idx = j;

                // wstring -> ASCII (base64 ASCII-only)
                std::string b64; b64.reserve(value.size());
                for (wchar_t wc : value) if (wc <= 0x7F) b64.push_back((char)wc);

                auto bytes = Base64Decode(b64);
                setEmbeddedPhoto(cur, std::move(bytes));
            }
        }
        else if (name == L"X-ANDROID-CUSTOM") {
            // Пример: X-ANDROID-CUSTOM:vnd.android.cursor.item/nickname;John;\;escaped\;;...
            std::wstring raw = unescape(decodeTextValue(value, encQP, charset));
            std::wstring rawType;
            std::vector<std::wstring> slots;
            size_t p = raw.find(L':');
            if (p != std::wstring::npos) {
                rawType = raw.substr(0, p);
                slots = splitSemicolonEscaped(raw.substr(p + 1));
            }
            else {
                slots = splitSemicolonEscaped(raw);
            }
            // положим (если есть место в модели; иначе тихий no-op)
            addAndroidCustom(cur, rawType, slots);
        }
    }

    if (inCard) contacts.push_back(cur);
    return contacts;
}
