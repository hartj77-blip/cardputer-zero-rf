#include "adsb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace zrf {
namespace {

const char kCharset[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Bits first..last of the 56-bit message field, numbered from 1 as the
// standard does.
uint32_t bits(const uint8_t* me, int first, int last) {
    uint32_t v = 0;
    for (int b = first; b <= last; ++b) v = v << 1 | (me[(b - 1) / 8] >> (7 - (b - 1) % 8) & 1);
    return v;
}
void put(uint8_t* me, int first, int last, uint32_t v) {
    for (int b = last; b >= first; --b, v >>= 1)
        if (v & 1) me[(b - 1) / 8] |= uint8_t(1 << (7 - (b - 1) % 8));
}

double mod(double a, double b) { return a - b * std::floor(a / b); }

// Number of longitude zones at a latitude.
int zones(double lat) {
    lat = std::fabs(lat);
    if (lat < 1e-9) return 59;
    if (lat > 87) return 1;
    if (lat == 87) return 2;
    double a = 1 - std::cos(M_PI / 30);
    double c = std::cos(M_PI / 180 * lat);
    return int(std::floor(2 * M_PI / std::acos(1 - a / (c * c))));
}

// Place an aircraft from an even and an odd position message heard close
// together. False if the pair doesn't agree (it straddled a zone boundary).
bool locate(Aircraft& a, int newest) {
    const double scale = 131072.0;
    double lat_e = a.cpr_lat[0] / scale, lat_o = a.cpr_lat[1] / scale;
    double lon_e = a.cpr_lon[0] / scale, lon_o = a.cpr_lon[1] / scale;
    double j = std::floor(59 * lat_e - 60 * lat_o + 0.5);
    double rlat_e = 360.0 / 60 * (mod(j, 60) + lat_e), rlat_o = 360.0 / 59 * (mod(j, 59) + lat_o);
    if (rlat_e >= 270) rlat_e -= 360;
    if (rlat_o >= 270) rlat_o -= 360;
    if (rlat_e < -90 || rlat_e > 90 || rlat_o < -90 || rlat_o > 90) return false;
    if (zones(rlat_e) != zones(rlat_o)) return false;
    double lat = newest == 0 ? rlat_e : rlat_o;
    int nl = zones(lat);
    int ni = std::max(nl - newest, 1);
    double m = std::floor(lon_e * (nl - 1) - lon_o * nl + 0.5);
    double lon = 360.0 / ni * (mod(m, ni) + (newest == 0 ? lon_e : lon_o));
    if (lon >= 180) lon -= 360;
    a.lat = lat;
    a.lon = lon;
    a.has_position = true;
    return true;
}

uint32_t crc_bytes(const uint8_t* data, int n);

std::string finish(uint8_t* frame) {
    // Checksum: the remainder of the first 11 bytes goes in the last 3.
    uint32_t crc = crc_bytes(frame, 11);
    frame[11] = uint8_t(crc >> 16);
    frame[12] = uint8_t(crc >> 8);
    frame[13] = uint8_t(crc);
    static const char digits[] = "0123456789ABCDEF";
    std::string s = "*";
    for (int i = 0; i < 14; ++i) { s += digits[frame[i] >> 4]; s += digits[frame[i] & 15]; }
    return s + ";";
}

void header(uint8_t* frame, uint32_t icao) {
    std::memset(frame, 0, 14);
    frame[0] = 0x8D;   // downlink format 17, capability 5
    frame[1] = uint8_t(icao >> 16);
    frame[2] = uint8_t(icao >> 8);
    frame[3] = uint8_t(icao);
}

}  // namespace

namespace {
uint32_t crc_bytes(const uint8_t* data, int n) {
    const uint32_t poly = 0xFFF409;
    uint32_t crc = 0;
    for (int i = 0; i < n; ++i) {
        crc ^= uint32_t(data[i]) << 16;
        for (int b = 0; b < 8; ++b) crc = crc & 0x800000 ? (crc << 1) ^ poly : crc << 1;
    }
    return crc & 0xFFFFFF;
}
}  // namespace

uint32_t modes_crc(const uint8_t* frame) { return crc_bytes(frame, 14); }

bool AdsbTracker::add(const std::string& line, double now) {
    size_t start = line.find('*');
    if (start == std::string::npos) return false;
    uint8_t f[14];
    for (int i = 0; i < 14; ++i) {
        size_t at = start + 1 + size_t(i) * 2;
        if (at + 1 >= line.size()) return false;   // short (56-bit) frames carry no ADS-B
        int hi = hex_val(line[at]), lo = hex_val(line[at + 1]);
        if (hi < 0 || lo < 0) return false;
        f[i] = uint8_t(hi << 4 | lo);
    }
    if (line.size() <= start + 29 || line[start + 29] != ';') return false;
    int df = f[0] >> 3;
    if ((df != 17 && df != 18) || modes_crc(f) != 0) return false;

    uint32_t icao = uint32_t(f[1]) << 16 | uint32_t(f[2]) << 8 | f[3];
    const uint8_t* me = f + 4;
    uint32_t type = bits(me, 1, 5);

    auto it = std::find_if(list_.begin(), list_.end(), [&](const Aircraft& a) { return a.icao == icao; });
    if (it == list_.end()) {
        if (list_.size() >= 200) return false;
        list_.emplace_back();
        it = list_.end() - 1;
        it->icao = icao;
    }
    Aircraft& a = *it;
    a.last_seen = now;
    ++a.messages;

    if (type >= 1 && type <= 4) {
        std::string cs;
        for (int i = 0; i < 8; ++i) cs += kCharset[bits(me, 9 + i * 6, 14 + i * 6)];
        while (!cs.empty() && cs.back() == ' ') cs.pop_back();
        if (cs.find('#') == std::string::npos) a.callsign = cs;
    } else if (type >= 9 && type <= 18) {
        uint32_t alt = bits(me, 9, 20);
        if (alt & 0x10) {   // 25 ft steps
            a.altitude_ft = int((alt & 0xFE0) >> 1 | (alt & 0xF)) * 25 - 1000;
            a.has_altitude = true;
        }
        int odd = int(bits(me, 22, 22));
        a.cpr_lat[odd] = bits(me, 23, 39);
        a.cpr_lon[odd] = bits(me, 40, 56);
        a.cpr_time[odd] = now;
        // Both halves within 10 s: the aircraft hasn't moved far between them.
        if (std::fabs(a.cpr_time[0] - a.cpr_time[1]) <= 10) locate(a, odd);
    } else if (type == 19) {
        uint32_t sub = bits(me, 6, 8);
        if (sub == 1 || sub == 2) {
            int ew = int(bits(me, 15, 24)), ns = int(bits(me, 26, 35));
            if (ew && ns) {
                double mult = sub == 2 ? 4 : 1;
                double vx = (ew - 1) * mult * (bits(me, 14, 14) ? -1 : 1);
                double vy = (ns - 1) * mult * (bits(me, 25, 25) ? -1 : 1);
                a.speed_kt = std::hypot(vx, vy);
                a.heading_deg = mod(std::atan2(vx, vy) * 180 / M_PI, 360);
                a.has_velocity = true;
            }
        } else if (sub == 3 || sub == 4) {
            int as = int(bits(me, 26, 35));
            if (bits(me, 14, 14) && as) {
                a.heading_deg = bits(me, 15, 24) * 360.0 / 1024;
                a.speed_kt = (as - 1) * (sub == 4 ? 4 : 1);
                a.has_velocity = true;
            }
        }
        int vr = int(bits(me, 38, 46));
        if (vr) a.climb_fpm = (vr - 1) * 64 * (bits(me, 37, 37) ? -1 : 1);
    }
    return true;
}

void AdsbTracker::expire(double now, double max_age) {
    list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const Aircraft& a) { return now - a.last_seen > max_age; }),
                list_.end());
}

