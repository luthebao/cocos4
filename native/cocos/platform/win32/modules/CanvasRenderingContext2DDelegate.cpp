/****************************************************************************
 Copyright (c) 2018-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

// The engine's delegate kept a device context, a bitmap and a font per label (3 GDI objects) until V8 finalized the label; a process may hold 10 000 GDI
// objects, so about 3 300 live labels made CreateBitmap fail and every NEW text blank. Here the device context and the fonts are shared by the process and a
// label's bitmap lives only from its first draw to fillTextureData: a label holds no GDI object between two draws. All text is drawn on the JS thread.
#include "platform/win32/modules/CanvasRenderingContext2DDelegate.h"
#include "base/memory/Memory.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <string>

namespace {
/**
 * GDI's grayscale anti-aliasing gives the plain coverage of a glyph's edge, and a texture blended over a panel then shows thin, pale strokes next to the
 * browser's (DirectWrite / FreeType text is gamma-corrected for light text on a dark ground). The coverage goes through a curve (exponent 0.7) that darkens
 * the edge pixels a little: 0 and 255 stay, 64 becomes 94, 128 becomes 157. (A try at widening the stems by a fraction of a pixel made the letters of a small
 * label touch.)
 */
const uint8_t *coverageCurve() {
    static uint8_t table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {
            table[i] = static_cast<uint8_t>(std::lround(255.0 * std::pow(i / 255.0, 0.7)));
        }
        ready = true;
    }
    return table;
}

/**
 * The glyphs are drawn TEXT_SCALE times bigger than the label (the bitmap, the font height, the positions and the sizes in the delegate are in these units) and
 * the coverage is averaged down to the label's pixels in fillTextureData. GDI's own rasteriser at the label's size bends every glyph to the pixel grid
 * (hinting: a 12 px Tahoma "o" comes out square, its advances rounded), which does not look like Tahoma next to Flash or macOS; at four times the size the
 * hinting moves nothing visible and the shapes are the font's own (owner, 2026-11-03: "the font does not look like Tahoma", "the letters are round, not bent").
 * The text's measure is the same draw's, divided by TEXT_SCALE (fractional advances, as Flash and the browser have).
 */
constexpr int TEXT_SCALE = 4;

/** the averaged coverage that reads as 0 and as 255 (fillTextureData): a stem of 1.1 pixels is solid, the faint fringe of a curve is not drawn */
constexpr int COVER_LOW = 30;
constexpr int COVER_HIGH = 225;

/** a font of this height in pixels or less gets half a pixel more between its letters (useFont) */
constexpr int SMALL_TEXT_PX = 14;

/** the smallest outline radius in pixels: 1.5 gives the ring's first pixel the full weight (see Pass::STROKE in fillTextureData) */
constexpr float MIN_OUTLINE_RADIUS = 1.5F;

/** source-over of a colour with coverage `sa` (0 to 255) onto an RGBA pixel of the label's buffer (straight alpha, as the buffer holds it) */
void blendPixel(uint8_t *px, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa) {
    if (sa == 0) {
        return;
    }
    const uint32_t keep = static_cast<uint32_t>(px[3]) * (255 - sa); // the destination's share, scaled by 255
    const uint32_t denom = sa * 255 + keep;
    px[0] = static_cast<uint8_t>((sr * sa * 255 + px[0] * keep + denom / 2) / denom);
    px[1] = static_cast<uint8_t>((sg * sa * 255 + px[1] * keep + denom / 2) / denom);
    px[2] = static_cast<uint8_t>((sb * sa * 255 + px[2] * keep + denom / 2) / denom);
    px[3] = static_cast<uint8_t>((denom + 127) / 255);
}

/** a Gaussian blur of a w x h coverage (0 to 1) with the standard deviation `sigma`, in place */
void blurCoverage(std::vector<float> &m, int w, int h, float sigma) {
    const int radius = (std::min)(32, static_cast<int>(std::ceil(sigma * 3.0F)));
    if (radius < 1) {
        return;
    }
    std::vector<float> kernel(radius * 2 + 1);
    float sum = 0.0F;
    for (int i = -radius; i <= radius; ++i) {
        kernel[i + radius] = std::exp(-(i * i) / (2.0F * sigma * sigma));
        sum += kernel[i + radius];
    }
    for (auto &k : kernel) {
        k /= sum;
    }
    std::vector<float> tmp(m.size(), 0.0F);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v = 0.0F;
            for (int i = -radius; i <= radius; ++i) {
                const int nx = x + i;
                if (nx >= 0 && nx < w) {
                    v += m[y * w + nx] * kernel[i + radius];
                }
            }
            tmp[y * w + x] = v;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v = 0.0F;
            for (int i = -radius; i <= radius; ++i) {
                const int ny = y + i;
                if (ny >= 0 && ny < h) {
                    v += tmp[ny * w + x] * kernel[i + radius];
                }
            }
            m[y * w + x] = v;
        }
    }
}

