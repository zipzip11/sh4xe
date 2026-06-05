#include "render/console_font.h"

#include <windows.h>

namespace sh4xe::render::console_font
{

bool Build(Atlas& out, const wchar_t* faceName, int pixelHeight)
{
    if (pixelHeight < 6)
        pixelHeight = 6;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc)
        return false;

    LOGFONTW lf = {};
    lf.lfHeight = -pixelHeight; // negative => character (em) height in pixels
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY; // grayscale AA, no ClearType colour fringing
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpynW(lf.lfFaceName, faceName, LF_FACESIZE);

    HFONT font = CreateFontIndirectW(&lf);
    if (!font)
    {
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ oldFont = SelectObject(dc, font);

    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);

    // Consolas is monospace, so a single advance covers every glyph. Pad the
    // cell slightly so anti-aliased edges are not clipped by neighbours.
    int advance = tm.tmAveCharWidth;
    if (advance <= 0)
        advance = pixelHeight / 2 + 1;

    const int cellW = advance + 2;
    const int cellH = tm.tmHeight + 2;
    const int columns = 16;
    const int glyphCount = out.lastChar - out.firstChar + 1;
    const int rows = (glyphCount + columns - 1) / columns;

    const int width = columns * cellW;
    const int height = rows * cellH;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib || !dibBits)
    {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(dc, dib);

    // Black background, white anti-aliased glyphs; we read coverage from a colour
    // channel and store it in the texture's alpha.
    RECT fill = {0, 0, width, height};
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(dc, &fill, black);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    for (int i = 0; i < glyphCount; ++i)
    {
        const wchar_t ch = static_cast<wchar_t>(out.firstChar + i);
        const int col = i % columns;
        const int row = i / columns;
        TextOutW(dc, col * cellW + 1, row * cellH + 1, &ch, 1);
    }

    GdiFlush();

    out.width = width;
    out.height = height;
    out.columns = columns;
    out.cellW = cellW;
    out.cellH = cellH;
    out.advance = advance;
    out.lineHeight = tm.tmHeight;
    out.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

    const uint32_t* src = static_cast<const uint32_t*>(dibBits);
    for (int i = 0; i < width * height; ++i)
    {
        // White-on-black render: any channel equals coverage. Keep RGB white and
        // move coverage into alpha for tintable, blendable text.
        const uint32_t coverage = src[i] & 0xFF;
        out.pixels[static_cast<size_t>(i)] = (coverage << 24) | 0x00FFFFFF;
    }

    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(dib);
    DeleteObject(font);
    DeleteDC(dc);

    return out.Valid();
}

} // namespace sh4xe::render::console_font
