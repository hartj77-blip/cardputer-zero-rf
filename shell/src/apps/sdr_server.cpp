// SDR Server: lend the dongle to an SDR program on another computer or phone
// (SDR++, GQRX, SDR#, SDR Touch) over the network, using rtl_tcp.
// Enter starts and stops it. It stops when you leave the app.
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "apps/background_radio.h"
#include "apps/common.h"
#include "apps/radio_link.h"

namespace zs {
namespace {
using namespace theme;

const int kPort = 1234;

// This device's IPv4 addresses, with the interface each belongs to.
std::vector<std::string> addresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
        out.push_back(fmt("%s:%d   (%s)", buf, kPort, i->ifa_name));
    }
    freeifaddrs(list);
    return out;
}

class SdrServerApp : public App {
public:
    ~SdrServerApp() override {
        if (timer_) lv_timer_delete(timer_);
    }

    void build(lv_obj_t* root) override {
        BackgroundRadio::get().stop();   // this app needs the receiver
        state_ = label(root, "", &lv_font_montserrat_16, kMuted);
        lv_obj_set_pos(state_, 8, 4);
        body_ = label(root, "", &lv_font_montserrat_12, kText);
        lv_obj_set_pos(body_, 8, 30);
        lv_obj_set_width(body_, kScreenW - 16);
        log_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(log_, LV_ALIGN_BOTTOM_LEFT, 8, -18);
        lv_obj_set_width(log_, kScreenW - 16);
        lv_label_set_long_mode(log_, LV_LABEL_LONG_DOT);
        hint_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -1);
        refresh();
        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<SdrServerApp*>(lv_timer_get_user_data(t))->pump(); },
                                 40, this);
    }

    void tick() override {
        if (++ticks_ % 20 == 0) refresh();   // addresses change when Wi-Fi does
    }

    bool on_key(uint32_t key) override {
        if (key == LV_KEY_ENTER || key == ' ') {
            on_ = !on_;
            problem_.clear();
            last_line_.clear();
            link_.client.send(on_ ? "decode server" : "decode off");
            refresh();
            return true;
        }
        return key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
    }

private:
    void pump() {
        bool fresh, unavailable;
        if (!link_.ensure(fresh, unavailable)) {
            if (unavailable) { problem_ = "Radio service not available"; refresh(); }
            return;
        }
        if (fresh && on_) link_.client.send("decode server");
        zrf::RadioEvents ev;
        if (!link_.client.poll(RadioLink::poll_wait_ms(), ev)) {
            link_.lost();
            return;
        }
        bool changed = false;
        if (ev.has_error) {
            problem_ = ev.error == "no dongle" ? "No radio found. Plug in an SDR dongle."
                       : ev.error == "program not installed" ? "rtl_tcp is not installed."
                       : "Radio: " + ev.error;
            changed = true;
        }
        if (!ev.decoded.empty()) {
            last_line_ = ev.decoded.back();
            problem_.clear();
            changed = true;
        }
        if (changed) refresh();
    }

    void refresh() {
        if (!on_) {
            lv_label_set_text(state_, "SDR server is off");
            lv_obj_set_style_text_color(state_, c(kMuted), 0);
            lv_label_set_text(body_, "Lets an SDR program on a computer or phone use this "
                                     "radio over Wi-Fi or Tailscale (rtl_tcp).\n\n"
                                     "While it is on, anyone on the same network can tune "
                                     "the radio, and the other RF apps can't use it.");
            lv_label_set_text(log_, "");
            lv_label_set_text(hint_, "Enter  start");
            return;
        }
        bool bad = !problem_.empty();
        lv_label_set_text(state_, bad ? "SDR server can't start" : LV_SYMBOL_OK "  SDR server is on");
        lv_obj_set_style_text_color(state_, c(bad ? kBad : kGood), 0);
        std::string text = bad ? problem_ : "In the SDR program, choose RTL-TCP and enter:\n";
        if (!bad) {
            std::vector<std::string> addrs = addresses();
            if (addrs.empty()) text += "(no network: join Wi-Fi first)";
            for (size_t i = 0; i < addrs.size() && i < 4; ++i) text += "    " + addrs[i] + "\n";
        }
        lv_label_set_text(body_, text.c_str());
        lv_label_set_text(log_, last_line_.c_str());
        lv_label_set_text(hint_, "Enter  stop");
    }

    RadioLink link_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *state_ = nullptr, *body_ = nullptr, *log_ = nullptr, *hint_ = nullptr;
    bool on_ = false;
    std::string problem_, last_line_;
    int ticks_ = 0;
};
}  // namespace

std::unique_ptr<App> make_sdr_server_app() { return std::make_unique<SdrServerApp>(); }

}  // namespace zs