void fillRectWithColor(uint8_t *buf, uint32_t totalWidth, uint32_t totalHeight, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    CC_ASSERT(x + width <= totalWidth);
    CC_ASSERT(y + height <= totalHeight);

    uint32_t y0 = y;
    uint32_t y1 = y + height;
    uint8_t *p;
    for (uint32_t offsetY = y0; offsetY < y1; ++offsetY) {
        for (uint32_t offsetX = x; offsetX < (x + width); ++offsetX) {
            p = buf + (totalWidth * offsetY + offsetX) * 4;
            *p++ = r;
            *p++ = g;
            *p++ = b;
            *p++ = a;
        }
    }
}

/** the one device context every label draws with, and the bitmap it holds while no label's is selected */
struct SharedDC {
    HDC dc{nullptr};
    HBITMAP idleBitmap{nullptr};
    SharedDC() {
        HDC screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (dc) {
            idleBitmap = static_cast<HBITMAP>(GetCurrentObject(dc, OBJ_BITMAP));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
        }
    }
};

SharedDC &shared() {
    static SharedDC instance;
    return instance;
}

HDC sharedDC() {
    return shared().dc;
}

HBITMAP sharedIdleBitmap() {
    return shared().idleBitmap;
}

/** the fonts made so far, by face / height / weight / italic: a few dozen at most, kept for the life of the process */
std::map<std::string, HFONT> &fontCache() {
    static std::map<std::string, HFONT> cache;
    return cache;
}

/** AddFontResource once per font file, RemoveFontResource when the process ends */
struct FontFiles {
    std::set<std::wstring> added;
    ~FontFiles() {
        for (const auto &path : added) RemoveFontResourceW(path.c_str());
    }
};

FontFiles &fontFiles() {
    static FontFiles files;
    return files;
}

HFONT cachedFont(const LOGFONTA &logFont) {
    char key[LF_FACESIZE + 64];
    sprintf_s(key, "%s|%ld|%ld|%d|%d", logFont.lfFaceName, static_cast<long>(logFont.lfHeight), static_cast<long>(logFont.lfWeight), static_cast<int>(logFont.lfItalic), static_cast<int>(logFont.lfCharSet));
    auto &cache = fontCache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HFONT font = CreateFontIndirectA(&logFont);
    if (font) cache.emplace(key, font);
    return font;
}
} // namespace

namespace cc {
CanvasRenderingContext2DDelegate::CanvasRenderingContext2DDelegate() {
}

CanvasRenderingContext2DDelegate::~CanvasRenderingContext2DDelegate() {
    deleteBitmap();
}

void CanvasRenderingContext2DDelegate::recreateBuffer(float w, float h) {
    _bufferWidth = w;
    _bufferHeight = h;
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        deleteBitmap();
        return;
    }

    auto textureSize = static_cast<int>(_bufferWidth * _bufferHeight * 4);
    auto *data = static_cast<uint8_t *>(malloc(sizeof(uint8_t) * textureSize));
    memset(data, 0x00, textureSize);
    _imageData.fastSet(data, textureSize);

    prepareBitmap(static_cast<int>(_bufferWidth), static_cast<int>(_bufferHeight));
}

void CanvasRenderingContext2DDelegate::beginPath() {
    // called: set_lineWidth() -> beginPath() -> moveTo() -> lineTo() -> stroke(), when draw line
    HDC dc = useFont();
    if (!dc || !useBitmap()) {
        return;
    }
    if (_hpen) {
        SelectObject(dc, GetStockObject(BLACK_PEN));
        DeleteObject(_hpen);
    }
    _hpen = CreatePen(PS_SOLID, static_cast<int>(_lineWidth * TEXT_SCALE), RGB(255, 255, 255));
    SelectObject(dc, _hpen);

    SetBkMode(dc, TRANSPARENT);
}

void CanvasRenderingContext2DDelegate::closePath() {
}

