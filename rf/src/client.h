// Non-blocking client for the radio daemon, for use from a UI loop.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zrf {

struct SpectrumFrame {
    double freq = 0, rate = 0;
    std::vector<uint8_t> cols;   // see proto.h for the level scale
};

// What the daemon has sent since the last poll.
struct RadioEvents {
    std::vector<SpectrumFrame> frames;
    std::vector<std::string> decoded;   // decoder output lines (JSON from rtl_433)
    std::string error;                  // latest problem reported; "" if none
    bool has_error = false;
};

class RadioClient {
public:
    ~RadioClient() { close(); }
    bool connected() const { return fd_ >= 0; }
    // "" = the default socket path.
    bool connect(const std::string& path = "");
    void close();
    void send(const std::string& line);
    // Collect what the daemon has sent, waiting up to wait_ms for something
    // to arrive. False if the connection is gone.
    bool poll(int wait_ms, RadioEvents& ev);

private:
    int fd_ = -1;
    std::string in_;
};

// Start the daemon in the background. It exits by itself once no client has
// been connected for a while. `source` is a source spec such as "sim".
// The program run is $ZRFD_BIN, or zrfd from PATH.
bool spawn_daemon(const std::string& source, const std::string& socket_path = "");

}  // namespace zrf
