// dllmain.cpp — Total Commander Lister plugin (VCF Lister)
// Добавлено: передаём в viewer сырьевые блоки vCard для полного вывода всех полей (v3/v4)
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <excpt.h>  // Оставляем, но не используем

#include "vcf_parser.hpp"
#include "vcf_view.hpp"
#include "vcf_utils.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Минимум из listplug.h
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
#endif
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN64)
#define WLX_EXPORT extern "C" __declspec(dllexport)
#else
#define WLX_EXPORT extern "C"
#endif

static HINSTANCE g_hInst = nullptr;

// Из viewer-а
extern "C" void VCFView_SetIniPath(const wchar_t* iniPath);
extern "C" void VCFView_SetRawBlocks(HWND hView, const std::vector<std::wstring>& rawBlocks);

// ANSI → UTF-16 (твоя версия с malloc, чтобы no unwinding)
static std::wstring A2W(const char* s) {
    if (!s) return L"";
    int need = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (need <= 0) return L"";
    wchar_t* buf = (wchar_t*)malloc(need * sizeof(wchar_t));
    if (!buf) return L"";
    MultiByteToWideChar(CP_ACP, 0, s, -1, buf, need);
    std::wstring w(buf);
    free(buf);
    return w;
}

// Прочитать файл целиком (wide), с попытками кодировок
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
    // строгий UTF-8 (без BOM) → нестрогий UTF-8 → 1251 → OEM → ACP
    std::wstring w = tryMbToW(buf.data(), (int)buf.size(), CP_UTF8, true);
    if (!w.empty()) return w;
    // пробуем UTF-8 без строгой проверки (для файлов UTF-8 без BOM)
    w = tryMbToW(buf.data(), (int)buf.size(), CP_UTF8, false);
    if (!w.empty()) return w;
    w = tryMbToW(buf.data(), (int)buf.size(), 1251);
    if (!w.empty()) return w;
    w = tryMbToW(buf.data(), (int)buf.size(), CP_OEMCP);
    if (!w.empty()) return w;
    return tryMbToW(buf.data(), (int)buf.size(), CP_ACP);
}

