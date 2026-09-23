#pragma once
#include <windows.h>
#include <string>

// --- theme globals ---
extern std::wstring g_iniPath;
extern bool         g_dark;
extern bool         g_tcRu;
extern int          g_tcDarkMode; // -1 unknown, 0 light, 1 dark (TC cm_SwitchDarkMode)

extern HBRUSH   g_hbrBk;
extern COLORREF g_clrBk, g_clrTxt, g_clrSub, g_clrGrid, g_clrSeparator;
extern COLORREF g_clrListBg, g_clrListSel;

void RecomputeTheme();
extern "C" void VCFView_SetIniPath(const wchar_t* iniPath);
extern "C" void VCFView_RefreshTheme(HWND hWnd);
// dark: 0=light, 1=dark — from ListLoad ShowFlags / ListSendCommand lc_newparams
extern "C" void VCFView_SetTCDarkMode(int dark, HWND hWnd);

void SafeDelBrush(HBRUSH& b);
