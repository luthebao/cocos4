/****************************************************************************
 Native crash reports on Windows (mcgame fork, platform/CrashReport.h): sentry-native 0.17.5, crashpad backend (native/vendor/sentry-native/win64).
****************************************************************************/

#include "platform/CrashReport.h"

#if defined(CC_USE_CRASH_REPORT)
    #include <chrono>
    #include <cstdlib>
    #include <thread>
    #include "platform/FileUtils.h"
    #include "sentry.h"
#endif

namespace cc {
namespace crashreport {

#if defined(CC_USE_CRASH_REPORT)

namespace {
bool started = false;

void closeAtExit() {
    sentry_close();
}
} // namespace

bool available() {
    return true;
}

bool start(const ccstd::string &dsn, const ccstd::string &release, const ccstd::string &environment, const ccstd::vector<ccstd::string> &attachments) {
    if (started) return true;
    if (dsn.empty()) return false;
    // MC_NO_CRASH_REPORT=1 turns it off on one PC (a handler that cannot start, a test)
    const char *off = std::getenv("MC_NO_CRASH_REPORT");
    if (off && off[0] == '1') return false;

    sentry_options_t *o = sentry_options_new();
    sentry_options_set_dsn(o, dsn.c_str());
    if (!release.empty()) sentry_options_set_release(o, release.c_str());
    if (!environment.empty()) sentry_options_set_environment(o, environment.c_str());
    // UTF-8 paths (sentry converts them to wide paths on Windows); the handler is crashpad_handler.exe next to the exe, sentry's default
    const ccstd::string db = FileUtils::getInstance()->getWritablePath() + "sentry";
    sentry_options_set_database_path(o, db.c_str());
    for (const auto &a : attachments) {
        if (!a.empty()) sentry_options_add_attachment(o, a.c_str());
    }
    if (sentry_init(o) != 0) return false;
    started = true;
    std::atexit(closeAtExit);

    // MC_CRASH_TEST=1: a crash 5 s after the start, to see a report reach Sentry (never set on a player's PC)
    const char *test = std::getenv("MC_CRASH_TEST");
    if (test && test[0] == '1') {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            volatile int *nowhere = nullptr;
            *nowhere = 1;
        }).detach();
    }
    return true;
}

void setTag(const ccstd::string &key, const ccstd::string &value) {
    if (started) sentry_set_tag(key.c_str(), value.c_str());
}

void addBreadcrumb(const ccstd::string &message, const ccstd::string &category) {
    if (!started) return;
    sentry_value_t crumb = sentry_value_new_breadcrumb("default", message.c_str());
    if (!category.empty()) sentry_value_set_by_key(crumb, "category", sentry_value_new_string(category.c_str()));
    sentry_add_breadcrumb(crumb);
}

#else

bool available() {
    return false;
}

bool start(const ccstd::string & /*dsn*/, const ccstd::string & /*release*/, const ccstd::string & /*environment*/, const ccstd::vector<ccstd::string> & /*attachments*/) {
    return false;
}

void setTag(const ccstd::string & /*key*/, const ccstd::string & /*value*/) {}

void addBreadcrumb(const ccstd::string & /*message*/, const ccstd::string & /*category*/) {}

#endif

} // namespace crashreport
} // namespace cc
