/****************************************************************************
 Copyright (c) 2017-2023 Xiamen Yaji Software Co., Ltd.

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

#include "BaseGame.h"
#include <string>
#include "ApplicationManager.h"
#include "platform/interfaces/modules/ISystemWindowManager.h"
#include "renderer/pipeline/GlobalDescriptorSetManager.h"

#if CC_PLATFORM == CC_PLATFORM_ANDROID
    #include "platform/android/adpf_manager.h"
#endif
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    #include <Windows.h>
#endif
extern "C" void cc_load_all_plugins(); // NOLINT

#if CC_PLATFORM == CC_PLATFORM_WINDOWS
namespace {
// The first window of the Windows exe, in physical pixels (the process is DPI-aware, so SDL takes pixels): the game's stage, 1050 x 665 (the Flash client's
// default size; owner 2026-11-03: the Windows default must be the Flash size, after a window of up to 2 stages was tried), times the display's DPI scale, so it is as
// large on the screen as the Flash client's window was (a DPI-unaware Flash player is stretched by Windows the same way). Shrunk to fit the work area when the screen is
// small, never below half a stage. The engine's own default was 800 x 600, the game drawn at 0.76 of its stage.
void defaultWindowSize(int &width, int &height) {
    const float stageWidth = 1050.0F;
    const float stageHeight = 665.0F;
    float dpiScale = 1.0F;
    using GetDpiForSystemFn = UINT(WINAPI *)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto getDpiForSystem = user32 ? reinterpret_cast<GetDpiForSystemFn>(reinterpret_cast<void *>(GetProcAddress(user32, "GetDpiForSystem"))) : nullptr;
    if (getDpiForSystem) {
        dpiScale = static_cast<float>(getDpiForSystem()) / 96.0F;
    }
    float scale = dpiScale;
    RECT work = {0, 0, 0, 0};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        // room for the title bar, the frame and the 50 px the window starts from
        const float fitWidth = (static_cast<float>(work.right - work.left) - 100.0F * dpiScale) / stageWidth;
        const float fitHeight = (static_cast<float>(work.bottom - work.top) - 140.0F * dpiScale) / stageHeight;
        const float fit = fitWidth < fitHeight ? fitWidth : fitHeight;
        if (fit < scale) {
            scale = fit;
        }
    }
    if (scale < 0.5F * dpiScale) {
        scale = 0.5F * dpiScale;
    }
    width = static_cast<int>(stageWidth * scale + 0.5F);
    height = static_cast<int>(stageHeight * scale + 0.5F);
}
} // namespace
#endif

namespace cc {

BaseGame::~BaseGame() { // NOLINT
#if (CC_PLATFORM == CC_PLATFORM_ANDROID) && CC_SUPPORT_ADPF
    ADPFManager::getInstance().destroy();
#endif
}

int BaseGame::init() {
    cc::pipeline::GlobalDSManager::setDescriptorSetLayout();

    cc_load_all_plugins();

#if (CC_PLATFORM == CC_PLATFORM_ANDROID) && CC_SUPPORT_ADPF
    ADPFManager::getInstance().initialize();
#endif

#if CC_PLATFORM == CC_PLATFORM_WINDOWS || CC_PLATFORM == CC_PLATFORM_LINUX || CC_PLATFORM == CC_PLATFORM_QNX || CC_PLATFORM == CC_PLATFORM_MACOS
    // override default value
    //_windowInfo.x      = _windowInfo.x == -1 ? 0 : _windowInfo.x;
    //_windowInfo.y      = _windowInfo.y == -1 ? 0 : _windowInfo.y;
    #if CC_PLATFORM == CC_PLATFORM_WINDOWS
    int defaultWidth = 800;
    int defaultHeight = 600;
    defaultWindowSize(defaultWidth, defaultHeight);
    _windowInfo.width = _windowInfo.width == -1 ? defaultWidth : _windowInfo.width;
    _windowInfo.height = _windowInfo.height == -1 ? defaultHeight : _windowInfo.height;
    _windowInfo.flags = _windowInfo.flags == -1 ? cc::ISystemWindow::CC_WINDOW_SHOWN |
                                                      cc::ISystemWindow::CC_WINDOW_RESIZABLE |
                                                      cc::ISystemWindow::CC_WINDOW_INPUT_FOCUS |
                                                      cc::ISystemWindow::CC_WINDOW_ALLOW_HIGHDPI
                                                : _windowInfo.flags;
    #else
    _windowInfo.width = _windowInfo.width == -1 ? 800 : _windowInfo.width;
    _windowInfo.height = _windowInfo.height == -1 ? 600 : _windowInfo.height;
    _windowInfo.flags = _windowInfo.flags == -1 ? cc::ISystemWindow::CC_WINDOW_SHOWN |
                                                      cc::ISystemWindow::CC_WINDOW_RESIZABLE |
                                                      cc::ISystemWindow::CC_WINDOW_INPUT_FOCUS
                                                : _windowInfo.flags;
    #endif
    std::call_once(_windowCreateFlag, [&]() {
        ISystemWindowInfo info;
        info.title = _windowInfo.title;
    #if CC_PLATFORM == CC_PLATFORM_WINDOWS
        info.x = _windowInfo.x == -1 ? 50 : _windowInfo.x; // 50 meams move window a little for now
        info.y = _windowInfo.y == -1 ? 50 : _windowInfo.y; // same above
    #else
        info.x = _windowInfo.x == -1 ? 0 : _windowInfo.x;
        info.y = _windowInfo.y == -1 ? 0 : _windowInfo.y;
    #endif
        info.width = _windowInfo.width;
        info.height = _windowInfo.height;
        info.flags = _windowInfo.flags;

        ISystemWindowManager* windowMgr = CC_GET_PLATFORM_INTERFACE(ISystemWindowManager);
        windowMgr->createWindow(info);
    });

#endif

    if (_debuggerInfo.enabled) {
        setDebugIpAndPort(_debuggerInfo.address, _debuggerInfo.port, _debuggerInfo.pauseOnStart);
    }

    int ret = cc::CocosApplication::init();
    if (ret != 0) {
        return ret;
    }

    setXXTeaKey(_xxteaKey);
    runScript("jsb-adapter/web-adapter.js");
    runScript("main.js");
    return 0;
}
} // namespace cc