void CanvasRenderingContext2DDelegate::moveTo(float x, float y) {
    HDC dc = sharedDC();
    if (!dc || !_bmp) return;
    SelectObject(dc, _bmp);
    SelectObject(dc, _hpen);
    MoveToEx(dc, static_cast<int>(x * TEXT_SCALE), static_cast<int>(-(y - _bufferHeight - _fontSize) * TEXT_SCALE), nullptr);
}

void CanvasRenderingContext2DDelegate::lineTo(float x, float y) {
    HDC dc = sharedDC();
    if (!dc || !_bmp) return;
    SelectObject(dc, _bmp);
    SelectObject(dc, _hpen);
    LineTo(dc, static_cast<int>(x * TEXT_SCALE), static_cast<int>(-(y - _bufferHeight - _fontSize) * TEXT_SCALE));
}

void CanvasRenderingContext2DDelegate::stroke() {
    if (_hpen) {
        // a pen still selected cannot be deleted
        if (HDC dc = sharedDC()) SelectObject(dc, GetStockObject(BLACK_PEN));
        DeleteObject(_hpen);
        _hpen = nullptr;
    }
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    fillTextureData(Pass::LINE);
}

void CanvasRenderingContext2DDelegate::saveContext() {
    // the shared device context is bound again (font, bitmap) before every use: nothing to save
}

void CanvasRenderingContext2DDelegate::restoreContext() {
}

void CanvasRenderingContext2DDelegate::clearRect(float /*x*/, float /*y*/, float w, float h) {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    if (_imageData.isNull()) {
        return;
    }

    recreateBuffer(w, h);
}

void CanvasRenderingContext2DDelegate::fillRect(float x, float y, float w, float h) {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    //not filled all Bits in buffer? the buffer length is _bufferWidth * _bufferHeight * 4, but it filled _bufferWidth * _bufferHeight * 3?
    uint8_t *buffer = _imageData.getBytes();
    if (buffer) {
        uint8_t r = static_cast<uint8_t>(_fillStyle[0] * 255.0f);
        uint8_t g = static_cast<uint8_t>(_fillStyle[1] * 255.0f);
        uint8_t b = static_cast<uint8_t>(_fillStyle[2] * 255.0f);
        uint8_t a = static_cast<uint8_t>(_fillStyle[3] * 255.0f);
        fillRectWithColor(buffer,
                          static_cast<uint32_t>(_bufferWidth),
                          static_cast<uint32_t>(_bufferHeight),
                          static_cast<uint32_t>(x),
                          static_cast<uint32_t>(y),
                          static_cast<uint32_t>(w),
                          static_cast<uint32_t>(h), r, g, b, a);
    }
}

