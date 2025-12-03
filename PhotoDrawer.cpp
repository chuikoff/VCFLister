// PhotoDrawer.cpp
// Исправленная версия с NOMINMAX, корректным использованием std::min и без двойного освобождения памяти.

#undef Font
#undef FontFamily

#define NOMINMAX             // важно: чтобы windows.h не подменял min/max макросами
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>         // для std::min / std::max

#include "PhotoDrawer.h"

// (опционально, если линковка GDI+ не прописана в проекте)
// #pragma comment(lib, "gdiplus.lib")

// БЕЗ using namespace Gdiplus!

// Преобразование массива байт (JPEG/PNG/...) в Gdiplus::Image
std::unique_ptr<Gdiplus::Image> ImageFromBytes(const std::vector<unsigned char>& bytes)
{
    if (bytes.empty()) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hMem) return nullptr;

    void* p = GlobalLock(hMem);
    if (!p) {
        GlobalFree(hMem);
        return nullptr;
    }

    memcpy(p, bytes.data(), bytes.size());
    GlobalUnlock(hMem);

    IStream* stm = nullptr;
    // ВАЖНО: TRUE => поток освободит hMem внутри Release()
    if (CreateStreamOnHGlobal(hMem, TRUE, &stm) != S_OK) {
        GlobalFree(hMem); // здесь освобождаем сами, потому что поток не создан
        return nullptr;
    }

    Gdiplus::Image* raw = Gdiplus::Image::FromStream(stm);
    // Release() освободит и hMem, т.к. выше был TRUE
    stm->Release();

    if (!raw || raw->GetLastStatus() != Gdiplus::Ok) {
        delete raw; // на случай, если raw != nullptr, но статус не Ok
        return nullptr;
    }

    return std::unique_ptr<Gdiplus::Image>(raw);
}

// Рисование фото контакта или заглушки с буквой
void DrawContactPhoto(
    Gdiplus::Graphics& g,
    HWND hWnd,
    const Contact& c,
    int dx, int& dy, int dw, const RECT& rc,
    const std::wstring& name)
{
    // Локальный помощник для перевода логических пикселей в физические с учётом DPI
    auto S = [](HWND h, int px) -> int {
        HDC dc = GetDC(h);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(h, dc);
        return MulDiv(px, dpi ? dpi : 96, 96);
        };

    if (c.photo && !c.photo->bytes.empty()) {
        auto img = ImageFromBytes(c.photo->bytes);
        if (img) {
            const int maxW = dw;
            int availableH = (rc.bottom - dy) - S(hWnd, 24);
            if (availableH < S(hWnd, 60)) availableH = S(hWnd, 60);

            const int iw = static_cast<int>(img->GetWidth());
            const int ih = static_cast<int>(img->GetHeight());
            if (iw > 0 && ih > 0) {
                // Вызовы std::min в скобках — безопасно при наличии макросов min/max (если вдруг где-то ещё).
                const double scale =
                    (std::min)(1.0, (std::min)(static_cast<double>(maxW) / iw,
                        static_cast<double>(availableH) / ih));

                const int drawW = static_cast<int>(iw * scale);
                const int drawH = static_cast<int>(ih * scale);

                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(img.get(), dx, dy, drawW, drawH);

                dy += drawH + S(hWnd, 8);
            }
        }
    }
    else {
        // Заглушка — круг + первая буква имени (или "?")
        const int phSize = S(hWnd, 64);

        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(230, 230, 230));
        g.FillEllipse(&bgBrush, dx, dy, phSize, phSize);

        std::wstring phLetter = L"?";
        if (!name.empty())
            phLetter = name.substr(0, 1);

        // Очень строгое объявление объектов шрифта — без using namespace Gdiplus
        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font font(&fontFamily, static_cast<Gdiplus::REAL>(S(hWnd, 28)),
            Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        // Центрирование текста по кругу (чуть аккуратнее, чем nullptr для StringFormat)
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        Gdiplus::SolidBrush letterBrush(Gdiplus::Color(200, 100, 100));
        Gdiplus::RectF rectF(static_cast<Gdiplus::REAL>(dx),
            static_cast<Gdiplus::REAL>(dy),
            static_cast<Gdiplus::REAL>(phSize),
            static_cast<Gdiplus::REAL>(phSize));

        g.DrawString(phLetter.c_str(), static_cast<INT>(phLetter.size()),
            &font, rectF, &sf, &letterBrush);

        dy += phSize + S(hWnd, 8);
    }
}
