// APRS: amateur radio position and message packets, as multimon-ng prints
// them ("APRS: N0CALL-9>APRS,WIDE1-1:!4903.50N/07201.75W>comment"), and
// Maidenhead grid locators.
#pragma once

#include <string>

namespace zrf {

struct AprsPacket {
    std::string source, destination, path, info;
    std::string kind;       // "position", "message", "status", "weather", "Mic-E", "other"
    std::string text;       // comment, status or message text
    std::string addressee;  // messages only
    bool has_position = false;
    double lat = 0, lon = 0;
};

// False if the line is not a packet.
bool parse_aprs(const std::string& line, AprsPacket& out);

// Centre of a 4- or 6-character grid locator such as "FN31pr". False if malformed.
bool grid_to_latlon(const std::string& grid, double& lat, double& lon);

// Great-circle distance in km and initial bearing in degrees from point 1 to point 2.
void distance_bearing(double lat1, double lon1, double lat2, double lon2, double& km, double& bearing);

// Valid amateur callsign shape (letters and digits with at least one of each,
// optional /suffix or -SSID).
bool looks_like_callsign(const std::string& s);

}  // namespace zrf
