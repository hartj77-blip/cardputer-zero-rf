// Saved channels: frequencies the operator wants to come back to, with a
// name and how to listen. Kept as one tab-separated line each
// (hz, mode, squelch 0/1, name) so the file can be edited by hand.
#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace zrf {

struct Memory {
    double hz = 0;
    std::string mode;     // daemon mode name; "off" = no preference
    bool squelch = false;
    std::string name;
};

// Lines that don't parse are skipped, so a damaged file loses one channel, not all.
inline std::vector<Memory> load_memories(const std::string& path) {
    std::vector<Memory> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
               c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos) continue;
        Memory m;
        char* end = nullptr;
        m.hz = std::strtod(line.c_str(), &end);
        if (end != line.c_str() + a || m.hz < 1e3 || m.hz > 6e9) continue;
        m.mode = line.substr(a + 1, b - a - 1);
        m.squelch = line.substr(b + 1, c - b - 1) == "1";
        m.name = line.substr(c + 1);
        if (m.mode.empty()) continue;
        out.push_back(std::move(m));
    }
    return out;
}

// Written beside the old file and swapped in. False if it could not be written.
inline bool save_memories(const std::string& path, const std::vector<Memory>& list) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "# ZeroOS RF saved channels: Hz, mode, squelch, name (tab-separated)\n";
        for (const Memory& m : list) {
            std::string name = m.name;
            for (char& ch : name)
                if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            char hz[32];
            std::snprintf(hz, sizeof hz, "%.0f", m.hz);
            f << hz << '\t' << m.mode << '\t' << (m.squelch ? 1 : 0) << '\t' << name << '\n';
        }
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace zrf
