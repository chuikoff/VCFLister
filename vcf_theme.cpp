#include "vcf_theme.hpp"
#include <algorithm>
#include <windows.h>

// =============================================================
// Theme: TC dark mode (cm_SwitchDarkMode) preferred over OS
// =============================================================
std::wstring g_iniPath;
bool g_dark = false;
bool g_tcRu = false;
int  g_tcDarkMode = -1; // -1 unknown, 0 light, 1 dark (from ListLoad/ListSendCommand flags)
HBRUSH   g_hbrBk = nullptr;
HBRUSH   g_hbrListBg = nullptr;
COLORREF g_clrBk, g_clrTxt, g_clrSub, g_clrGrid, g_clrSeparator;
COLORREF g_clrListBg, g_clrListSel;

void SafeDelBrush(HBRUSH& b) { if (b) { DeleteObject(b); b = nullptr; } }

std::wstring GetPluginDirectory() {
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&GetPluginDirectory, &hMod) || !hMod) {
        return L"";
    }
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(hMod, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) slash = wcsrchr(path, L'/');
    if (!slash) return L"";
    *slash = L'\0';
    return path;
}

std::wstring ResolvePluginIni() {
    std::wstring dir = GetPluginDirectory();
    if (dir.empty()) return L"";
    std::wstring cand1 = dir + L"\\VCFLister.ini";
    if (GetFileAttributesW(cand1.c_str()) != INVALID_FILE_ATTRIBUTES)
        return cand1;
    std::wstring cand2 = dir + L"\\VcfLister.ini";
    if (GetFileAttributesW(cand2.c_str()) != INVALID_FILE_ATTRIBUTES)
        return cand2;
    return L"";
}

