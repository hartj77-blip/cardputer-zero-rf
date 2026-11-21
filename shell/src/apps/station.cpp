// Station: the operator's details and the radio hardware settings the RF apps
// share. Up/Down move, Enter edits or toggles, Esc cancels an edit.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "apps/common.h"
#include "aprs.h"
#include "ui/prefs.h"

namespace zs {
namespace {
using namespace theme;

enum class Kind { Text, Number, Toggle };

struct Field {
    const char* label;
    const char* key;
    Kind kind;
    const char* fallback;   // shown when nothing is set
    const char* help;
};

const Field kFields[] = {
    {"Callsign", "rf.callsign", Kind::Text, "not set", "Your amateur callsign"},
    {"Grid locator", "rf.grid", Kind::Text, "not set", "Maidenhead, like FN31pr. Used for distances"},
    {"Frequency correction", "rf.ppm", Kind::Number, "0", "Dongle crystal error in ppm, -500 to 500"},
    {"Antenna power", "rf.biastee", Kind::Toggle, "0", "Bias tee: 4.5 V up the coax for an amplifier"},
    {"APRS frequency", "rf.aprs_khz", Kind::Number, "144390", "In kHz. 144390 Americas, 144800 Europe"},
    {"Rig model", "rf.rig_model", Kind::Number, "0", "Hamlib number (rigctl -l lists them). 0 = none"},
    {"Rig port", "rf.rig_port", Kind::Text, "/dev/ttyUSB0", "Serial device the rig is on"},
    {"Rig speed", "rf.rig_baud", Kind::Number, "0", "Baud rate. 0 = the model's default"},
};

class StationApp : public App {
public:
    void build(lv_obj_t* root) override {
        title_ = label(root, "", &zs_font_14, kAccent);
        lv_obj_set_pos(title_, 4, 0);
        help_ = label(root, "", &zs_font_12, kMuted);
        lv_obj_align(help_, LV_ALIGN_BOTTOM_LEFT, 4, -1);
        lv_obj_set_width(help_, kScreenW - 8);
        lv_label_set_long_mode(help_, LV_LABEL_LONG_DOT);

        list_ = make_list(root, 0, 18, kScreenW, kContentH - 18 - 16);
        for (size_t i = 0; i < std::size(kFields); ++i) {
            lv_obj_t* row = list_button(list_, nullptr, kFields[i].label, on_clicked, this);
            lv_obj_add_event_cb(row, on_focused, LV_EVENT_FOCUSED, this);
            values_.push_back(lv_label_create(row));
        }
        finish_list(list_);
        refresh();
        show_help(0);
    }

    bool on_key(uint32_t key) override {
        if (editing_ < 0) return false;   // the list handles Up, Down and Enter
        const Field& f = kFields[editing_];
        if (key == LV_KEY_ENTER) {
            commit();
        } else if (key == LV_KEY_BACKSPACE) {
            if (!entry_.empty()) entry_.pop_back();
        } else if (key >= 32 && key < 127 && entry_.size() < 24) {
            char c = char(key);
            bool ok = f.kind == Kind::Number ? std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && entry_.empty())
                                             : c != ' ' && c != '=';
            if (ok) entry_ += c;
        }
        refresh();
        return true;
    }

    bool on_back() override {
        if (editing_ < 0) return false;
        editing_ = -1;
        refresh();
        return true;
    }

private:
    static void on_clicked(lv_event_t* e) {
        auto* self = static_cast<StationApp*>(lv_event_get_user_data(e));
        if (self->editing_ >= 0) return;
        int i = int(lv_obj_get_index(static_cast<lv_obj_t*>(lv_event_get_target(e))));
        const Field& f = kFields[i];
        if (f.kind == Kind::Toggle) {
            prefs::set_int(f.key, prefs::get_int(f.key, 0) ? 0 : 1);
            if (std::string(f.key) == "rf.biastee" && prefs::get_int(f.key, 0))
                shell_toast("On from the next time an RF app opens");
        } else {
            self->editing_ = i;
            self->entry_ = f.kind == Kind::Number ? std::to_string(prefs::get_int(f.key, std::atoi(f.fallback)))
                                                  : prefs::get_str(f.key, "");
            if (f.kind == Kind::Number && self->entry_ == "0") self->entry_.clear();
        }
        self->refresh();
    }

    static void on_focused(lv_event_t* e) {
        auto* self = static_cast<StationApp*>(lv_event_get_user_data(e));
        self->show_help(int(lv_obj_get_index(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
    }

    void show_help(int i) {
        focused_ = i;
        if (editing_ < 0) lv_label_set_text(help_, kFields[i].help);
    }

    void commit() {
        const Field& f = kFields[editing_];
        std::string key = f.key;
        if (f.kind == Kind::Number) {
            int v = std::atoi(entry_.c_str());
            if (key == "rf.ppm" && (v < -500 || v > 500)) { shell_toast("Between -500 and 500"); return; }
            if (key == "rf.aprs_khz" && (v < 144000 || v > 148000)) { shell_toast("Between 144000 and 148000 kHz"); return; }
            if ((key == "rf.rig_model" || key == "rf.rig_baud") && v < 0) { shell_toast("Can't be negative"); return; }
            prefs::set_int(key, v);
        } else {
            std::string v = entry_;
            if (key == "rf.callsign") {
                for (char& c : v) c = char(std::toupper(static_cast<unsigned char>(c)));
                if (!v.empty() && !zrf::looks_like_callsign(v)) { shell_toast("That doesn't look like a callsign"); return; }
            }
            if (key == "rf.grid" && !v.empty()) {
                double lat, lon;
                if (!zrf::grid_to_latlon(v, lat, lon)) { shell_toast("Use 4 or 6 characters, like FN31 or FN31pr"); return; }
            }
            prefs::set_str(key, v);
        }
        editing_ = -1;
    }

    std::string shown(const Field& f) const {
        std::string key = f.key;
        if (f.kind == Kind::Toggle) return prefs::get_int(f.key, 0) ? "on" : "off";
        if (f.kind == Kind::Text) {
            std::string v = prefs::get_str(f.key, "");
            return v.empty() ? f.fallback : v;
        }
        int v = prefs::get_int(f.key, std::atoi(f.fallback));
        if (key == "rf.ppm") return fmt("%d ppm", v);
        if (key == "rf.aprs_khz") return fmt("%.3f MHz", v / 1000.0);
        if (key == "rf.rig_model") return v ? fmt("%d", v) : "none";
        if (key == "rf.rig_baud") return v ? fmt("%d", v) : "default";
        return fmt("%d", v);
    }

    void refresh() {
        for (size_t i = 0; i < std::size(kFields); ++i)
            lv_label_set_text(values_[i], int(i) == editing_ ? (entry_ + "_").c_str() : shown(kFields[i]).c_str());
        std::string call = prefs::get_str("rf.callsign", "");
        if (editing_ >= 0) lv_label_set_text(title_, (std::string("Editing: ") + kFields[editing_].label).c_str());
        else lv_label_set_text(title_, call.empty() ? "Station" : ("Station " + call).c_str());
        lv_label_set_text(help_, editing_ >= 0 ? "Type, then Enter to save or Esc to cancel" : kFields[focused_].help);
    }

    lv_obj_t *title_ = nullptr, *help_ = nullptr, *list_ = nullptr;
    std::vector<lv_obj_t*> values_;
    int editing_ = -1, focused_ = 0;
    std::string entry_;
};
}  // namespace

std::unique_ptr<App> make_station_app() { return std::make_unique<StationApp>(); }

}  // namespace zs
