#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "vcf_parser.hpp"

// create and manage our custom view
HWND  CreateVCFView(HWND parent, const std::vector<Contact>& contacts);
void  VCFView_SetContacts(HWND h, const std::vector<Contact>& contacts);

// Simple search (kept for compatibility)
bool  VCFView_Search(HWND h, const std::wstring& needle);

// Extended search used by Lister (F3/Shift+F3)
bool  VCFView_SearchEx(HWND h, const std::wstring& needle,
    size_t startIndex, bool backwards,
    bool matchCase, bool wholeWord, bool wrap);

// Query/modify selection and count (for ListSearchTextW)
size_t VCFView_Count(HWND h);
size_t VCFView_GetSelection(HWND h);
void   VCFView_SetSelection(HWND h, size_t idx);

// Copy last activated (click) text to clipboard (optional hotkeys)
bool  VCFView_CopyActive(HWND h);