void CanvasRenderingContext2DDelegate::fillText(const ccstd::string &text, float x, float y, float /*maxWidth*/) {
    if (text.empty() || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    Point offsetPoint = convertDrawPoint(Point{x, y}, text);

    drawText(text, static_cast<int>(std::lround(offsetPoint[0] * TEXT_SCALE)), static_cast<int>(std::lround(offsetPoint[1] * TEXT_SCALE)));
    fillTextureData(Pass::FILL);
}

void CanvasRenderingContext2DDelegate::strokeText(const ccstd::string &text, float /*x*/, float /*y*/, float /*maxWidth*/) const {
    if (text.empty() || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }
}

CanvasRenderingContext2DDelegate::Size CanvasRenderingContext2DDelegate::measureText(const ccstd::string &text) {
    if (text.empty())
        return ccstd::array<float, 2>{0.0f, 0.0f};

    int bufferLen = 0;
    wchar_t *pwszBuffer = CanvasRenderingContext2DDelegate::utf8ToUtf16(text, &bufferLen);
    Size size = sizeWithText(pwszBuffer, bufferLen);
    //SE_LOGD("CanvasRenderingContext2DImpl::measureText: %s, %d, %d\n", text.c_str(), size.cx, size.cy);
    CC_SAFE_DELETE_ARRAY(pwszBuffer);
    return size;
}

void CanvasRenderingContext2DDelegate::updateFont(const ccstd::string &fontName,
                                                  float fontSize,
                                                  bool bold,
                                                  bool italic,
                                                  bool /* oblique */,
                                                  bool /* smallCaps */) {
    do {
        _fontName = fontName;
        _fontSize = static_cast<int>(fontSize);
        ccstd::string fontPath;
        LOGFONTA tFont = {0};
        if (!_fontName.empty()) {
            // firstly, try to create font from ttf file
            const auto &fontInfoMap = getFontFamilyNameMap();
            auto iter = fontInfoMap.find(_fontName);
            if (iter != fontInfoMap.end()) {
                fontPath = iter->second;
                ccstd::string tmpFontPath = fontPath;
                size_t nFindPos = tmpFontPath.rfind("/");
                tmpFontPath = &tmpFontPath[nFindPos + 1];
                nFindPos = tmpFontPath.rfind(".");
                // IDEA: draw ttf failed if font file name not equal font face name
                // for example: "DejaVuSansMono-Oblique" not equal "DejaVu Sans Mono"  when using DejaVuSansMono-Oblique.ttf
                _fontName = tmpFontPath.substr(0, nFindPos);
            } else {
                auto nFindPos = fontName.rfind("/");
                if (nFindPos != fontName.npos) {
                    if (fontName.length() == nFindPos + 1) {
                        _fontName = "";
                    } else {
                        _fontName = &_fontName[nFindPos + 1];
                    }
                }
            }
            tFont.lfCharSet = DEFAULT_CHARSET;
            strcpy_s(tFont.lfFaceName, LF_FACESIZE, _fontName.c_str());
        }

        if (_fontSize) {
            tFont.lfHeight = -static_cast<LONG>(std::lround(fontSize * TEXT_SCALE));
        }

        if (bold) {
            tFont.lfWeight = FW_BOLD;
        } else {
            tFont.lfWeight = FW_NORMAL;
        }

        tFont.lfItalic = italic;

        // disable Cleartype
        tFont.lfQuality = ANTIALIASED_QUALITY;

        if (!fontPath.empty()) {
            wchar_t *pwszBuffer = utf8ToUtf16(fontPath);
            if (pwszBuffer) {
                // once per font file for the whole process (a font file of the game is registered again by every label otherwise)
                if (fontFiles().added.insert(pwszBuffer).second) {
                    AddFontResourceW(pwszBuffer);
                }
                delete[] pwszBuffer;
                pwszBuffer = nullptr;
            }
        }

        // a font of the shared cache (made once per face / size / weight / italic)
        _font = cachedFont(tFont);
        if (!_font) {
            // create failed, use default font
            SE_LOGE("Failed to create custom font(font name: %s, font size: %f), use default font.\n",
                    _fontName.c_str(), fontSize);
            _font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
    } while (false);
}

void CanvasRenderingContext2DDelegate::setTextAlign(TextAlign align) {
    _textAlign = align;
}

void CanvasRenderingContext2DDelegate::setTextBaseline(TextBaseline baseline) {
    _textBaseLine = baseline;
}

void CanvasRenderingContext2DDelegate::setFillStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    _fillStyle = {r / 255.0F, g / 255.0F, b / 255.0F, a / 255.0F};
}

void CanvasRenderingContext2DDelegate::setStrokeStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    _strokeStyle = {r / 255.0F, g / 255.0F, b / 255.0F, a / 255.0F};
}

void CanvasRenderingContext2DDelegate::setLineWidth(float lineWidth) {
    _lineWidth = lineWidth;
}

const cc::Data &CanvasRenderingContext2DDelegate::getDataRef() const {
    return _imageData;
}

// change utf-8 string to utf-16, pRetLen is the string length after changing
wchar_t *CanvasRenderingContext2DDelegate::utf8ToUtf16(const ccstd::string &str, int *pRetLen /* = nullptr*/) {
    wchar_t *pwszBuffer = nullptr;
    do {
        if (str.empty()) {
            break;
        }
        int nLen = static_cast<int>(str.size());
        int nBufLen = nLen + 1;
        pwszBuffer = ccnew wchar_t[nBufLen];
        CC_BREAK_IF(!pwszBuffer);
        memset(pwszBuffer, 0, sizeof(wchar_t) * nBufLen);
        // str.size() not equal actuallyLen for Chinese char
        int actuallyLen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), nLen, pwszBuffer, nBufLen);
        // SE_LOGE("_utf8ToUtf16, str:%s, strLen:%d, retLen:%d\n", str.c_str(), str.size(), actuallyLen);
        if (pRetLen != nullptr) {
            *pRetLen = actuallyLen;
        }
    } while (false);
    return pwszBuffer;
}

