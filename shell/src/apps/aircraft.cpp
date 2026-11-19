// Aircraft: ADS-B position reports from aircraft overhead (1090 MHz), one row
// per aircraft. Up/Down scroll, C clears.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "adsb.h"
#include "apps/common.h"
#include "apps/radio_link.h"

namespace zs {
namespace {
using namespace theme;

const int kRows = 7;
const int kRowH = 16;
const int kTopH = 18;
// Column left edges: flight, altitude, speed, heading, position.
const int kCol[] = {6, 76, 132, 174, 232};

class AircraftApp : public App {
public:
    ~AircraftApp() override {
        if (timer_) lv_timer_delete(timer_);
    }

    void build(lv_obj_t* root) override {
        title_ = label(root, "1090 MHz", &lv_font_montserrat_14, kAccent);
        lv_obj_set_pos(title_, 4, 0);
        count_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(count_, LV_ALIGN_TOP_RIGHT, -4, 2);

        const char* heads[] = {"Flight", "Alt ft", "Kt", "Hdg", "Position"};
        for (int c = 0; c < 5; ++c) {
            lv_obj_t* h = label(root, heads[c], &lv_font_montserrat_12, kMuted);
            lv_obj_set_pos(h, kCol[c], kTopH);
        }
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < 5; ++c) {
                cells_[r][c] = label(root, "", &lv_font_montserrat_12, kText);
                lv_obj_set_pos(cells_[r][c], kCol[c], kTopH + (r + 1) * kRowH);
            }
        message_ = label(root, "", &lv_font_montserrat_14, kMuted);
        lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(message_, LV_ALIGN_CENTER, 0, 14);
        set_message("Starting radio...");

        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<AircraftApp*>(lv_timer_get_user_data(t))->pump(); },
                                 40, this);
        pump();
    }

    void tick() override {
        if (++ticks_ % 10 != 0) return;
        tracker_.expire(now(), 60);
        refresh();
    }

    bool on_key(uint32_t key) override {
        int n = int(tracker_.aircraft().size());
        switch (key) {
        case LV_KEY_DOWN: top_ = std::min(top_ + 1, std::max(0, n - kRows)); refresh(); return true;
        case LV_KEY_UP: top_ = std::max(top_ - 1, 0); refresh(); return true;
        case 'c': case 'C': tracker_ = zrf::AdsbTracker(); frames_ = 0; top_ = 0; refresh(); return true;
        case LV_KEY_ENTER: case LV_KEY_LEFT: case LV_KEY_RIGHT: return true;
        default: return false;
        }
    }

private:
    static double now() { return lv_tick_get() / 1000.0; }

    void pump() {
        bool fresh, unavailable;
        if (!link_.ensure(fresh, unavailable)) {
            if (unavailable) set_message("Radio service not available");
            return;
        }
        if (fresh) {
            link_.client.send("decode aircraft");
            if (tracker_.aircraft().empty()) set_message("Listening for aircraft...\nBest outdoors or by a window");
        }
        zrf::RadioEvents ev;
        if (!link_.client.poll(RadioLink::poll_wait_ms(), ev)) {
            link_.lost();
            set_message("Radio stopped. Restarting...");
            return;
        }
        if (ev.has_error) {
            if (ev.error == "no dongle") set_message("No radio found.\nPlug in an SDR dongle.");
            else if (ev.error == "program not installed") set_message("The aircraft decoder (rtl_adsb)\nis not installed.");
            else if (tracker_.aircraft().empty()) set_message(("Radio: " + ev.error).c_str());
        }
        bool any = false;
        for (auto& line : ev.decoded)
            if (tracker_.add(line, now())) { ++frames_; any = true; }
        if (any) refresh();
    }

    void refresh() {
        // Most recently heard first.
        std::vector<zrf::Aircraft> list = tracker_.aircraft();
        std::sort(list.begin(), list.end(), [](const zrf::Aircraft& a, const zrf::Aircraft& b) {
            return a.last_seen != b.last_seen ? a.last_seen > b.last_seen : a.icao < b.icao;
        });
        top_ = std::clamp(top_, 0, std::max(0, int(list.size()) - kRows));
        for (int r = 0; r < kRows; ++r) {
            size_t i = size_t(top_ + r);
            if (i >= list.size()) {
                for (auto* c : cells_[r]) lv_label_set_text(c, "");
                continue;
            }
            const zrf::Aircraft& a = list[i];
            // Fade aircraft that have gone quiet.
            uint32_t color = now() - a.last_seen > 15 ? kMuted : kText;
            std::string text[5] = {
                a.callsign.empty() ? fmt("%06X", a.icao) : a.callsign,
                a.has_altitude ? fmt("%d", a.altitude_ft) : "-",
                a.has_velocity ? fmt("%.0f", a.speed_kt) : "-",
                a.has_velocity ? fmt("%.0f°%s", a.heading_deg, a.climb_fpm > 300 ? " " LV_SYMBOL_UP : a.climb_fpm < -300 ? " " LV_SYMBOL_DOWN : "") : "-",
                a.has_position ? fmt("%.2f, %.2f", a.lat, a.lon) : "-",
            };
            for (int c = 0; c < 5; ++c) {
                lv_label_set_text(cells_[r][c], text[c].c_str());
                lv_obj_set_style_text_color(cells_[r][c], theme::c(c == 0 && color == kText ? kAccent : color), 0);
            }
        }
        if (!list.empty()) set_message("");
        lv_label_set_text(count_, fmt("%d aircraft   %d messages", int(list.size()), frames_).c_str());
    }

    void set_message(const char* text) {
        lv_label_set_text(message_, text);
        if (*text) lv_obj_remove_flag(message_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(message_, LV_OBJ_FLAG_HIDDEN);
    }

    RadioLink link_;
    zrf::AdsbTracker tracker_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *title_ = nullptr, *count_ = nullptr, *message_ = nullptr;
    lv_obj_t* cells_[kRows][5] = {};
    int top_ = 0, frames_ = 0, ticks_ = 0;
};
}  // namespace

std::unique_ptr<App> make_aircraft_app() { return std::make_unique<AircraftApp>(); }

}  // namespace zs
