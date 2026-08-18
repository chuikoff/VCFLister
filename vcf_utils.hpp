#pragma once
#include <string>
#include <vector>
#include <cwctype>
#include <algorithm>
#include <cstdint>
#include <windows.h>

std::wstring Trim(const std::wstring& s);
std::wstring ToUpperASCII(const std::wstring& s);
std::vector<std::wstring> SplitLines(const std::wstring& block);
std::vector<std::wstring> UnfoldVCard_Folded(const std::vector<std::wstring>& lines);
std::wstring unquote(const std::wstring& s);

// case-insensitive utils
std::wstring LowerInvariant(const std::wstring& s);
inline bool isWordBoundary(const std::wstring& s, size_t pos) { return (pos == 0) || !iswalnum(s[pos - 1]); }
inline bool isWordBoundary2(const std::wstring& s, size_t pos) { return (pos >= s.size()) || !iswalnum(s[pos]); }

// --- Shared vCard codecs (single implementation for parser + view) ---
std::wstring UnescapeVCard(const std::wstring& s);
std::vector<uint8_t> VcfBase64Decode(const std::wstring& wsrc);
std::vector<uint8_t> VcfQuotedPrintableDecode(const std::wstring& wsrc);
std::wstring VcfBytesToWide(const std::vector<uint8_t>& bytes, UINT codepage);
UINT VcfCodepageFromCharset(const std::wstring& charset);
// Decode text property value (optional QP + charset → Unicode, then unescape)
std::wstring VcfDecodeTextValue(const std::wstring& raw, bool isQP, const std::wstring& charset);
