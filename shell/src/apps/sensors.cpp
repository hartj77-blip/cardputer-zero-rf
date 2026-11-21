// Sensors: wireless weather stations, tyre sensors, doorbells and remotes
// decoded by rtl_433, one row per device with its latest readings.
//
// Up/Down move, Enter shows everything the selected device sent, F changes
// band (433.92, 315, 868.3, 915 MHz), C clears the list.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "apps/background_radio.h"
#include "apps/common.h"
#include "apps/radio_link.h"
#include "json_flat.h"

namespace zs {
namespace {
using namespace theme;

const double kBands[] = {433.92e6, 315e6, 868.3e6, 915e6};
const int kTopH = 18;
const size_t kMaxDevices = 40;

struct Device {
    std::string key, name;
    zrf::JsonFields fields;   // the latest message
    uint32_t last_seen = 0;   // lv_tick
    int count = 0;
    lv_obj_t* row = nullptr;
    lv_obj_t* value = nullptr;
};

std::string age_text(uint32_t ms) {
    if (ms < 2000) return "now";
    if (ms < 60000) return fmt("%us", ms / 1000);
    if (ms < 3600000) return fmt("%um", ms / 60000);
    return fmt("%uh", ms / 3600000);
}

// The readings worth a place on the row, most useful first.
std::string summary(const zrf::JsonFields& f) {
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : "  ") + s; };
    auto num = [&](const char* key) -> const std::string* { return zrf::json_get(f, key); };
    if (auto v = num("temperature_C")) add(fmt("%.1f°C", std::atof(v->c_str())));
    if (auto v = num("humidity")) add(fmt("%.0f%%", std::atof(v->c_str())));
    if (auto v = num("pressure_kPa")) add(fmt("%.0f kPa", std::atof(v->c_str())));
    if (auto v = num("pressure_hPa")) add(fmt("%.0f hPa", std::atof(v->c_str())));
    if (auto v = num("wind_avg_m_s")) add(fmt("%.1f m/s", std::atof(v->c_str())));
    if (auto v = num("wind_avg_km_h")) add(fmt("%.0f km/h", std::atof(v->c_str())));
    if (out.empty()) {
        for (const char* key : {"button", "cmd", "code", "event", "state", "alarm", "data"})
            if (auto v = num(key)) { add(std::string(key) + " " + v->substr(0, 12)); break; }
    }
    if (auto v = num("battery_ok"); v && *v == "0") add("low batt");
    return out;
}

class SensorsApp : public App {
public:
    ~SensorsApp() override {
        if (timer_) lv_timer_delete(timer_);
    }

    void build(lv_obj_t* root) override {
        BackgroundRadio::get().stop();   // this app needs the receiver
        root_ = root;
        title_ = label(root, "", &zs_font_14, kAccent);
        lv_obj_set_pos(title_, 4, 0);
        hint_ = label(root, "", &zs_font_12, kMuted);
        lv_obj_align(hint_, LV_ALIGN_TOP_RIGHT, -4, 2);

        list_ = make_list(root, 0, kTopH, kScreenW, kContentH - kTopH);
        finish_list(list_);

        detail_ = lv_obj_create(root);
        style_panel(detail_);
        lv_obj_set_pos(detail_, 4, kTopH);
        lv_obj_set_size(detail_, kScreenW - 8, kContentH - kTopH - 2);
        detail_text_ = label(detail_, "", &zs_font_12, kText);
        lv_obj_set_width(detail_text_, kScreenW - 30);
        lv_obj_add_flag(detail_, LV_OBJ_FLAG_HIDDEN);

        message_ = label(root, "", &zs_font_14, kMuted);
        lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(message_, LV_ALIGN_CENTER, 0, 8);

        set_message("Starting radio...");
        update_title();
        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<SensorsApp*>(lv_timer_get_user_data(t))->pump(); },
                                 40, this);
        pump();
    }

    void tick() override {
        // Ages only need refreshing about once a second.
        if (++ticks_ % 10 != 0) return;
        for (auto& d : devices_) update_row(d);
    }

    bool on_key(uint32_t key) override {
        if (showing_ >= 0) {
            // Up/Down scroll the detail page.
            if (key == LV_KEY_DOWN) { lv_obj_scroll_by_bounded(detail_, 0, -24, LV_ANIM_OFF); return true; }
            if (key == LV_KEY_UP) { lv_obj_scroll_by_bounded(detail_, 0, 24, LV_ANIM_OFF); return true; }
            if (key == LV_KEY_ENTER) { close_detail(); return true; }
            return key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
        }
        switch (key) {
        case 'f': case 'F':
            band_ = (band_ + 1) % int(std::size(kBands));
            link_.client.send(fmt("tune %.0f", kBands[band_]));
            clear();
            update_title();
            return true;
        case 'c': case 'C':
            clear();
            update_title();
            return true;
        default: return false;
        }
    }

    bool on_back() override {
        if (showing_ < 0) return false;
        close_detail();
        return true;
    }

