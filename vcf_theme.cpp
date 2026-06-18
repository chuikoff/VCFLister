#include "vcf_theme.hpp"
#include <algorithm>
#include <windows.h>

// =============================================================
// ===================== THEME (Auto + INI override) + язык TC =====================
std::wstring g_iniPath;
bool g_dark = false;
bool g_tcRu = false;
HBRUSH   g_hbrBk = nullptr;
COLORREF g_clrBk, g_clrTxt, g_clrSub, g_clrGrid, g_clrSeparator;
COLORREF g_clrListBg, g_clrListSel;

void SafeDelBrush(HBRUSH& b) { if (b) { DeleteObject(b); b = nullptr; } }

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
static int ReadIniDarkMode() {
    if (g_iniPath.empty()) return 2;
    wchar_t buf[16]{};
    GetPrivateProfileStringW(L"VCFLister", L"Dark", L"2", buf, 16, g_iniPath.c_str());
    if (buf[0] == L'0') return 0;
    if (buf[0] == L'1') return 1;
    return 2;
}
// ���������� ���� TC �� [Configuration] LanguageIni=...
static bool DetectTCRussian() {
    if (g_iniPath.empty()) return false;
    wchar_t buf[MAX_PATH]{};
    GetPrivateProfileStringW(L"Configuration", L"LanguageIni", L"", buf, MAX_PATH, g_iniPath.c_str());
    std::wstring s = buf;
    std::wstring low; low.resize(s.size());
    std::transform(s.begin(), s.end(), low.begin(), ::towlower);
    if (low.find(L"rus") != std::wstring::npos || low.find(L"russian") != std::wstring::npos)
        return true;
    return false;
}

void RecomputeTheme() {
    const int ini = ReadIniDarkMode();
    const bool sysDark = DetectSystemDark();
    g_dark = (ini == 2) ? sysDark : (ini == 1);
    g_tcRu = DetectTCRussian();

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
}
extern "C" void VCFView_SetIniPath(const wchar_t* iniPath) { if (iniPath && *iniPath) g_iniPath = iniPath; RecomputeTheme(); }
extern "C" void VCFView_RefreshTheme(HWND hWnd) { RecomputeTheme(); if (IsWindow(hWnd)) InvalidateRect(hWnd, nullptr, TRUE); }

// ��������: ������ � ������ �� ������.
