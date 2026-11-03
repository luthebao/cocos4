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

#pragma once

#include "platform/interfaces/modules/canvas/ICanvasRenderingContext2D.h"

#include <Windows.h>
#include <cstdint>
#include <regex>
#include "base/csscolorparser.h"
#include "base/std/container/array.h"
#include "cocos/bindings/jswrapper/SeApi.h"
#include "cocos/bindings/manual/jsb_platform.h"
#include "math/Math.h"
#include "platform/FileUtils.h"

namespace cc {

class CC_DLL CanvasRenderingContext2DDelegate : public ICanvasRenderingContext2D::Delegate {
public:
    using Point = ccstd::array<float, 2>;
    using Vec2 = ccstd::array<float, 2>;
    using Size = ccstd::array<float, 2>;
    using Color4F = ccstd::array<float, 4>;
    using TextAlign = ICanvasRenderingContext2D::TextAlign;
    using TextBaseline = ICanvasRenderingContext2D::TextBaseline;
    CanvasRenderingContext2DDelegate();
    ~CanvasRenderingContext2DDelegate() override;

    void recreateBuffer(float w, float h) override;
    void beginPath() override;
    void closePath() override;
    void moveTo(float x, float y) override;
    void lineTo(float x, float y) override;
    void stroke() override;
    void saveContext() override;
    void restoreContext() override;
    void clearRect(float /*x*/, float /*y*/, float w, float h) override;
    void fillRect(float x, float y, float w, float h) override;
    void fillText(const ccstd::string &text, float x, float y, float /*maxWidth*/) override;
    void strokeText(const ccstd::string &text, float /*x*/, float /*y*/, float /*maxWidth*/) const;
    Size measureText(const ccstd::string &text) override;
    void updateFont(const ccstd::string &fontName, float fontSize, bool bold, bool italic, bool oblique, bool smallCaps) override;
    void setTextAlign(TextAlign align) override;
    void setTextBaseline(TextBaseline baseline) override;
    void setFillStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
    void setStrokeStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
    void setLineWidth(float lineWidth) override;
    const cc::Data &getDataRef() const override;
    void fill() override;
    void setLineCap(const ccstd::string &lineCap) override;
    void setLineJoin(const ccstd::string &lineCap) override;
    void fillImageData(const Data &imageData, float imageWidth, float imageHeight, float offsetX, float offsetY) override;
    void strokeText(const ccstd::string &text, float /*x*/, float /*y*/, float /*maxWidth*/) override;
    void rect(float x, float y, float w, float h) override;
    void updateData() override {}
    void setShadowBlur(float blur) override { _shadowBlur = blur; }
    void setShadowColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override { _shadowColor = {r / 255.0F, g / 255.0F, b / 255.0F, a / 255.0F}; }
    void setShadowOffsetX(float offsetX) override { _shadowOffsetX = offsetX; }
    void setShadowOffsetY(float offsetY) override { _shadowOffsetY = offsetY; }

private:
    /** what the glyph coverage of the bitmap is painted as: the text (with its shadow), its outline, or a line (the old replace-the-pixel paint) */
    enum class Pass {
        FILL,
        STROKE,
        LINE
    };
    static wchar_t *utf8ToUtf16(const ccstd::string &str, int *pRetLen = nullptr);
    int drawText(const ccstd::string &text, int x, int y);
    Size sizeWithText(const wchar_t *pszText, int nLen);
    void prepareBitmap(int nWidth, int nHeight);
    void deleteBitmap();
    bool useBitmap();
    HDC useFont();
    void fillTextureData(Pass pass);
    ccstd::array<float, 2> convertDrawPoint(Point point, const ccstd::string &text);

    // A label owns no GDI object between two draws (a process may hold 10 000; a label used to keep a device context, a bitmap and a font, so about
    // 3 300 live labels blanked every new text). The device context and the fonts are shared by the process (the .cpp), the bitmap lives from the first draw
    // to fillTextureData and is deleted there.
    HBITMAP _bmp{nullptr};

private:
    cc::Data _imageData;
    HFONT _font{static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))}; // from the shared cache: not owned
    HWND _wnd{nullptr};
    HPEN _hpen{nullptr};
    PAINTSTRUCT _paintStruct;
    float _lineWidth{0.0F};
    float _bufferWidth{0.0F};
    float _bufferHeight{0.0F};

    ccstd::string _fontName;
    int _fontSize{0};
    Size _textSize;
    TextAlign _textAlign{TextAlign::CENTER};
    TextBaseline _textBaseLine{TextBaseline::TOP};
    Color4F _fillStyle;
    Color4F _strokeStyle;
    Color4F _shadowColor{0.0F, 0.0F, 0.0F, 0.0F};
    float _shadowBlur{0.0F};
    float _shadowOffsetX{0.0F};
    float _shadowOffsetY{0.0F};

    TEXTMETRIC _tm;
};

} // namespace cc
