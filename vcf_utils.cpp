#include "vcf_utils.hpp"
#include <windows.h>

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring ToUpperASCII(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') ? (wchar_t)(c - 32) : c;
        });
    return t;
}

std::vector<std::wstring> SplitLines(const std::wstring& block) {
    std::vector<std::wstring> lines; lines.reserve(64);
    size_t i = 0, n = block.size();
    while (i < n) {
        size_t j = block.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) { lines.push_back(block.substr(i)); break; }
        lines.push_back(block.substr(i, j - i));
        // CR+CRLF ("\r\r\n") from old iOS — one separator
        while (j < n && (block[j] == L'\r' || block[j] == L'\n')) ++j;
        i = j;
    }
    return lines;
}

std::vector<std::wstring> UnfoldVCard_Folded(const std::vector<std::wstring>& lines) {
    std::vector<std::wstring> out;
    out.reserve(lines.size());
    for (const auto& Lraw : lines) {
        std::wstring L = Lraw;
        while (!L.empty() && (L.back() == L'\r' || L.back() == L'\n')) L.pop_back();
        if (L.empty()) continue;
        if (!out.empty() && (L[0] == L' ' || L[0] == L'\t'))
            out.back() += L.substr(1);
        else
            out.push_back(std::move(L));
    }
    return out;
}

std::wstring LowerInvariant(const std::wstring& s) {
    if (s.empty()) return s;
    int need = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(), -1, nullptr, 0);
    if (need <= 0) {
        std::wstring t = s;
        std::transform(t.begin(), t.end(), t.begin(), ::towlower);
        return t;
    }
    std::wstring out; out.resize(need);
    LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(), -1, &out[0], need);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring unquote(const std::wstring& s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
        return s.substr(1, s.size() - 2);
    return s;
}

std::wstring UnescapeVCard(const std::wstring& s) {
    std::wstring r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t c = s[i + 1];
            if (c == L'n' || c == L'N') { r.push_back(L'\n'); ++i; continue; }
            if (c == L't' || c == L'T') { r.push_back(L'\t'); ++i; continue; }
            if (c == L',' || c == L';' || c == L'\\' || c == L':') { r.push_back(c); ++i; continue; }
        }
        r.push_back(s[i]);
    }
    return r;
}

std::vector<uint8_t> VcfBase64Decode(const std::wstring& wsrc) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string alph;
    alph.reserve(wsrc.size());
    for (wchar_t wc : wsrc) {
        if (wc == L'=') break;
        if (wc == L'\r' || wc == L'\n' || wc == L' ' || wc == L'\t') continue;
        if (wc > 127) continue;
        if (val((unsigned char)wc) < 0) continue;
        alph.push_back((char)wc);
    }
    while (alph.size() % 4 == 1 && !alph.empty()) alph.pop_back();

    std::vector<uint8_t> out;
    out.reserve(alph.size() * 3 / 4);
    int v = 0, vb = -8;
    for (unsigned char c : alph) {
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

std::vector<uint8_t> VcfQuotedPrintableDecode(const std::wstring& wsrc) {
    std::string in; in.reserve(wsrc.size());
    for (wchar_t wc : wsrc) in.push_back((char)((unsigned)wc & 0xFF));

    std::vector<uint8_t> out; out.reserve(in.size());
    const size_t n = in.size();
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') {
            if (i + 1 >= n) break;
            if (in[i + 1] == '\r' && i + 2 < n && in[i + 2] == '\n') { i += 3; continue; }
            if (in[i + 1] == '\n' || in[i + 1] == '\r') { i += 2; continue; }
            if (in[i + 1] == ' ' || in[i + 1] == '\t') { i += 2; continue; }
            auto hex = [](char x) -> int {
                if (x >= '0' && x <= '9') return x - '0';
                if (x >= 'A' && x <= 'F') return 10 + (x - 'A');
                if (x >= 'a' && x <= 'f') return 10 + (x - 'a');
                return -1;
            };
            if (i + 2 < n) {
                int h1 = hex(in[i + 1]), h2 = hex(in[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    out.push_back((uint8_t)((h1 << 4) | h2));
                    i += 3;
                    continue;
                }
            }
            out.push_back('=');
            ++i;
        } else if (c == '\r' || c == '\n') {
            if (out.empty() || out.back() != '\n') out.push_back('\n');
            ++i;
            if (c == '\r' && i < n && in[i] == '\n') ++i;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

std::wstring VcfBytesToWide(const std::vector<uint8_t>& bytes, UINT codepage) {
    if (bytes.empty()) return L"";
    DWORD flags = (codepage == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
    int wlen = MultiByteToWideChar(codepage, flags, (LPCCH)bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wlen <= 0) {
        // fallback UTF-8 then 1251
        wlen = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)bytes.data(), (int)bytes.size(), nullptr, 0);
        if (wlen > 0) codepage = CP_UTF8;
        else {
            wlen = MultiByteToWideChar(1251, 0, (LPCCH)bytes.data(), (int)bytes.size(), nullptr, 0);
            if (wlen <= 0) return L"";
            codepage = 1251;
        }
    }
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(codepage, 0, (LPCCH)bytes.data(), (int)bytes.size(), &w[0], wlen);
    return w;
}

UINT VcfCodepageFromCharset(const std::wstring& charset) {
    std::wstring cs = ToUpperASCII(charset);
    if (cs == L"UTF-8" || cs == L"UTF8") return CP_UTF8;
    if (cs == L"UTF-16" || cs == L"UTF16" || cs == L"UTF-16LE") return 1200;
    if (cs == L"WINDOWS-1251" || cs == L"CP1251" || cs == L"WIN-1251") return 1251;
    if (cs == L"KOI8-R" || cs == L"KOI8R") return 20866;
    if (cs == L"ISO-8859-5" || cs == L"ISO8859-5") return 28595;
    if (cs == L"ISO-8859-1" || cs == L"ISO8859-1" || cs == L"LATIN1") return 28591;
    return CP_UTF8;
}

static std::wstring stripQpArtifacts(std::wstring s) {
    while (!s.empty()) {
        wchar_t c = s.back();
        if (c == L'=' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') s.pop_back();
        else break;
    }
    return s;
}

std::wstring VcfDecodeTextValue(const std::wstring& raw, bool isQP, const std::wstring& charset) {
    // Does not unescape \, \n — call UnescapeVCard after if needed
    if (!isQP) {
        std::wstring t = raw;
        while (!t.empty() && t.back() == L'=') t.pop_back();
        return t;
    }
    auto bytes = VcfQuotedPrintableDecode(raw);
    std::wstring w = VcfBytesToWide(bytes, VcfCodepageFromCharset(charset));
    return stripQpArtifacts(std::move(w));
}