// Разбить исходный текст на блоки BEGIN:VCARD ... END:VCARD (v3/v4)
static std::vector<std::wstring> SplitVCardBlocks(const std::wstring& text) {
    std::vector<std::wstring> out;
    const std::wstring begin = L"BEGIN:VCARD";
    const std::wstring end = L"END:VCARD";

    size_t i = 0, n = text.size();
    while (i < n) {
        size_t b = text.find(begin, i);
        if (b == std::wstring::npos) break;
        size_t e = text.find(end, b);
        if (e == std::wstring::npos) { // последний незакрытый — берём до конца
            out.push_back(text.substr(b));
            break;
        }
        e = text.find_first_of(L"\r\n", e); // до конца строки после END:VCARD
        if (e == std::wstring::npos) e = n;
        std::wstring block = text.substr(b, e - b);
        // Show ALL cards, even completely empty ones (user request). Keep sync with parser.
        out.push_back(block);
        i = e;
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Общие Unicode-реализации
static HWND Impl_ListLoadW(HWND ParentWin, const wchar_t* FileToLoad, int /*ShowFlags*/) {
    if (!ParentWin || !FileToLoad) return nullptr;
    std::wstring text = ReadWholeFileAsWide(FileToLoad);
    if (text.empty()) return nullptr;

    // Разбираем контакты как раньше
    std::vector<Contact> contacts = ParseVCard(text);

    // А также сохраняем сырые блоки для полного вывода всех полей
    std::vector<std::wstring> rawBlocks = SplitVCardBlocks(text);

    HWND hView = CreateVCFView(ParentWin, contacts);
    if (hView) VCFView_SetRawBlocks(hView, rawBlocks);
    return hView;
}

static int Impl_ListLoadNextW(HWND ParentWin, HWND PluginWin, const wchar_t* FileToLoad, int /*ShowFlags*/) {
    if (!ParentWin || !PluginWin || !FileToLoad) return LISTPLUGIN_ERROR;

    std::wstring text = ReadWholeFileAsWide(FileToLoad);
    if (text.empty()) return LISTPLUGIN_ERROR;

    std::vector<Contact> contacts = ParseVCard(text);
    std::vector<std::wstring> rawBlocks = SplitVCardBlocks(text);

    // Ensure the view is properly updated with new contacts and raw blocks
    VCFView_SetContacts(PluginWin, contacts);
    VCFView_SetRawBlocks(PluginWin, rawBlocks);
    
    // Ensure the selection and display are properly reset/updated
    if (!contacts.empty()) {
        VCFView_SetSelection(PluginWin, 0); // Set selection to first contact
    } else {
        // If no contacts, still ensure the display is refreshed
        InvalidateRect(PluginWin, NULL, TRUE);
    }
    
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
    if (Command == lc_selectall) return 1;
    return 0;
}

static void Impl_ListSetDefaultParams(const ListDefaultParamStruct* dps) {
    if (!dps) return;
    std::wstring wideIni = A2W(dps->DefaultIniName);
    if (!wideIni.empty()) VCFView_SetIniPath(wideIni.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// x64: экспорт Unicode (с __try, так как x64 ok)
#if defined(_WIN64)

WLX_EXPORT HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int ShowFlags) {
    __try {
        return Impl_ListLoadW(ParentWin, FileToLoad, ShowFlags);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}
WLX_EXPORT int __stdcall ListLoadNextW(HWND ParentWin, HWND PluginWin, wchar_t* FileToLoad, int ShowFlags) {
    __try {
        return Impl_ListLoadNextW(ParentWin, PluginWin, FileToLoad, ShowFlags);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return LISTPLUGIN_ERROR;
    }
}
WLX_EXPORT int __stdcall ListSearchTextW(HWND PluginWin, wchar_t* SearchString, int SearchParameter) {
    __try {
        return Impl_ListSearchTextW(PluginWin, SearchString, SearchParameter);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return LISTPLUGIN_ERROR;
    }
}
WLX_EXPORT int __stdcall ListSendCommand(HWND PluginWin, int Command, int Parameter) {
    __try {
        return Impl_ListSendCommand(PluginWin, Command, Parameter);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
WLX_EXPORT void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps) {
    __try {
        Impl_ListSetDefaultParams(dps);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Игнорируем
    }
}
WLX_EXPORT void __stdcall ListCloseWindow(HWND ListWin) {
    __try {
        if (ListWin && IsWindow(ListWin)) DestroyWindow(ListWin);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Игнорируем
    }
}
WLX_EXPORT int __stdcall ListGetDetectStringW(wchar_t* DetectString, int maxlen) {
    __try {
        if (!DetectString || maxlen <= 0) return 0;
        const wchar_t* ds = L"EXT=\"VCF\" | EXT=\"VCARD\"";
        lstrcpynW(DetectString, ds, maxlen);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

#else
// x86: экспорт ANSI + Unicode без __try
extern "C" {
    int __stdcall ListGetDetectString(char* DetectString, int maxlen) {
        const char* ds = "EXT=\"VCF\" | EXT=\"VCARD\"";
        if (!DetectString || maxlen <= 0) return 0;
        strncpy_s(DetectString, maxlen, ds, _TRUNCATE);
        return 1;
    }
    HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags) {
        std::wstring wfile = A2W(FileToLoad);
        return Impl_ListLoadW(ParentWin, wfile.c_str(), ShowFlags);
    }
    int __stdcall ListLoadNext(HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags) {
        std::wstring wfile = A2W(FileToLoad);
        return Impl_ListLoadNextW(ParentWin, PluginWin, wfile.c_str(), ShowFlags);
    }
    int __stdcall ListSearchText(HWND PluginWin, char* SearchString, int SearchParameter) {
        std::wstring w = A2W(SearchString);
        return Impl_ListSearchTextW(PluginWin, w.c_str(), SearchParameter);
    }
    int __stdcall ListSendCommand(HWND PluginWin, int Command, int Parameter) {
        return Impl_ListSendCommand(PluginWin, Command, Parameter);
    }
    void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps) {
        Impl_ListSetDefaultParams(dps);
    }
    void __stdcall ListCloseWindow(HWND ListWin) {
        if (ListWin && IsWindow(ListWin)) DestroyWindow(ListWin);
    }

    // Добавлено: Unicode версии для x86 (без __try)
    HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int ShowFlags) {
        return Impl_ListLoadW(ParentWin, FileToLoad, ShowFlags);
    }
    int __stdcall ListSearchTextW(HWND PluginWin, wchar_t* SearchString, int SearchParameter) {
        return Impl_ListSearchTextW(PluginWin, SearchString, SearchParameter);
    }
    int __stdcall ListGetDetectStringW(wchar_t* DetectString, int maxlen) {
        if (!DetectString || maxlen <= 0) return 0;
        const wchar_t* ds = L"EXT=\"VCF\" | EXT=\"VCARD\"";
        lstrcpynW(DetectString, ds, maxlen);
        return 1;
    }
}
#endif

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}