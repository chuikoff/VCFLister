// dllmain.cpp — Total Commander Lister plugin (VCF Lister)
// x64: Unicode (W-версии) экспортируются напрямую;
// x86: экспорт ANSI-имён через .def + тонкие A→W обёртки.
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
#include <cstring> // strlen/strcpy_s

#include "vcf_parser.hpp"
#include "vcf_view.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Минимум из listplug.h (чтобы избежать конфликтов линковки/объявлений)
#ifndef LISTPLUG_MINI
#define LISTPLUG_MINI

#define lc_copy         1
#define lc_newparams    2
#define lc_selectall    3
#define lc_setpercent   4

#define lcp_wraptext    1
#define lcp_fittowindow 2
#define lcp_ansi        4
#define lcp_ascii       8
#define lcp_variable    12
#define lcp_forceshow   16

#define lcs_findfirst   1
#define lcs_matchcase   2
#define lcs_wholewords  4
#define lcs_backwards   8

typedef struct {
    int   size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char  DefaultIniName[MAX_PATH];
} ListDefaultParamStruct;

#define LISTPLUGIN_OK     0
#define LISTPLUGIN_ERROR  1
#endif // LISTPLUG_MINI
// ─────────────────────────────────────────────────────────────────────────────

// Экспорт только для x64 (x86 экспортируется через .def)
#if defined(_WIN64)
#define WLX_EXPORT extern "C" __declspec(dllexport)
#else
#define WLX_EXPORT extern "C"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Глобалы и утилиты
static HINSTANCE g_hInst = nullptr;

// ANSI → UTF-16
static std::wstring A2W(const char* s) {
    if (!s) return L"";
    int need = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (need <= 0) return L"";
    std::wstring w; w.resize(need);
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], need);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// Прочитать файл целиком в UTF-16 (детект UTF-8/1251/BOM)
static std::wstring ReadWholeFileAsWide(const wchar_t* path) {
    std::wstring empty;
    std::ifstream f(path, std::ios::binary);
    if (!f) return empty;

    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) return empty;

    auto tryMbToW = [](const char* data, int len, UINT cp, bool strictUTF8 = false)->std::wstring {
        DWORD flags = (strictUTF8 && cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
        int wlen = MultiByteToWideChar(cp, flags, data, len, nullptr, 0);
        if (wlen <= 0) return L"";
        std::wstring w(wlen, L'\0');
        if (!MultiByteToWideChar(cp, flags, data, len, w.data(), wlen)) return L"";
        return w;
        };

    // UTF-16LE BOM
    if (buf.size() >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(buf.data() + 2);
        size_t wlen = (buf.size() - 2) / sizeof(wchar_t);
        return std::wstring(w, w + wlen);
    }
    // UTF-8 BOM
    if (buf.size() >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        return tryMbToW(buf.data() + 3, (int)buf.size() - 3, CP_UTF8, true);
    }

    // 1) строгий UTF-8
    std::wstring w = tryMbToW(buf.data(), (int)buf.size(), CP_UTF8, true);
    if (!w.empty()) return w;
    // 2) 1251
    w = tryMbToW(buf.data(), (int)buf.size(), 1251);
    if (!w.empty()) return w;
    // 3) OEM
    w = tryMbToW(buf.data(), (int)buf.size(), CP_OEMCP);
    if (!w.empty()) return w;
    // 4) ACP
    return tryMbToW(buf.data(), (int)buf.size(), CP_ACP);
}

// Guards: не даём исключениям вылетать в Total Commander
#define TRY_API     try {
#define CATCH_API(r) } catch (...) { return (r); }
#define TRY_VOID    try {
#define CATCH_VOID  } catch (...) { /* swallow */ }

// ─────────────────────────────────────────────────────────────────────────────
// DllMain
BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// ВНУТРЕННИЕ Unicode-реализации (общие)
static HWND Impl_ListLoadW(HWND ParentWin, const wchar_t* FileToLoad, int /*ShowFlags*/) {
    if (!ParentWin || !FileToLoad) return nullptr;
    std::wstring text = ReadWholeFileAsWide(FileToLoad);
    if (text.empty()) return nullptr;
    std::vector<Contact> contacts = ParseVCard(text);
    return CreateVCFView(ParentWin, contacts);
}

static int Impl_ListLoadNextW(HWND ParentWin, HWND PluginWin, const wchar_t* FileToLoad, int /*ShowFlags*/) {
    if (!ParentWin || !PluginWin || !FileToLoad) return LISTPLUGIN_ERROR;
    std::wstring text = ReadWholeFileAsWide(FileToLoad);
    if (text.empty()) return LISTPLUGIN_ERROR;
    std::vector<Contact> contacts = ParseVCard(text);
    VCFView_SetContacts(PluginWin, contacts);
    return LISTPLUGIN_OK;
}