private:
    void pump() {
        bool fresh, unavailable;
        if (!link_.ensure(fresh, unavailable)) {
            if (unavailable) set_message("Radio service not available");
            return;
        }
        if (fresh) {
            link_.client.send(fmt("tune %.0f", kBands[band_]));
            link_.client.send("decode sensors");
            if (devices_.empty()) set_message("Listening for sensors...\nWeather stations, tyre sensors, remotes");
        }
        zrf::RadioEvents ev;
        if (!link_.client.poll(RadioLink::poll_wait_ms(), ev)) {
            link_.lost();
            set_message("Radio stopped. Restarting...");
            return;
        }
        if (ev.has_error) {
            if (ev.error == "no dongle") set_message("No radio found.\nPlug in an SDR dongle.");
            else if (ev.error == "program not installed") set_message("The sensor decoder (rtl_433)\nis not installed.");
            else if (devices_.empty()) set_message(("Radio: " + ev.error).c_str());
        }
        for (auto& line : ev.decoded) add_message(line);
        if (!ev.decoded.empty()) update_title();
    }

    void add_message(const std::string& line) {
        zrf::JsonFields f = zrf::parse_flat_json(line);
        const std::string* model = zrf::json_get(f, "model");
        if (!model) return;
        const std::string* id = zrf::json_get(f, "id");
        const std::string* channel = zrf::json_get(f, "channel");
        std::string key = *model + "|" + (id ? *id : "") + "|" + (channel ? *channel : "");
        ++messages_;

        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.key == key; });
        if (it == devices_.end()) {
            if (devices_.size() >= kMaxDevices) return;
            Device d;
            d.key = key;
            d.name = *model + (id ? " " + *id : "");
            d.row = list_button(list_, nullptr, d.name.c_str(), on_row_clicked, this);
            d.value = lv_label_create(d.row);
            finish_list(list_);
            devices_.push_back(std::move(d));
            it = devices_.end() - 1;
            set_message("");
        }
        it->fields = std::move(f);
        it->last_seen = lv_tick_get();
        ++it->count;
        update_row(*it);
        if (showing_ == int(it - devices_.begin())) fill_detail();
    }

    void update_row(Device& d) {
        std::string s = summary(d.fields);
        lv_label_set_text(d.value, (s + (s.empty() ? "" : "   ") + age_text(lv_tick_elaps(d.last_seen))).c_str());
    }

    static void on_row_clicked(lv_event_t* e) {
        auto* self = static_cast<SensorsApp*>(lv_event_get_user_data(e));
        lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_target(e));
        for (size_t i = 0; i < self->devices_.size(); ++i)
            if (self->devices_[i].row == row) { self->open_detail(int(i)); return; }
    }

    void open_detail(int index) {
        showing_ = index;
        fill_detail();
        lv_obj_scroll_to_y(detail_, 0, LV_ANIM_OFF);
        lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(detail_, LV_OBJ_FLAG_HIDDEN);
        update_title();
    }

    void close_detail() {
        showing_ = -1;
        lv_obj_add_flag(detail_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(list_, LV_OBJ_FLAG_HIDDEN);
        update_title();
    }

    void fill_detail() {
        const Device& d = devices_[size_t(showing_)];
        std::string text = fmt("%d messages\n", d.count);
        for (auto& kv : d.fields) {
            if (kv.first == "time") continue;
            text += kv.first + ":  " + kv.second.substr(0, 40) + "\n";
        }
        lv_label_set_text(detail_text_, text.c_str());
    }

    void clear() {
        if (showing_ >= 0) close_detail();
        lv_obj_clean(list_);
        // The rows are gone; drop them from the focus group too (lv_obj_clean did).
        devices_.clear();
        messages_ = 0;
        set_message("Listening for sensors...\nWeather stations, tyre sensors, remotes");
    }

    void set_message(const char* text) {
        lv_label_set_text(message_, text);
        if (*text) lv_obj_remove_flag(message_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(message_, LV_OBJ_FLAG_HIDDEN);
    }

    void update_title() {
        if (showing_ >= 0) {
            lv_label_set_text(title_, devices_[size_t(showing_)].name.c_str());
            lv_label_set_text(hint_, "Esc back");
            return;
        }
        lv_label_set_text(title_, fmt("%g MHz", kBands[band_] / 1e6).c_str());
        if (devices_.empty()) lv_label_set_text(hint_, "F band");
        else lv_label_set_text(hint_, fmt("%d devices   F band   C clear", int(devices_.size())).c_str());
    }

    RadioLink link_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *root_ = nullptr, *title_ = nullptr, *hint_ = nullptr, *list_ = nullptr;
    lv_obj_t *detail_ = nullptr, *detail_text_ = nullptr, *message_ = nullptr;
    std::vector<Device> devices_;
    int band_ = 0, showing_ = -1, messages_ = 0, ticks_ = 0;
};
}  // namespace

std::unique_ptr<App> make_sensors_app() { return std::make_unique<SensorsApp>(); }

}  // namespace zs
