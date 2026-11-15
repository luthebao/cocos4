/****************************************************************************
 Native crash reports (mcgame fork): sentry-native with its crashpad backend, on Windows only for now (native/vendor/sentry-native/win64, CC_USE_CRASH_REPORT).
 A crash of the native code is caught by crashpad_handler.exe (a process of its own, next to the exe), written as a minidump under
 <writable path>/sentry/ and sent to the DSN with the attached files, at once or at the next start. The game starts it from JS (jsb.startCrashReport), so its
 DSN and its version (an OTA package's, not the exe's) come from the game's code.
****************************************************************************/

#pragma once

#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {
namespace crashreport {

/** true where this build carries the crash reports */
bool available();

/** starts once: `dsn` of the Sentry project, `release` (e.g. mcgame@0.10.3), `environment`, files read at the crash and sent with it; false if it did not start */
bool start(const ccstd::string &dsn, const ccstd::string &release, const ccstd::string &environment, const ccstd::vector<ccstd::string> &attachments);

/** a tag of the next crash report */
void setTag(const ccstd::string &key, const ccstd::string &value);

/** a breadcrumb of the next crash report (what the game did before it) */
void addBreadcrumb(const ccstd::string &message, const ccstd::string &category);

} // namespace crashreport
} // namespace cc
