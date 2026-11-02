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

#include <cmath>
#include <map>
#include <set>
#include <string>

namespace {
/**
 * GDI's grayscale anti-aliasing gives the plain coverage of a glyph's edge, and a texture blended over a panel then shows thin, pale strokes next to the
 * browser's (DirectWrite / FreeType text is gamma-corrected for light text on a dark ground). The coverage goes through a curve (exponent 0.8) that darkens
 * the edge pixels a little: 0 and 255 stay, 64 becomes 84, 128 becomes 147.
 */
const uint8_t *coverageCurve() {
    static uint8_t table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {
            table[i] = static_cast<uint8_t>(std::lround(255.0 * std::pow(i / 255.0, 0.8)));
        }
        ready = true;
    }
    return table;
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
    _hpen = CreatePen(PS_SOLID, static_cast<int>(_lineWidth), RGB(255, 255, 255));
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
    MoveToEx(dc, static_cast<int>(x), static_cast<int>(-(y - _bufferHeight - _fontSize)), nullptr);
}

void CanvasRenderingContext2DDelegate::lineTo(float x, float y) {
    HDC dc = sharedDC();
    if (!dc || !_bmp) return;
    SelectObject(dc, _bmp);
    SelectObject(dc, _hpen);
    LineTo(dc, static_cast<int>(x), static_cast<int>(-(y - _bufferHeight - _fontSize)));
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

    fillTextureData();
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

    drawText(text, (int)offsetPoint[0], (int)offsetPoint[1]);
    fillTextureData();
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
            tFont.lfHeight = -_fontSize;
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
        _bmp = CreateBitmap(static_cast<int>(_bufferWidth), static_cast<int>(_bufferHeight), 1, 32, nullptr);
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

        DWORD dwFmt = DT_SINGLELINE | DT_NOPREFIX;

        int bufferLen = 0;
        pwszBuffer = utf8ToUtf16(text, &bufferLen);

        Size newSize = sizeWithText(pwszBuffer, bufferLen);

        _textSize = newSize;

        RECT rcText = {0};

        rcText.right = static_cast<int>(newSize[0]);
        rcText.bottom = static_cast<int>(newSize[1]);

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

        tRet[0] = static_cast<float>(rc.right);
        tRet[1] = static_cast<float>(rc.bottom);
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

void CanvasRenderingContext2DDelegate::fillTextureData() {
    do {
        HDC dc = sharedDC();
        // no bitmap: nothing was drawn (GDI refused it), the buffer stays transparent
        CC_BREAK_IF(!dc || !_bmp);
        SelectObject(dc, _bmp);

        auto dataLen = static_cast<int>(_bufferWidth * _bufferHeight * 4);
        auto *dataBuf = static_cast<unsigned char *>(malloc(sizeof(unsigned char) * dataLen));
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
        GetDIBits(dc, _bmp, 0, static_cast<UINT>(_bufferHeight), dataBuf,
                  (LPBITMAPINFO)&bi, DIB_RGB_COLORS);

        uint8_t r = static_cast<uint8_t>(round(_fillStyle[0] * 255));
        uint8_t g = static_cast<uint8_t>(round(_fillStyle[1] * 255));
        uint8_t b = static_cast<uint8_t>(round(_fillStyle[2] * 255));
        COLORREF textColor = (b << 16 | g << 8 | r) & 0x00ffffff;
        COLORREF *pPixel = nullptr;
        COLORREF *pImage = nullptr;
        int bufferHeight = static_cast<int>(_bufferHeight);
        int bufferWidth = static_cast<int>(_bufferWidth);
        const uint8_t *curve = coverageCurve();
        for (int y = 0; y < bufferHeight; ++y) {
            pPixel = (COLORREF *)dataBuf + y * bufferWidth;
            pImage = (COLORREF *)imageBuf + y * bufferWidth;
            for (int x = 0; x < bufferWidth; ++x) {
                COLORREF &clr = *pPixel;
                COLORREF &val = *pImage;
                // Because text is drawn in white color, and background color is black,
                // so the red value is equal to alpha value. And we should keep this value
                // as it includes anti-atlas information.
                uint8_t alpha = curve[GetRValue(clr)];
                if (alpha > 0) {
                    val = (alpha << 24) | textColor;
                }
                ++pPixel;
                ++pImage;
            }
        }
        free(dataBuf);
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
        point[1] += -_tm.tmInternalLeading;
    } else if (_textBaseLine == TextBaseline::MIDDLE) {
        point[1] += -textSize[1] / 2.0f;
    } else if (_textBaseLine == TextBaseline::BOTTOM) {
        point[1] += -textSize[1];
    } else if (_textBaseLine == TextBaseline::ALPHABETIC) {
        GetTextMetrics(useFont(), &_tm);
        point[1] -= _tm.tmAscent;
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

void CanvasRenderingContext2DDelegate::strokeText(const ccstd::string & /* text */,
                                                  float /* x */,
                                                  float /* y */,
                                                  float /* maxWidth */) {
}

void CanvasRenderingContext2DDelegate::rect(float /* x */,
                                            float /* y */,
                                            float /* w */,
                                            float /* h */) {
}

} // namespace cc
