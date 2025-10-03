// vcf_parser.cpp — разбор vCard 3.0 (поддержка itemN.EMAIL/TEL, EMAIL выводится; без Address::types)
#define UNICODE
#define _UNICODE
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

// ---- utils -------------------------------------------------
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
static std::wstring unescape(const std::wstring& s) {
    std::wstring r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t c = s[i + 1];
            if (c == L'n' || c == L'N') { r.push_back(L'\n'); ++i; continue; }
            if (c == L',' || c == L';' || c == L'\\') { r.push_back(c); ++i; continue; }
        }
        r.push_back(s[i]);
    }
    return r;
}

// разворачиваем перенесённые строки (следующая начинается с пробела/таба)
static std::vector<std::wstring> unfoldLines(const std::wstring& text) {
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

// отрезаем itemN. у имени свойства (item1.EMAIL -> EMAIL)
static std::wstring basePropName(const std::wstring& name) {
    size_t dot = name.find(L'.');
    if (dot == std::wstring::npos) return name;
    std::wstring left = upper(name.substr(0, dot));
    if (left.rfind(L"ITEM", 0) == 0) return name.substr(dot + 1);
    size_t last = name.rfind(L'.');
    return (last == std::wstring::npos) ? name : name.substr(last + 1);
}

// TYPE=HOME,WORK и т.п. (для TEL/EMAIL)
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

// ---- parser ------------------------------------------------
std::vector<Contact> ParseVCard(const std::wstring& text)
{
    std::vector<Contact> contacts;

    auto lines = unfoldLines(text);
    Contact cur;
    bool inCard = false;

    for (auto& raw : lines) {
        auto line = trim(raw);
        if (line.empty()) continue;

        auto up = upper(line);

        if (up == L"BEGIN:VCARD") { inCard = true; cur = Contact(); continue; }
        if (up == L"END:VCARD") { if (inCard) { contacts.push_back(cur); cur = Contact(); inCard = false; } continue; }
        if (!inCard) continue;

        size_t colon = line.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring left = line.substr(0, colon);
        std::wstring value = unescape(line.substr(colon + 1));

        auto parts = split(left, L';');
        if (parts.empty()) continue;

        std::wstring name = basePropName(upper(parts[0]));
        std::vector<std::wstring> params;
        for (size_t i = 1; i < parts.size(); ++i) params.push_back(parts[i]);

        if (name == L"N") {
            auto v = split(value, L';');
            if (v.size() >= 1) cur.n_family = v[0];
            if (v.size() >= 2) cur.n_given = v[1];
        }
        else if (name == L"FN") {
            cur.fn = value;
        }
        else if (name == L"ORG") {
            cur.org = value;
        }
        else if (name == L"TITLE") {
            cur.title = value;
        }
        else if (name == L"URL") {
            cur.url = value;
        }
        else if (name == L"BDAY") {
            cur.bday = value;
        }
        else if (name == L"NOTE") {
            cur.note = value;
        }
        else if (name == L"TEL") {
            Phone p;
            p.number = value;
            p.types = parseTypes(params);
            if (!p.number.empty()) cur.phones.push_back(std::move(p));
        }
        else if (name == L"EMAIL") {
            Email e;
            e.addr = value;
            e.types = parseTypes(params);
            if (!e.addr.empty()) cur.emails.push_back(std::move(e));
        }
        else if (name == L"ADR") {
            // ADR: PO;EXT;STREET;LOCALITY;REGION;POSTCODE;COUNTRY -> соберём одной строкой
            auto v = split(value, L';');
            std::wstring joined;
            for (size_t i = 0; i < v.size(); ++i) {
                auto t = trim(v[i]); if (t.empty()) continue;
                if (!joined.empty()) joined += L", ";
                joined += t;
            }
            if (!joined.empty()) {
                Address a; a.text = joined;            // <-- только text, без types
                cur.addrs.push_back(std::move(a));
            }
        }
        // PHOTO/X-ABLABEL/прочее — оставим как есть/игнорируем
    }

    if (inCard) contacts.push_back(cur);
    return contacts;
}
