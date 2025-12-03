#pragma once
#include <string>
#include <vector>
#include <cwctype>
#include <algorithm>

std::wstring Trim(const std::wstring& s);
std::wstring ToUpperASCII(const std::wstring& s);
std::vector<std::wstring> SplitLines(const std::wstring& block);
std::vector<std::wstring> UnfoldVCard_Folded(const std::vector<std::wstring>& lines);

// case-insensitive utils
std::wstring LowerInvariant(const std::wstring& s);
inline bool isWordBoundary(const std::wstring& s, size_t pos) { return (pos == 0) || !iswalnum(s[pos - 1]); }
inline bool isWordBoundary2(const std::wstring& s, size_t pos) { return (pos >= s.size()) || !iswalnum(s[pos]); }
