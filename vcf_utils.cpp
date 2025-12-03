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
        if (j < n && block[j] == L'\r') ++j;
        if (j < n && block[j] == L'\n') ++j;
        i = j;
    }
    return lines;
}

std::vector<std::wstring> UnfoldVCard_Folded(const std::vector<std::wstring>& lines) {
    std::vector<std::wstring> out;
    for (const auto& L : lines) {
        if (!out.empty() && !L.empty() && (L[0] == L' ' || L[0] == L'\t')) {
            out.back() += L.substr(1);
        }
        else {
            out.push_back(L);
        }
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
