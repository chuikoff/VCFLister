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

#include "vcf_parser.hpp"
#include "vcf_utils.hpp"

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
static std::wstring unquote(const std::wstring& s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Prefer shared UnescapeVCard; keep name for local call sites
static std::wstring unescape(const std::wstring& s) {
    return UnescapeVCard(s);
}

static bool isHiddenProp(const std::wstring& prop) {
    // Not shown as card text (photo rendered separately; labels applied to groups)
    return prop == L"BEGIN" || prop == L"END" || prop == L"VERSION"
        || prop == L"PHOTO" || prop == L"X-ABLABEL"
        || prop == L"UID" || prop == L"PRODID" || prop == L"CLIENTPIDMAP"
        || prop == L"X-ADDRESSING-GRAMMAR" || prop == L"X-ABUID" || prop == L"X-ABSHOWAS"
        || prop == L"X-IMAGETYPE" || prop == L"X-IMAGEHASH"
        || prop == L"X-SHARED-PHOTO-DISPLAY-PREF";
}

static void pushField(Contact& c, const std::wstring& prop, const std::wstring& head,
    const std::wstring& value, bool isNote = false) {
    CardField f;
    f.prop = prop;
    f.head = head;
    f.value = value;
    f.isNote = isNote;
    f.show = !isHiddenProp(prop) && !(value.empty() && !isNote);
    // Always keep X-ABLABEL for label resolution even if hidden
    if (prop == L"X-ABLABEL") {
        f.show = false;
        f.value = value;
        c.fields.push_back(std::move(f));
        return;
    }
    if (!f.show && prop != L"PHOTO") return;
    if (prop == L"PHOTO") {
        f.show = false; // photo panel only
        f.value.clear();
    }
    c.fields.push_back(std::move(f));
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

// Text decode — shared VcfDecodeTextValue (QP + charset + unescape)
static std::wstring decodeTextValue(const std::wstring& wval, bool isQP, const std::wstring& charset) {
    return VcfDecodeTextValue(wval, isQP, charset);
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
            std::wstring tv = unquote(P.substr(5));
            auto list = split(tv, L',');
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

static void addNote(Contact& c, const std::wstring& txt) {
    if (txt.empty()) return;
    c.notes.push_back(txt);
    if (c.note.empty()) c.note = txt; // primary note for list/compat
}

static void addAndroidCustom(Contact& c, const std::wstring& rawType, const std::vector<std::wstring>& slots) {
    Contact::AndroidCustom ac;
    ac.rawType = rawType;
    ac.slots = slots;
    c.androidCustoms.push_back(std::move(ac));
}

// ---------- main parser ----------
std::vector<Contact> ParseVCard(const std::wstring& text)
{
    std::vector<Contact> contacts;

    // One shared unfold path (same as UI utilities)
    auto lines = UnfoldVCard_Folded(SplitLines(text));

    Contact cur;
    bool inCard = false;
    bool hasProp = false;

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        auto raw = trim(lines[idx]);
        if (raw.empty()) continue;

        auto up = upper(raw);

        if (up == L"BEGIN:VCARD") { inCard = true; cur = Contact(); hasProp = false; continue; }
        if (up == L"END:VCARD") { 
            if (inCard) { 
                // Show empty cards too (as requested). Raw blocks and contacts stay in sync.
                contacts.push_back(cur); 
                cur = Contact(); 
                inCard = false; 
                hasProp = false;
            } 
            continue; 
        }
        if (!inCard) continue;

        size_t colon = raw.find(L':');
        if (colon == std::wstring::npos) continue;
        hasProp = true;

        std::wstring left = raw.substr(0, colon);
        std::wstring value = raw.substr(colon + 1);

        auto parts = split(left, L';');
        if (parts.empty()) continue;

        std::wstring propName = basePropName(upper(parts[0]));
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
                auto v = unquote(P.substr(9));
                if (v == L"QUOTED-PRINTABLE" || v == L"QP") encQP = true;
                if (v == L"BASE64" || v == L"B") photoIsBase64 = true;
            }
            else if (P.rfind(L"CHARSET=", 0) == 0) {
                charset = unquote(p.substr(8));
            }
            else if (P.rfind(L"VALUE=", 0) == 0) {
                auto v = unquote(P.substr(6));
                if (v == L"URL") photoIsURL = true;
            }
        }

        // Для vCard 2.1 + QP: soft-break '=' в конце строки + продолжения без нового свойства
        if (encQP) {
            while (idx + 1 < lines.size()) {
                bool soft = !value.empty() && value.back() == L'=';
                const std::wstring& nextRaw = lines[idx + 1];
                std::wstring nextTrim = trim(nextRaw);
                if (nextTrim.empty()) { ++idx; continue; }

                // Следующая строка похожа на новое свойство? (FN:, TEL;TYPE=...:, N;CHARSET=...:)
                bool looksLikeNewProp = false;
                if (nextTrim[0] != L'=') {
                    size_t cp = nextTrim.find(L':');
                    if (cp != std::wstring::npos && cp > 0) {
                        std::wstring head = nextTrim.substr(0, cp);
                        // head: letters/digits/;/=/-/.  no spaces
                        looksLikeNewProp = head.find_first_of(L" \t") == std::wstring::npos &&
                            head.find_first_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != std::wstring::npos;
                    }
                }

                if (soft) {
                    value.pop_back(); // drop soft-break '='
                    if (looksLikeNewProp) break; // orphan soft-break before next field
                    value += nextTrim;
                    ++idx;
                    continue;
                }
                // без soft-break: только явные продолжения QP (часто начинаются с =HH) или строки без ':'
                if (looksLikeNewProp) break;
                if (nextTrim[0] == L'=' || nextRaw.find(L':') == std::wstring::npos) {
                    value += nextTrim;
                    ++idx;
                    continue;
                }
                break;
            }
            // leftover trailing soft-break (no continuation)
            while (!value.empty() && value.back() == L'=') value.pop_back();
        }

        // ----- structured Contact + CardField (single pass) -----
        if (propName == L"N") {
            auto vs = split(value, L';');
            if (vs.size() >= 1) cur.n_family = unescape(decodeTextValue(vs[0], encQP, charset));
            if (vs.size() >= 2) cur.n_given = unescape(decodeTextValue(vs[1], encQP, charset));
            // display: cleaned name parts
            std::wstring disp;
            for (auto& part : vs) {
                auto t = trim(unescape(decodeTextValue(part, encQP, charset)));
                if (t.empty()) continue;
                if (!disp.empty()) disp += L" ";
                disp += t;
            }
            pushField(cur, propName, left, disp);
        }
        else if (propName == L"FN") {
            cur.fn = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, cur.fn);
        }
        else if (propName == L"ORG") {
            cur.org = unescape(decodeTextValue(value, encQP, charset));
            // clean ;;;
            auto vs = split(cur.org, L';');
            std::wstring cleaned;
            for (auto& part : vs) {
                auto t = trim(part);
                if (t.empty()) continue;
                if (!cleaned.empty()) cleaned += L", ";
                cleaned += t;
            }
            if (!cleaned.empty()) cur.org = cleaned;
            pushField(cur, propName, left, cur.org);
        }
        else if (propName == L"TITLE") {
            cur.title = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, cur.title);
        }
        else if (propName == L"URL") {
            std::wstring u = unescape(decodeTextValue(value, encQP, charset));
            if (!u.empty()) {
                if (cur.url.empty()) cur.url = u;
                cur.urls.push_back(u);
                pushField(cur, propName, left, u);
            }
        }
        else if (propName == L"BDAY") {
            cur.bday = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, cur.bday);
        }
        else if (propName == L"GENDER") {
            cur.gender = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, cur.gender);
        }
        else if (propName == L"LANG") {
            std::wstring lg = unescape(decodeTextValue(value, encQP, charset));
            if (!lg.empty()) {
                if (cur.lang.empty()) cur.lang = lg;
                cur.langs.push_back(lg);
                pushField(cur, propName, left, lg);
            }
        }
        else if (propName == L"KIND") {
            cur.kind = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, cur.kind);
        }
        else if (propName == L"MEMBER") {
            std::wstring m = unescape(decodeTextValue(value, encQP, charset));
            if (!m.empty()) {
                cur.members.push_back(m);
                pushField(cur, propName, left, m);
            }
        }
        else if (propName == L"X-GENDER") {
            if (cur.gender.empty())
                cur.gender = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, unescape(decodeTextValue(value, encQP, charset)));
        }
        else if (propName == L"NOTE") {
            std::wstring nt = unescape(decodeTextValue(value, encQP, charset));
            addNote(cur, nt);
            pushField(cur, propName, left, nt, true);
        }
        else if (propName == L"TEL") {
            Phone p;
            p.number = unescape(decodeTextValue(value, encQP, charset));
            p.types = parseTypes(params);
            if (!p.number.empty()) {
                cur.phones.push_back(p);
                pushField(cur, propName, left, p.number);
            }
        }
        else if (propName == L"EMAIL") {
            Email e;
            e.addr = unescape(decodeTextValue(value, encQP, charset));
            e.types = parseTypes(params);
            if (!e.addr.empty()) {
                cur.emails.push_back(e);
                pushField(cur, propName, left, e.addr);
            }
        }
        else if (propName == L"ADR") {
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
                pushField(cur, propName, left, joined);
            }
        }
        else if (propName == L"PHOTO") {
            if (photoIsURL) {
                cur.photo_url = unescape(decodeTextValue(value, encQP, charset));
            }
            else if (photoIsBase64) {
                size_t j = idx;
                std::wstring fullValue = value;
                while (j + 1 < lines.size()) {
                    const std::wstring& nxt = lines[j + 1];
                    if (!nxt.empty() && (nxt[0] == L' ' || nxt[0] == L'\t')) {
                        fullValue.append(nxt.c_str() + 1);
                        ++j;
                    }
                    else break;
                }
                idx = j;
                setEmbeddedPhoto(cur, VcfBase64Decode(fullValue));
            }
            // 3) BASE64 without ENCODING= (or bare ;JPEG)
            else {
                size_t j = idx;
                std::wstring fullValue = value;
                while (j + 1 < lines.size()) {
                    const std::wstring& nxt = lines[j + 1];
                    if (!nxt.empty() && (nxt[0] == L' ' || nxt[0] == L'\t')) {
                        fullValue.append(nxt.c_str() + 1);
                        ++j;
                    }
                    else break;
                }
                if (j > idx) idx = j;

                if (fullValue.rfind(L"http://", 0) == 0 || fullValue.rfind(L"https://", 0) == 0) {
                    cur.photo_url = fullValue;
                } else {
                    auto bytes = VcfBase64Decode(fullValue);
                    if (!bytes.empty())
                        setEmbeddedPhoto(cur, std::move(bytes));
                }
            }
            pushField(cur, propName, left, L""); // hide binary/url from text
        }
        else if (propName == L"X-ANDROID-CUSTOM") {
            std::wstring raw_val = unescape(decodeTextValue(value, encQP, charset));
            std::wstring rawType;
            std::vector<std::wstring> slots;
            size_t p = raw_val.find(L':');
            if (p != std::wstring::npos) {
                rawType = raw_val.substr(0, p);
                slots = splitSemicolonEscaped(raw_val.substr(p + 1));
            }
            else {
                slots = splitSemicolonEscaped(raw_val);
            }
            addAndroidCustom(cur, rawType, slots);
            pushField(cur, propName, left, raw_val);
        }
        else {
            // Unknown / other properties — keep for card display
            std::wstring txt = unescape(decodeTextValue(value, encQP, charset));
            pushField(cur, propName, left, txt);
        }
    }

    if (inCard) contacts.push_back(cur);
    return contacts;
}