// the shared device context with this label's font selected (nullptr when the context could not be made)
HDC CanvasRenderingContext2DDelegate::useFont() {
    HDC dc = sharedDC();
    if (dc && _font) {
        SelectObject(dc, _font);
        // Text of a window of the stage's own size (12 px glyphs) has an outline a pixel wide (Pass::STROKE), which fills the gap between two letters: half a
        // pixel more between them, in the measuring too (every size goes through here). A bigger window draws bigger glyphs, which keep their own gaps (owner,
        // 2026-11-03: "the letters are too close, more space").
        SetTextCharacterExtra(dc, (_fontSize > 0 && _fontSize <= SMALL_TEXT_PX) ? TEXT_SCALE / 2 : 0);
    }
    return dc;
}

// this label's bitmap, made at its first draw and selected into the shared device context (false when GDI refuses it)
bool CanvasRenderingContext2DDelegate::useBitmap() {
    HDC dc = sharedDC();
    if (!dc || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return false;
    }
    if (!_bmp) {
        _bmp = CreateBitmap(static_cast<int>(_bufferWidth) * TEXT_SCALE, static_cast<int>(_bufferHeight) * TEXT_SCALE, 1, 32, nullptr);
        if (!_bmp) {
            return false;
        }
    }
    SelectObject(dc, _bmp);
    return true;
}

// x, y offset value
int CanvasRenderingContext2DDelegate::drawText(const ccstd::string &text, int x, int y) {
    int nRet = 0;
    wchar_t *pwszBuffer = nullptr;
    do {
        CC_BREAK_IF(text.empty());

        HDC dc = useFont();
        CC_BREAK_IF(!dc);
        CC_BREAK_IF(!useBitmap());

        // DT_NOCLIP: the box below is the measured size, and a glyph that overhangs it (italics, the space after the last letter) must still be drawn
        DWORD dwFmt = DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP;

        int bufferLen = 0;
        pwszBuffer = utf8ToUtf16(text, &bufferLen);

        Size newSize = sizeWithText(pwszBuffer, bufferLen);

        _textSize = newSize;

        RECT rcText = {0};

        // the bitmap's units (TEXT_SCALE times the label's pixels)
        rcText.right = static_cast<int>(std::ceil(newSize[0] * TEXT_SCALE));
        rcText.bottom = static_cast<int>(std::ceil(newSize[1] * TEXT_SCALE));

        LONG offsetX = x;
        LONG offsetY = y;
        if (offsetX || offsetY) {
            OffsetRect(&rcText, offsetX, offsetY);
        }

        // SE_LOGE("_drawText text,%s size: (%d, %d) offset after convert: (%d, %d) \n", text.c_str(), newSize.cx, newSize.cy, offsetX, offsetY);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255)); // white color

        // draw text
        nRet = DrawTextW(dc, pwszBuffer, bufferLen, &rcText, dwFmt);
    } while (false);
    CC_SAFE_DELETE_ARRAY(pwszBuffer);

    return nRet;
}

CanvasRenderingContext2DDelegate::Size CanvasRenderingContext2DDelegate::sizeWithText(const wchar_t *pszText, int nLen) {
    Size tRet{0, 0};
    do {
        CC_BREAK_IF(!pszText || nLen <= 0);

        HDC dc = useFont();
        CC_BREAK_IF(!dc);

        RECT rc = {0, 0, 0, 0};
        DWORD dwCalcFmt = DT_CALCRECT | DT_NOPREFIX;

        // measure text size
        DrawTextW(dc, pszText, nLen, &rc, dwCalcFmt);
        // DrawText's calculated width leaves out the extra space between the letters (useFont): it is added here, one per character. Not
        // GetTextExtentPoint32W: it has no font fallback, so a letter the font lacks (the styled math letters U+1D400.. of a player name, an emoji) was
        // measured as two missing-glyph boxes, the text came out far too wide and a centred label was drawn to the left of its place
        const int extra = GetTextCharacterExtra(dc);
        if (extra > 0) {
            int chars = 0;
            for (int i = 0; i < nLen; ++i) {
                if (pszText[i] < 0xDC00 || pszText[i] > 0xDFFF) ++chars; // a surrogate pair is one character
            }
            rc.right += extra * chars;
        }

        // the label's pixels: the font is TEXT_SCALE times its size
        tRet[0] = static_cast<float>(rc.right) / TEXT_SCALE;
        tRet[1] = static_cast<float>(rc.bottom) / TEXT_SCALE;
    } while (false);

    return tRet;
}