static int Impl_ListSearchTextW(HWND PluginWin, const wchar_t* SearchString, int SearchParameter) {
    if (!PluginWin || !SearchString) return LISTPLUGIN_ERROR;

    const bool findFirst = (SearchParameter & lcs_findfirst) != 0;
    const bool matchCase = (SearchParameter & lcs_matchcase) != 0;
    const bool whole = (SearchParameter & lcs_wholewords) != 0;
    const bool backwards = (SearchParameter & lcs_backwards) != 0;

    const size_t count = VCFView_Count(PluginWin);
    if (count == 0) return LISTPLUGIN_ERROR;

    size_t start = VCFView_GetSelection(PluginWin);
    if (findFirst) {
        start = backwards ? (count - 1) : 0;
    }
    else {
        start = backwards ? (start == 0 ? count - 1 : start - 1)
            : (start + 1) % count;
    }

    const bool ok = VCFView_SearchEx(PluginWin, SearchString, start, backwards, matchCase, whole, /*wrap*/true);
    return ok ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

static int Impl_ListSendCommand(HWND PluginWin, int Command, int Parameter) {
    (void)Parameter;
    if (!PluginWin) return 0;
    if (Command == lc_selectall) {
        return 1; // успех
    }
    return 0;
}

static void Impl_ListSetDefaultParams(const ListDefaultParamStruct* /*dps*/) {
    // no-op
}

// ─────────────────────────────────────────────────────────────────────────────
// x64: EXPORT Unicode-версий (без .def). Определения ТОЛЬКО здесь.
#if defined(_WIN64)

WLX_EXPORT HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int ShowFlags) {
    TRY_API
        return Impl_ListLoadW(ParentWin, FileToLoad, ShowFlags);
    CATCH_API(NULL)
}

WLX_EXPORT int __stdcall ListLoadNextW(HWND ParentWin, HWND PluginWin, wchar_t* FileToLoad, int ShowFlags) {
    TRY_API
        return Impl_ListLoadNextW(ParentWin, PluginWin, FileToLoad, ShowFlags);
    CATCH_API(LISTPLUGIN_ERROR)
}

WLX_EXPORT int __stdcall ListSearchTextW(HWND PluginWin, wchar_t* SearchString, int SearchParameter) {
    TRY_API
        return Impl_ListSearchTextW(PluginWin, SearchString, SearchParameter);
    CATCH_API(LISTPLUGIN_ERROR)
}

WLX_EXPORT int __stdcall ListSendCommand(HWND PluginWin, int Command, int Parameter) {
    TRY_API
        return Impl_ListSendCommand(PluginWin, Command, Parameter);
    CATCH_API(0)
}

WLX_EXPORT void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps) {
    TRY_VOID
        Impl_ListSetDefaultParams(dps);
    CATCH_VOID
}

WLX_EXPORT void __stdcall ListCloseWindow(HWND ListWin) {
    TRY_VOID
        if (ListWin && IsWindow(ListWin)) DestroyWindow(ListWin);
    CATCH_VOID
}

// Unicode-детект строка
WLX_EXPORT int __stdcall ListGetDetectStringW(wchar_t* DetectString, int maxlen) {
    TRY_API
        if (!DetectString || maxlen <= 0) return 0;
    const wchar_t* ds = L"EXT=\"VCF\" | EXT=\"VCARD\"";
    int need = (int)lstrlenW(ds) + 1;
    if (need > maxlen) {
        lstrcpynW(DetectString, ds, maxlen);
    }
    else {
        lstrcpyW(DetectString, ds);
    }
    return 1;
    CATCH_API(0)
}

#else
// ─────────────────────────────────────────────────────────────────────────────
// x86: EXPORT ANSI-имён через .def + обёртки. Определения ТОЛЬКО здесь.
extern "C" {

    int __stdcall ListGetDetectString(char* DetectString, int maxlen) {
        TRY_API
            const char* ds = "EXT=\"VCF\" | EXT=\"VCARD\"";
        if (!DetectString || maxlen <= 0) return 0;
        size_t need = std::strlen(ds) + 1;
        if ((int)need > maxlen) {
            strncpy_s(DetectString, maxlen, ds, _TRUNCATE);
        }
        else {
            strcpy_s(DetectString, maxlen, ds);
        }
        return 1;
        CATCH_API(0)
    }

    HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags) {
        TRY_API
            std::wstring wfile = A2W(FileToLoad);
        return Impl_ListLoadW(ParentWin, wfile.c_str(), ShowFlags);
        CATCH_API(NULL)
    }

    int __stdcall ListLoadNext(HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags) {
        TRY_API
            std::wstring wfile = A2W(FileToLoad);
        return Impl_ListLoadNextW(ParentWin, PluginWin, wfile.c_str(), ShowFlags);
        CATCH_API(LISTPLUGIN_ERROR)
    }

    int __stdcall ListSearchText(HWND PluginWin, char* SearchString, int SearchParameter) {
        TRY_API
            std::wstring w = A2W(SearchString);
        return Impl_ListSearchTextW(PluginWin, w.c_str(), SearchParameter);
        CATCH_API(LISTPLUGIN_ERROR)
    }

    int __stdcall ListSendCommand(HWND PluginWin, int Command, int Parameter) {
        TRY_API
            return Impl_ListSendCommand(PluginWin, Command, Parameter);
        CATCH_API(0)
    }

    void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps) {
        TRY_VOID
            Impl_ListSetDefaultParams(dps);
        CATCH_VOID
    }

    void __stdcall ListCloseWindow(HWND ListWin) {
        TRY_VOID
            if (ListWin && IsWindow(ListWin)) DestroyWindow(ListWin);
        CATCH_VOID
    }

} // extern "C"
#endif // _WIN64
