// dllmain.cpp — Total Commander WLX64 плагин (VCF Lister)
#define UNICODE
#define _UNICODE
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>

#include "listplug.h"      // из TC SDK (wlx)
#include "vcf_parser.hpp"  // наш парсер vCard
#include "vcf_view.hpp"    // наше окно-рендер

// ---- guards: не даем исключениям вылетать в Total Commander ----
#define TRY_API     try {
#define CATCH_API(r) } catch (...) { return (r); }

#define TRY_VOID    try {
#define CATCH_VOID  } catch (...) { /* swallow */ }

// ---------- утилита: читаем файл в wstring ----------
static std::wstring ReadWholeFileAsWide(const wchar_t* path)
{
    std::wstring empty;
    std::ifstream f(path, std::ios::binary);
    if (!f) return empty;

    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) return empty;

    auto toWide = [](const char* data, int len, UINT cp)->std::wstring {
        int wlen = MultiByteToWideChar(cp, 0, data, len, nullptr, 0);
        if (wlen <= 0) return L"";
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(cp, 0, data, len, w.data(), wlen);
        return w;
        };

    // UTF-16LE BOM
    if (buf.size() >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        const wchar_t* w = (const wchar_t*)(buf.data() + 2);
        size_t wlen = (buf.size() - 2) / sizeof(wchar_t);
        return std::wstring(w, w + wlen);
    }
    // UTF-8 BOM
    if (buf.size() >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        return toWide(buf.data() + 3, (int)buf.size() - 3, CP_UTF8);
    }

    // UTF-8 без BOM -> CP1251 -> ACP
    std::wstring w = toWide(buf.data(), (int)buf.size(), CP_UTF8);
    if (!w.empty()) return w;
    w = toWide(buf.data(), (int)buf.size(), 1251);
    if (!w.empty()) return w;
    return toWide(buf.data(), (int)buf.size(), CP_ACP);
}

// -------------------- ЭКСПОРТЫ ПЛАГИНА --------------------

// ВАЖНО: в проекте должна остаться РОВНО ОДНА такая функция (ANSI, int)
int __stdcall ListGetDetectString(char* DetectString, int maxlen)
{
    TRY_API
        const char* s = "EXT=\"VCF\" | EXT=\"VCARD\"";
    if (!DetectString || maxlen <= 0) return 0;

    size_t need = strlen(s) + 1;
    if ((int)need > maxlen) {
        strncpy_s(DetectString, maxlen, s, _TRUNCATE);
    }
    else {
        strcpy_s(DetectString, maxlen, s);
    }
    return 1;
    CATCH_API(0)
}

HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int /*ShowFlags*/)
{
    TRY_API
        if (!ParentWin || !FileToLoad) return nullptr;

    std::wstring text = ReadWholeFileAsWide(FileToLoad);
    if (text.empty()) return nullptr;

    std::vector<Contact> contacts = ParseVCard(text);
    HWND view = CreateVCFView(ParentWin, contacts);
    return view;
    CATCH_API(NULL)
}

void __stdcall ListCloseWindow(HWND ListWin)
{
    TRY_VOID
        if (ListWin && IsWindow(ListWin)) DestroyWindow(ListWin);
    CATCH_VOID
}

// Поиск (Ctrl+F, F3/Shift+F3)
int __stdcall ListSearchTextW(HWND ListWin, wchar_t* SearchString, int SearchParameter)
{
    TRY_API
        if (!ListWin || !SearchString) return LISTPLUGIN_ERROR;

    // fallback-флаги (если старый listplug.h)
#ifndef lcs_findfirst
#define lcs_findfirst 1
#endif
#ifndef lcs_matchcase
#define lcs_matchcase 2
#endif
#ifndef lcs_wholewords
#define lcs_wholewords 4
#endif
#ifndef lcs_backwards
#define lcs_backwards 8
#endif

    const bool findFirst = (SearchParameter & lcs_findfirst) != 0;
    const bool matchCase = (SearchParameter & lcs_matchcase) != 0;
    const bool whole = (SearchParameter & lcs_wholewords) != 0;
    const bool backwards = (SearchParameter & lcs_backwards) != 0;

    const size_t count = VCFView_Count(ListWin);
    if (count == 0) return LISTPLUGIN_ERROR;

    size_t start = VCFView_GetSelection(ListWin);
    if (findFirst) {
        start = backwards ? (count - 1) : 0;
    }
    else {
        start = backwards ? (start == 0 ? count - 1 : start - 1)
            : (start + 1) % count;
    }

    const bool ok = VCFView_SearchEx(ListWin, SearchString, start, backwards, matchCase, whole, /*wrap*/true);
    return ok ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
    CATCH_API(LISTPLUGIN_ERROR)
}

// -------------------- DllMain --------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