void CanvasRenderingContext2DDelegate::prepareBitmap(int /*nWidth*/, int /*nHeight*/) {
    // a new buffer: the old bitmap (if a draw left one) is of the wrong size; the next draw makes its own
    deleteBitmap();
}

void CanvasRenderingContext2DDelegate::deleteBitmap() {
    if (_bmp) {
        // a bitmap selected into a device context cannot be deleted: give the context its idle one back first
        HDC dc = sharedDC();
        if (dc && GetCurrentObject(dc, OBJ_BITMAP) == _bmp) {
            SelectObject(dc, sharedIdleBitmap());
        }
        DeleteObject(_bmp);
        _bmp = nullptr;
    }
}

// The bitmap holds the glyphs drawn white on black: its red channel is their coverage. It is painted into the label's RGBA buffer as `pass` says:
// - FILL: the text in the fill colour, over a shadow of it when the label has one (LabelShadow: the coverage moved by the offset, blurred, in the shadow colour);
// - STROKE: the coverage grown by half the line width, in the stroke colour (the engine fills the text over it afterwards);
// - LINE: a line of `stroke()` (the underline), as before: the pixels it covers are replaced.
void CanvasRenderingContext2DDelegate::fillTextureData(Pass pass) {
    do {
        HDC dc = sharedDC();
        // no bitmap: nothing was drawn (GDI refused it), the buffer stays transparent
        CC_BREAK_IF(!dc || !_bmp);
        SelectObject(dc, _bmp);

        // the bitmap is TEXT_SCALE times the label's size in both directions
        const int bitmapWidth = static_cast<int>(_bufferWidth) * TEXT_SCALE;
        const int bitmapHeight = static_cast<int>(_bufferHeight) * TEXT_SCALE;
        const size_t dataLen = static_cast<size_t>(bitmapWidth) * bitmapHeight * 4;
        auto *dataBuf = static_cast<unsigned char *>(malloc(dataLen));
        CC_BREAK_IF(!dataBuf);
        unsigned char *imageBuf = _imageData.getBytes();
        if (!imageBuf) {
            free(dataBuf);
            break;
        }

        struct
        {
            BITMAPINFOHEADER bmiHeader;
            int mask[4];
        } bi = {0};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        if (!GetDIBits(dc, _bmp, 0, 0,
                       nullptr, (LPBITMAPINFO)&bi, DIB_RGB_COLORS)) {
            free(dataBuf);
            break;
        }

        // copy pixel data
        bi.bmiHeader.biHeight = (bi.bmiHeader.biHeight > 0) ? -bi.bmiHeader.biHeight : bi.bmiHeader.biHeight;
        GetDIBits(dc, _bmp, 0, static_cast<UINT>(bitmapHeight), dataBuf,
                  (LPBITMAPINFO)&bi, DIB_RGB_COLORS);

        const int bufferHeight = static_cast<int>(_bufferHeight);
        const int bufferWidth = static_cast<int>(_bufferWidth);
        // Because text is drawn in white color, and background color is black,
        // so the red value is equal to alpha value. And we should keep this value
        // as it includes anti-atlas information.
        // The coverage of a label's pixel is the mean of the TEXT_SCALE x TEXT_SCALE bitmap pixels it covers, with the levels COVER_LOW to COVER_HIGH
        // stretched to 0 to 255: a stem a pixel wide and a bit more is solid, and the faint fringe of the averaging is not drawn.
        std::vector<uint8_t> cover(static_cast<size_t>(bufferWidth) * bufferHeight);
        const auto *bits = reinterpret_cast<const COLORREF *>(dataBuf);
        for (int y = 0; y < bufferHeight; ++y) {
            for (int x = 0; x < bufferWidth; ++x) {
                int sum = 0;
                for (int dy = 0; dy < TEXT_SCALE; ++dy) {
                    const COLORREF *row = bits + static_cast<size_t>(y * TEXT_SCALE + dy) * bitmapWidth + x * TEXT_SCALE;
                    for (int dx = 0; dx < TEXT_SCALE; ++dx) {
                        sum += GetRValue(row[dx]);
                    }
                }
                const int mean = sum / (TEXT_SCALE * TEXT_SCALE);
                cover[static_cast<size_t>(y) * bufferWidth + x] = static_cast<uint8_t>(
                    std::lround(255.0 * (std::min)(1.0, (std::max)(0.0, (mean - COVER_LOW) / static_cast<double>(COVER_HIGH - COVER_LOW)))));
            }
        }
        free(dataBuf);

        const uint8_t *curve = coverageCurve();
        auto channel = [](float v) { return static_cast<uint32_t>(std::lround(v * 255.0F)); };

        if (pass == Pass::LINE) {
            const COLORREF color = (channel(_fillStyle[2]) << 16 | channel(_fillStyle[1]) << 8 | channel(_fillStyle[0])) & 0x00ffffff;
            auto *image = reinterpret_cast<COLORREF *>(imageBuf);
            for (size_t i = 0; i < cover.size(); ++i) {
                const uint8_t alpha = curve[cover[i]];
                if (alpha > 0) {
                    image[i] = (static_cast<COLORREF>(alpha) << 24) | color;
                }
            }
        } else if (pass == Pass::STROKE) {
            // An edge thinner than one pixel is one full pixel: the game's hairline outline (0.75 of a stage pixel) is 1.5 device pixels in a window twice the stage
            // (a solid ring) but 0.75 in a window of the stage's own size, where the ring below covers a quarter of a pixel and the text reads thin and pale next
            // to Flash's dark edge (owner, 2026-11-03: the text is thinner when the window is shrunk to 1050).
            const float radius = _lineWidth > 0.0F ? (std::max)(_lineWidth * 0.5F, MIN_OUTLINE_RADIUS) : 0.0F;
            if (radius > 0.0F) {
                const int reach = static_cast<int>(std::ceil(radius + 0.5F));
                // the box of the glyphs: nothing outside it grown by `reach` is touched
                int minX = bufferWidth;
                int minY = bufferHeight;
                int maxX = -1;
                int maxY = -1;
                for (int y = 0; y < bufferHeight; ++y) {
                    for (int x = 0; x < bufferWidth; ++x) {
                        if (cover[static_cast<size_t>(y) * bufferWidth + x] > 0) {
                            minX = (std::min)(minX, x);
                            maxX = (std::max)(maxX, x);
                            minY = (std::min)(minY, y);
                            maxY = (std::max)(maxY, y);
                        }
                    }
                }
                // the weight of a neighbour at a distance: 1 inside the radius, falling to 0 over one pixel (the edge is anti-aliased)
                std::vector<float> weight(static_cast<size_t>(2 * reach + 1) * (2 * reach + 1));
                for (int dy = -reach; dy <= reach; ++dy) {
                    for (int dx = -reach; dx <= reach; ++dx) {
                        const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                        weight[static_cast<size_t>(dy + reach) * (2 * reach + 1) + (dx + reach)] = (std::min)(1.0F, (std::max)(0.0F, radius + 0.5F - d));
                    }
                }
                const uint32_t sr = channel(_strokeStyle[0]);
                const uint32_t sg = channel(_strokeStyle[1]);
                const uint32_t sb = channel(_strokeStyle[2]);
                for (int y = (std::max)(0, minY - reach); y <= (std::min)(bufferHeight - 1, maxY + reach); ++y) {
                    for (int x = (std::max)(0, minX - reach); x <= (std::min)(bufferWidth - 1, maxX + reach); ++x) {
                        float best = 0.0F;
                        for (int dy = -reach; dy <= reach; ++dy) {
                            const int ny = y + dy;
                            if (ny < 0 || ny >= bufferHeight) {
                                continue;
                            }
                            for (int dx = -reach; dx <= reach; ++dx) {
                                const int nx = x + dx;
                                if (nx < 0 || nx >= bufferWidth) {
                                    continue;
                                }
                                const uint8_t c = cover[static_cast<size_t>(ny) * bufferWidth + nx];
                                if (c == 0) {
                                    continue;
                                }
                                const float v = (std::min)(c / 255.0F, weight[static_cast<size_t>(dy + reach) * (2 * reach + 1) + (dx + reach)]);
                                best = (std::max)(best, v);
                            }
                        }
                        const uint32_t alpha = channel(best * _strokeStyle[3]);
                        blendPixel(imageBuf + (static_cast<size_t>(y) * bufferWidth + x) * 4, sr, sg, sb, alpha);
                    }
                }
            }
        } else {
            // the shadow first: a canvas draws it under the text
            const int offsetX = static_cast<int>(std::lround(_shadowOffsetX));
            const int offsetY = static_cast<int>(std::lround(_shadowOffsetY));
            if (_shadowColor[3] > 0.0F && (_shadowBlur > 0.0F || offsetX != 0 || offsetY != 0)) {
                std::vector<float> shadow(cover.size(), 0.0F);
                for (int y = 0; y < bufferHeight; ++y) {
                    const int sy = y + offsetY;
                    if (sy < 0 || sy >= bufferHeight) {
                        continue;
                    }
                    for (int x = 0; x < bufferWidth; ++x) {
                        const int sx = x + offsetX;
                        if (sx >= 0 && sx < bufferWidth) {
                            shadow[static_cast<size_t>(sy) * bufferWidth + sx] = cover[static_cast<size_t>(y) * bufferWidth + x] / 255.0F;
                        }
                    }
                }
                if (_shadowBlur > 0.0F) {
                    blurCoverage(shadow, bufferWidth, bufferHeight, _shadowBlur * 0.5F); // a canvas: the blur is twice the standard deviation
                }
                const uint32_t sr = channel(_shadowColor[0]);
                const uint32_t sg = channel(_shadowColor[1]);
                const uint32_t sb = channel(_shadowColor[2]);
                for (size_t i = 0; i < shadow.size(); ++i) {
                    blendPixel(imageBuf + i * 4, sr, sg, sb, channel(shadow[i] * _shadowColor[3]));
                }
            }
            const uint32_t fr = channel(_fillStyle[0]);
            const uint32_t fg = channel(_fillStyle[1]);
            const uint32_t fb = channel(_fillStyle[2]);
            for (size_t i = 0; i < cover.size(); ++i) {
                blendPixel(imageBuf + i * 4, fr, fg, fb, channel(curve[cover[i]] / 255.0F * _fillStyle[3]));
            }
        }
    } while (false);
    // the pixels are in _imageData now: the bitmap is not needed until the next draw
    deleteBitmap();
}