std::wstring ResolveWincmdIni() {
    // COMMANDER_INI is the ini TC loaded (/i=, portable). DefaultIniName may be a
    // redirected plugin ini ([ReplaceIniLocation]) and has no BackColor or LanguageIni.
    wchar_t cmdIni[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"COMMANDER_INI", cmdIni, MAX_PATH);
    if (n > 0 && n < MAX_PATH && GetFileAttributesW(cmdIni) != INVALID_FILE_ATTRIBUTES)
        return cmdIni;

    if (!g_iniPath.empty() && GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return g_iniPath;

    wchar_t appdata[MAX_PATH]{};
    DWORD na = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (na > 0 && na < MAX_PATH) {
        wchar_t out[MAX_PATH]{};
        swprintf_s(out, L"%s\\GHISLER\\wincmd.ini", appdata);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return out;
    }
    return g_iniPath;
}

static bool ReadRegDWORD(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& out) {
    HKEY h; if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = sizeof(DWORD);
    LONG r = RegGetValueW(h, nullptr, name, RRF_RT_REG_DWORD, &type, &out, &size);
    RegCloseKey(h); return r == ERROR_SUCCESS;
}
static bool DetectSystemDark() {
    DWORD v = 1; // 1=Light, 0=Dark
    if (ReadRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", v))
        return v == 0;
    return false;
}

// TC stores panel colors; dark mode typically uses dark BackColor.
// Used when ShowFlags not yet known (Dark=2 auto).
static bool DetectTCDarkFromIniColors() {
    std::wstring wincmd = ResolveWincmdIni();
    if (wincmd.empty()) return false;
    wchar_t buf[64]{};
    // Prefer Lister colors if set; fall back to file panel BackColor
    GetPrivateProfileStringW(L"Lister", L"BackColor", L"", buf, 64, wincmd.c_str());
    if (!buf[0])
        GetPrivateProfileStringW(L"Colors", L"BackColor", L"", buf, 64, wincmd.c_str());
    if (!buf[0]) return false;
    // Formats: "R,G,B" or single COLORREF decimal
    int r = 0, g = 0, b = 0;
    if (swscanf_s(buf, L"%d,%d,%d", &r, &g, &b) == 3) {
        // luminance
        return (0.299 * r + 0.587 * g + 0.114 * b) < 128.0;
    }
    long v = wcstol(buf, nullptr, 10);
    if (v != 0 || buf[0] == L'0') {
        r = GetRValue((COLORREF)v);
        g = GetGValue((COLORREF)v);
        b = GetBValue((COLORREF)v);
        return (0.299 * r + 0.587 * g + 0.114 * b) < 128.0;
    }
    return false;
}

static int ReadIniDarkMode() {
    wchar_t buf[16]{};
    std::wstring pluginIni = ResolvePluginIni();
    if (!pluginIni.empty()) {
        GetPrivateProfileStringW(L"VCFLister", L"Dark", L"", buf, 16, pluginIni.c_str());
        if (buf[0] == L'0') return 0;
        if (buf[0] == L'1') return 1;
        if (buf[0] == L'2') return 2;
    }
    if (!g_iniPath.empty()) {
        GetPrivateProfileStringW(L"VCFLister", L"Dark", L"2", buf, 16, g_iniPath.c_str());
        if (buf[0] == L'0') return 0;
        if (buf[0] == L'1') return 1;
        return 2; // auto → TC dark mode
    }
    return 2;
}

static bool DetectTCRussian() {
    wchar_t buf[MAX_PATH]{};
    std::wstring pluginIni = ResolvePluginIni();
    // 1) Explicit setting: [VCFLister] Language=auto|RUS|ENG
    if (!pluginIni.empty()) {
        GetPrivateProfileStringW(L"VCFLister", L"Language", L"", buf, MAX_PATH, pluginIni.c_str());
    }
    if (!buf[0] && !g_iniPath.empty()) {
        GetPrivateProfileStringW(L"VCFLister", L"Language", L"", buf, MAX_PATH, g_iniPath.c_str());
    }
    if (buf[0]) {
        std::wstring s = buf;
        std::wstring low; low.resize(s.size());
        std::transform(s.begin(), s.end(), low.begin(), ::towlower);
        if (low.find(L"rus") != std::wstring::npos || low.find(L"russian") != std::wstring::npos) return true;
        if (low.find(L"eng") != std::wstring::npos || low.find(L"english") != std::wstring::npos) return false;
    }

    // 2) TC configuration from wincmd.ini
    std::wstring wincmd = ResolveWincmdIni();
    if (!wincmd.empty()) {
        buf[0] = 0;
        GetPrivateProfileStringW(L"Configuration", L"LanguageIni", L"", buf, MAX_PATH, wincmd.c_str());
        if (!buf[0])
            GetPrivateProfileStringW(L"Configuration", L"LanguageDll", L"", buf, MAX_PATH, wincmd.c_str());
        if (buf[0]) {
            std::wstring s = buf;
            std::wstring low; low.resize(s.size());
            std::transform(s.begin(), s.end(), low.begin(), ::towlower);
            if (low.find(L"rus") != std::wstring::npos || low.find(L"russian") != std::wstring::npos)
                return true;
            return false;
        }
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
}

void RecomputeTheme() {
    const int ini = ReadIniDarkMode();
    g_tcRu = DetectTCRussian();

    if (ini == 0) {
        g_dark = false;
    } else if (ini == 1) {
        g_dark = true;
    } else {
        // Auto: follow Total Commander dark mode (cm_SwitchDarkMode), NOT OS
        if (g_tcDarkMode == 0) g_dark = false;
        else if (g_tcDarkMode == 1) g_dark = true;
        else if (DetectTCDarkFromIniColors()) g_dark = true;
        else g_dark = false; // default light when unknown (do not use Windows theme)
    }

    if (g_dark) {
        g_clrBk = RGB(32, 32, 32);
        g_clrTxt = RGB(220, 220, 220);
        g_clrSub = RGB(160, 160, 160);
        g_clrGrid = RGB(64, 64, 64);
        g_clrSeparator = RGB(70, 70, 70);
        g_clrListBg = RGB(28, 28, 28);
        g_clrListSel = RGB(60, 80, 120);
    }
    else {
        g_clrBk = RGB(255, 255, 255);
        g_clrTxt = RGB(30, 30, 30);
        g_clrSub = RGB(110, 110, 110);
        g_clrGrid = RGB(220, 220, 220);
        g_clrSeparator = RGB(200, 200, 200);
        g_clrListBg = RGB(248, 248, 248);
        g_clrListSel = RGB(219, 234, 254);
    }
    SafeDelBrush(g_hbrBk);
    g_hbrBk = CreateSolidBrush(g_clrBk);
    SafeDelBrush(g_hbrListBg);
    g_hbrListBg = CreateSolidBrush(g_clrListBg);
}

extern "C" void VCFView_SetIniPath(const wchar_t* iniPath) {
    if (iniPath && *iniPath) g_iniPath = iniPath;
    RecomputeTheme();
}

extern "C" void VCFView_SetTCDarkMode(int dark /*0/1*/, HWND hWnd) {
    g_tcDarkMode = (dark != 0) ? 1 : 0;
    RecomputeTheme();
    if (hWnd && IsWindow(hWnd)) {
        VCFView_ApplyChrome(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);
        // repaint children (filter, edit, checkbox)
        EnumChildWindows(hWnd, [](HWND c, LPARAM) -> BOOL {
            InvalidateRect(c, nullptr, TRUE);
            return TRUE;
        }, 0);
    }
}

extern "C" void VCFView_RefreshTheme(HWND hWnd) {
    RecomputeTheme();
    if (IsWindow(hWnd)) {
        VCFView_ApplyChrome(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);
        EnumChildWindows(hWnd, [](HWND c, LPARAM) -> BOOL {
            InvalidateRect(c, nullptr, TRUE);
            return TRUE;
        }, 0);
    }
}
