// ADS-B: turn raw Mode S frames (as rtl_adsb and dump1090 --raw print them,
// "*8D4840D6202CC371C32CE0576098;") into a list of aircraft.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zrf {

struct Aircraft {
    uint32_t icao = 0;            // 24-bit airframe address
    std::string callsign;
    bool has_altitude = false, has_velocity = false, has_position = false;
    int altitude_ft = 0;
    double speed_kt = 0, heading_deg = 0;
    int climb_fpm = 0;
    double lat = 0, lon = 0;
    double last_seen = 0;         // caller's clock, seconds
    int messages = 0;
    // The two halves a position is sent in; both are needed to place it.
    uint32_t cpr_lat[2] = {}, cpr_lon[2] = {};
    double cpr_time[2] = {-1e9, -1e9};
};

class AdsbTracker {
public:
    // Feed one line. False if it isn't a valid ADS-B frame (bad checksum,
    // wrong length, another message type). `now` is any steady clock in seconds.
    bool add(const std::string& line, double now);
    // Forget aircraft not heard for max_age seconds.
    void expire(double now, double max_age = 60);
    const std::vector<Aircraft>& aircraft() const { return list_; }

private:
    std::vector<Aircraft> list_;
};

// Remainder of the Mode S checksum over a 14-byte frame; 0 means intact.
uint32_t modes_crc(const uint8_t* frame);

// Build frames (for the simulator and tests). `odd` picks which half of the
// position encoding to send.
std::string adsb_ident_frame(uint32_t icao, const std::string& callsign);
std::string adsb_position_frame(uint32_t icao, int altitude_ft, double lat, double lon, bool odd);
std::string adsb_velocity_frame(uint32_t icao, double speed_kt, double heading_deg, int climb_fpm);

}  // namespace zrf