ccstd::array<float, 2> CanvasRenderingContext2DDelegate::convertDrawPoint(Point point, const ccstd::string &text) {
    Size textSize = measureText(text);
    if (_textAlign == TextAlign::CENTER) {
        point[0] -= textSize[0] / 2.0f;
    } else if (_textAlign == TextAlign::RIGHT) {
        point[0] -= textSize[0];
    }

    if (_textBaseLine == TextBaseline::TOP) {
        // DrawText default
        GetTextMetrics(useFont(), &_tm);
        point[1] += -static_cast<float>(_tm.tmInternalLeading) / TEXT_SCALE; // the font's units are TEXT_SCALE times the label's
    } else if (_textBaseLine == TextBaseline::MIDDLE) {
        point[1] += -textSize[1] / 2.0f;
    } else if (_textBaseLine == TextBaseline::BOTTOM) {
        point[1] += -textSize[1];
    } else if (_textBaseLine == TextBaseline::ALPHABETIC) {
        GetTextMetrics(useFont(), &_tm);
        point[1] -= static_cast<float>(_tm.tmAscent) / TEXT_SCALE;
    }

    return point;
}

void CanvasRenderingContext2DDelegate::fill() {
}

void CanvasRenderingContext2DDelegate::setLineCap(const ccstd::string & /* lineCap */) {
}

void CanvasRenderingContext2DDelegate::setLineJoin(const ccstd::string & /* lineCap */) {
}

void CanvasRenderingContext2DDelegate::fillImageData(const Data & /* imageData */,
                                                     float /* imageWidth */,
                                                     float /* imageHeight */,
                                                     float /* offsetX */,
                                                     float /* offsetY */) {
}

// The outline of a label (LabelOutline): the engine strokes the text with the outline colour, then fills it over. It was empty here, so a Windows label had no
// outline while macOS and iOS drew one.
void CanvasRenderingContext2DDelegate::strokeText(const ccstd::string &text,
                                                  float x,
                                                  float y,
                                                  float /* maxWidth */) {
    if (text.empty() || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    Point offsetPoint = convertDrawPoint(Point{x, y}, text);

    drawText(text, static_cast<int>(std::lround(offsetPoint[0] * TEXT_SCALE)), static_cast<int>(std::lround(offsetPoint[1] * TEXT_SCALE)));
    fillTextureData(Pass::STROKE);
}

void CanvasRenderingContext2DDelegate::rect(float /* x */,
                                            float /* y */,
                                            float /* w */,
                                            float /* h */) {
}

} // namespace cc
