#pragma once
#include <windows.h>
#include <string>

// --- Глобалы темы (точно как были в vcf_view.cpp) ---
extern std::wstring g_iniPath;
extern bool         g_dark;
extern bool         g_tcRu;

extern HBRUSH   g_hbrBk;
extern COLORREF g_clrBk, g_clrTxt, g_clrSub, g_clrGrid, g_clrSeparator;
extern COLORREF g_clrListBg, g_clrListSel;

// --- Функции темы (точно как были в vcf_view.cpp) ---
void RecomputeTheme();
extern "C" void VCFView_SetIniPath(const wchar_t* iniPath);
extern "C" void VCFView_RefreshTheme(HWND hWnd);

// --- Хелпер, который раньше был static ---
void SafeDelBrush(HBRUSH& b);
