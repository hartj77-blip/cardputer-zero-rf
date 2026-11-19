// Amateur radio logbook in ADIF, the format every logging program and
// Logbook of the World, QRZ and eQSL read and write.
#pragma once

#include <string>
#include <vector>

namespace zrf {

struct Qso {
    std::string call;
    std::string date;       // YYYYMMDD, UTC
    std::string time;       // HHMMSS, UTC
    double freq_mhz = 0;
    std::string mode;       // as the operator sees it: USB, LSB, CW, FM, AM, DIGI
    std::string rst_sent, rst_rcvd;
    std::string comment;
    std::string my_call, my_grid;   // the station that made the contact
};

// ADIF band name for a frequency ("20m", "70cm"), or "" outside the amateur bands.
std::string adif_band(double freq_mhz);

// Text that starts a new log file.
std::string adif_header();

// One contact as a line of ADIF.
std::string adif_record(const Qso& q);

// Every contact in a log. Fields we don't keep are skipped; a contact
// without a callsign is dropped.
std::vector<Qso> adif_parse(const std::string& text);

}  // namespace zrf
