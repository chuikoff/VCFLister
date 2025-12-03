#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <memory>

//!!! Важно!!! — подключите ваш файл с Contact:
// Пример:
#include "vcf_parser.hpp" // или "vcfview.hpp" — НЕ добавляйте struct Contact!

void DrawContactPhoto(
    Gdiplus::Graphics& g,
    HWND hWnd,
    const Contact& c,
    int dx, int& dy, int dw, const RECT& rc,
    const std::wstring& name
);
