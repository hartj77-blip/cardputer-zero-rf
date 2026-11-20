// Keeps the radio playing after Spectrum is left, so you can listen while
// writing notes or reading files. One connection to the radio daemon that
// lives as long as the shell does and holds the audio open.
//
// The other RF apps stop it when they start: they need the receiver for
// something else. Opening Spectrum again takes the audio back over.
#pragma once

#include <string>

#include "apps/common.h"
#include "client.h"

namespace zs {

class BackgroundRadio {
public:
    static BackgroundRadio& get() {
        static BackgroundRadio instance;
        return instance;
    }

    bool playing() const { return client_.connected(); }
    double hz() const { return hz_; }
    const std::string& mode() const { return mode_; }

    // Take over listening. The daemon must already be running (the app that
    // hands over has just been using it). False if it could not be reached.
    bool start(double hz, double rate, const std::string& mode, bool squelch) {
        stop();
        if (!client_.connect()) return false;
        hz_ = hz;
        mode_ = mode;
        client_.send(fmt("rate %.0f", rate));
        client_.send(fmt("tune %.0f", hz));
        client_.send(squelch ? "squelch 10" : "squelch off");
        client_.send("mode " + mode);
        // The daemon may have things to say (the dongle was unplugged, say);
        // read them so they don't pile up, and notice if it goes away.
        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<BackgroundRadio*>(lv_timer_get_user_data(t))->drain(); }, 500, this);
        return client_.connected();
    }

    // Stop the audio.
    void stop() {
        if (client_.connected()) client_.send("mode off");
        release();
    }

    // Let go without stopping the audio: another connection has already
    // asked for the same thing and carries on.
    void release() {
        client_.close();
        if (timer_) lv_timer_delete(timer_);
        timer_ = nullptr;
    }

private:
    void drain() {
        zrf::RadioEvents ev;
        if (!client_.poll(0, ev)) release();
    }

    zrf::RadioClient client_;
    lv_timer_t* timer_ = nullptr;
    double hz_ = 0;
    std::string mode_;
};

}  // namespace zs
