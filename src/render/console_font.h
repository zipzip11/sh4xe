#pragma once

#include <cstdint>
#include <vector>

namespace sh4xe::render::console_font
{

// A CPU-side ARGB glyph atlas rasterized from a system TrueType font (Consolas)
// via GDI. RGB is forced white and the glyph coverage is stored in alpha, so the
// texture can be tinted per-vertex and alpha-blended for anti-aliased text.
struct Atlas
{
    int width = 0;       // atlas texture width in texels
    int height = 0;      // atlas texture height in texels
    int columns = 0;     // glyph cells per row
    int cellW = 0;       // atlas cell width
    int cellH = 0;       // atlas cell height
    int advance = 0;     // horizontal advance per glyph (monospace)
    int lineHeight = 0;  // baseline-to-baseline spacing
    int firstChar = 32;  // first printable ASCII glyph
    int lastChar = 126;  // last printable ASCII glyph
    std::vector<uint32_t> pixels; // width*height, 0xAARRGGBB

    bool Valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// Rasterize the named monospace font at the given pixel height. Returns false if
// GDI font creation or the DIB render fails; callers should fall back to the
// built-in stroke font in that case.
bool Build(Atlas& out, const wchar_t* faceName, int pixelHeight);

} // namespace sh4xe::render::console_font