std::string adsb_ident_frame(uint32_t icao, const std::string& callsign) {
    uint8_t f[14];
    header(f, icao);
    uint8_t* me = f + 4;
    put(me, 1, 5, 4);
    for (int i = 0; i < 8; ++i) {
        char ch = i < int(callsign.size()) ? callsign[size_t(i)] : ' ';
        const char* p = std::strchr(kCharset, ch);
        put(me, 9 + i * 6, 14 + i * 6, p && ch != '#' ? uint32_t(p - kCharset) : 32);
    }
    return finish(f);
}

std::string adsb_position_frame(uint32_t icao, int altitude_ft, double lat, double lon, bool odd) {
    uint8_t f[14];
    header(f, icao);
    uint8_t* me = f + 4;
    put(me, 1, 5, 11);
    uint32_t n = uint32_t((altitude_ft + 1000) / 25);
    put(me, 9, 20, (n & 0x7F0) << 1 | 0x10 | (n & 0xF));
    put(me, 22, 22, odd);
    double dlat = 360.0 / (odd ? 59 : 60);
    double yz = std::floor(131072 * mod(lat, dlat) / dlat + 0.5);
    double rlat = dlat * (yz / 131072 + std::floor(lat / dlat));
    double dlon = 360.0 / std::max(zones(rlat) - (odd ? 1 : 0), 1);
    double xz = std::floor(131072 * mod(lon, dlon) / dlon + 0.5);
    put(me, 23, 39, uint32_t(mod(yz, 131072)));
    put(me, 40, 56, uint32_t(mod(xz, 131072)));
    return finish(f);
}

std::string adsb_velocity_frame(uint32_t icao, double speed_kt, double heading_deg, int climb_fpm) {
    uint8_t f[14];
    header(f, icao);
    uint8_t* me = f + 4;
    put(me, 1, 5, 19);
    put(me, 6, 8, 1);
    double vx = speed_kt * std::sin(heading_deg * M_PI / 180), vy = speed_kt * std::cos(heading_deg * M_PI / 180);
    put(me, 14, 14, vx < 0);
    put(me, 15, 24, uint32_t(std::lround(std::fabs(vx))) + 1);
    put(me, 25, 25, vy < 0);
    put(me, 26, 35, uint32_t(std::lround(std::fabs(vy))) + 1);
    put(me, 37, 37, climb_fpm < 0);
    put(me, 38, 46, uint32_t(std::abs(climb_fpm) / 64) + 1);
    return finish(f);
}

}  // namespace zrf
