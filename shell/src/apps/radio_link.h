// Shared by the RF apps: keep a connection to the radio daemon, starting the
// daemon when it isn't running.
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "client.h"

namespace zs {

class RadioLink {
public:
    zrf::RadioClient client;

    // The headless build's clock is not real time, so its apps wait for the
    // daemon; everywhere else the UI must never block.
    static bool headless() {
        static const bool h = std::strcmp(ZS_BACKEND_NAME, "headless") == 0;
        return h;
    }
    static int poll_wait_ms() { return headless() ? 100 : 0; }

    // Call from a timer (about every 40 ms). True when connected. `fresh` is
    // set when the connection was just made, so the app can send its setup.
    // `unavailable` is set once the daemon has failed to appear for a while.
    bool ensure(bool& fresh, bool& unavailable) {
        fresh = unavailable = false;
        if (client.connected()) return true;
        if (retry_in_ > 0) { --retry_in_; return false; }
        if (client.connect()) {
            failures_ = 0;
            fresh = true;
            return true;
        }
        if (!spawned_) {
            const char* src = std::getenv("ZRF_SOURCE");
            bool device = std::strcmp(ZS_BACKEND_NAME, "device") == 0;
            spawned_ = zrf::spawn_daemon(src && *src ? src : device ? "rtlsdr" : "sim");
        } else if (++failures_ > 10) {
            unavailable = true;
        }
        if (headless()) usleep(50 * 1000);
        retry_in_ = headless() ? 0 : 6;
        return false;
    }

    // The daemon went away: start it again on the next ensure().
    void lost() { spawned_ = false; }

private:
    bool spawned_ = false;
    int retry_in_ = 0, failures_ = 0;
};

}  // namespace zs
